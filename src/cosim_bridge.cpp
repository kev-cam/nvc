//
//  cosim_bridge.cpp — NVC <-> analog engine bridge (Xyce DPWL / VACASK)
//
//  The analog solver is the master scheduler.  Every converged candidate step
//  t_n -> t_{n+1} is offered to the digital before the analog accepts it:
//  the A2D probes stage V(t_n) and V(t_{n+1}), the engine glue calls
//  cosim_bridge_step(t_{n+1}), and the stepper the NVC driver registered
//  advances the digital to t_{n+1}.  If the digital changes an analog input
//  (a D2A source) at t_evt < t_{n+1}, the analog redoes the step so that it
//  ends at t_evt, where the D2A source starts its ramp.  The analog never
//  stops anywhere else.
//
//  Xyce:
//    V_in n_in 0 PWL FILE "code:libcosim_bridge.so:nvc_bridge_init:d2a:name"
//    I_out n_out 0 PWL FILE "code:libcosim_bridge.so:nvc_bridge_init:a2d:name"
//    (Xyce calls xyce_bridge_step at every converged step.)
//  VACASK (external-source ABI, VACASK include/extsource.h):
//    v_in (n_in 0) vsource type="pwl" file="code:libcosim_bridge.so:vacask_bridge_init:d2a:name"
//    i_out (n_out 0) isource type="pwl" file="code:libcosim_bridge.so:vacask_bridge_init:a2d:name"
//    (VACASK calls vacask_extsource_step at every converged step.)
//
//  Build:
//    c++ -O2 -shared -fPIC -o libcosim_bridge.so cosim_bridge.cpp
//

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <utility>

#include "cosim_bridge.h"

// COSIM_TRACE=1: trace vetoes and D2A changes
static bool trace_on(void)
{
   static int t = -1;
   if (t < 0)
      t = getenv("COSIM_TRACE") != nullptr;
   return t == 1;
}

// A2D sampling resolution (volts).  Inside an analog step the digital is fed
// the probes' trajectory (linear between the two ends of the step, which is
// the dV/dt the analog itself assumes) at enough evenly spaced times for no
// sample to move more than this; a threshold crossing is therefore seen
// within this much of its true voltage, whatever the analog step size.  The
// samples cost digital events only.  Override with COSIM_A2D_DV.
static double a2d_dv(void)
{
   static double v = -1.0;
   if (v < 0.0) {
      const char *e = getenv("COSIM_A2D_DV");
      v = (e != nullptr) ? atof(e) : 0.01;
   }
   return v;
}

// Cap on the number of samples per analog step
#define A2D_MAX_SAMPLES 1000

// --- Signal registry ---

#define MAX_BRIDGE_SIGNALS 256

typedef enum { DIR_D2A = 0, DIR_A2D = 1 } sig_dir_t;

struct bridge_signal {
   char              name[256];
   sig_dir_t         dir;
   double            voltage;        // D2A: value from NVC; A2D: last value deposited into NVC
   double            change_time_s;  // D2A: digital time at which NVC last changed it
   double            next_time_s;    // next scheduled digital event, -1 = none
   double            rise_time;      // D2A: rise/fall time of the analog ramp (seconds)
   bridge_deposit_fn deposit_fn;     // A2D: NVC deposit callback
   void             *deposit_ctx;    // A2D: opaque context for callback
   int               in_use;
   // A2D: the candidate step staged by the engine glue
   bool              staged;
   bool              deposited;      // the digital has received a value
   double            st_tprev, st_vprev, st_t, st_v;
};

static bridge_signal g_signals[MAX_BRIDGE_SIGNALS];
static int g_nsignals = 0;

static bridge_stepper_fn g_stepper = nullptr;
static void *g_stepper_ctx = nullptr;

static bridge_signal *find_signal(const char *name)
{
   for (int i = 0; i < g_nsignals; i++) {
      if (g_signals[i].in_use && strcmp(g_signals[i].name, name) == 0)
         return &g_signals[i];
   }
   return nullptr;
}

static void a2d_stage(bridge_signal *s, double tprev, double vprev, double t, double v)
{
   if (s->dir != DIR_A2D)
      return;
   s->st_tprev = tprev;
   s->st_vprev = vprev;
   s->st_t = t;
   s->st_v = v;
   s->staged = true;
}

// Deposit staged A2D values (called by the stepper inside the model).
// frac < 0: at the digital's current time before it advances, a probe the
// digital has never heard from deposits its step-start value (the first step
// after the analog operating point).  0 < frac <= 1: the probes deposit
// their value interpolated at that fraction of the step (frac == 1: the
// candidate value).
static int cosim_apply(void *, double frac)
{
   int n = 0;
   for (int i = 0; i < g_nsignals; i++) {
      bridge_signal *s = &g_signals[i];
      if (!s->in_use || s->dir != DIR_A2D || !s->staged)
         continue;
      double v;
      if (frac < 0.0) {
         if (s->deposited)
            continue;
         v = s->st_vprev;
      }
      else {
         v = s->st_vprev + frac * (s->st_v - s->st_vprev);
         if (s->deposited && fabs(v - s->voltage) <= 1e-9)
            continue;
      }
      s->voltage = v;
      s->deposited = true;
      if (s->deposit_fn)
         s->deposit_fn(s->deposit_ctx, v);
      n++;
   }
   return n;
}

// C API
extern "C" {

int cosim_bridge_register(const char *name, int dir,
                          double initial_voltage,
                          bridge_deposit_fn deposit_fn,
                          void *deposit_ctx)
{
   if (g_nsignals >= MAX_BRIDGE_SIGNALS)
      return -1;

   bridge_signal *s = &g_signals[g_nsignals];
   memset(s, 0, sizeof(*s));
   strncpy(s->name, name, sizeof(s->name) - 1);
   s->dir = (sig_dir_t)dir;
   s->voltage = initial_voltage;
   s->change_time_s = 0.0;
   s->next_time_s = -1.0;
   s->rise_time = 1e-9;  // 1ns default rise/fall time
   s->deposit_fn = deposit_fn;
   s->deposit_ctx = deposit_ctx;
   s->in_use = 1;
   s->staged = false;
   s->deposited = false;

   return g_nsignals++;
}

int cosim_bridge_update_d2a(int idx, double voltage, double next_time_s,
                            double now_s)
{
   if (idx < 0 || idx >= g_nsignals || !g_signals[idx].in_use)
      return 0;
   bridge_signal *s = &g_signals[idx];
   s->next_time_s = next_time_s;
   if (fabs(voltage - s->voltage) <= 1e-9)
      return 0;
   if (trace_on())
      fprintf(stderr, "[cosim] D2A '%s' %.4f -> %.4f V @ %.6g ns\n",
              s->name, s->voltage, voltage, now_s * 1e9);
   s->voltage = voltage;
   s->change_time_s = now_s;
   return 1;
}

void cosim_bridge_reset(void)
{
   for (int i = 0; i < g_nsignals; i++)
      g_signals[i].in_use = 0;
   g_nsignals = 0;
   g_stepper = nullptr;
   g_stepper_ctx = nullptr;
}

void cosim_bridge_set_stepper(bridge_stepper_fn fn, void *ctx)
{
   g_stepper = fn;
   g_stepper_ctx = ctx;
}

int cosim_bridge_step(double t, double *t_evt)
{
   int veto = 0;
   *t_evt = -1.0;

   // Samples the probes need inside this step
   int nsamples = 1;
   double t_prev = t;
   for (int i = 0; i < g_nsignals; i++) {
      bridge_signal *s = &g_signals[i];
      if (!s->in_use || s->dir != DIR_A2D || !s->staged)
         continue;
      t_prev = s->st_tprev;
      double n = ceil(fabs(s->st_v - s->st_vprev) / a2d_dv());
      if (n > nsamples)
         nsamples = n > A2D_MAX_SAMPLES ? A2D_MAX_SAMPLES : (int)n;
   }

   if (g_stepper != nullptr)
      veto = g_stepper(g_stepper_ctx, t_prev, t, nsamples, cosim_apply, nullptr, t_evt);

   for (int i = 0; i < g_nsignals; i++)
      g_signals[i].staged = false;

   if (veto && trace_on())
      fprintf(stderr, "[cosim] step to %.6g ns vetoed: input changed at %.6g ns\n",
              t * 1e9, *t_evt * 1e9);
   return veto;
}

} // extern "C"


// --- D2A ramp ---
//
// Each D2A source ramps to the NVC value over rise_time, starting at the
// digital time NVC changed it (change_time_s): the analog step that is cut
// to end there sees nothing of it, the next step sees the ramp.  The ramp
// state is per source and a new ramp starts from the value the source is
// driving at that moment, so the driven voltage is always continuous.

struct d2a_ramp {
   double t0, v0, v1;   // from v0 at t0 to v1 at t0 + rise_time
};

// Times reported to the analog (ramp ends) are built from whole femtoseconds
// the same way the NVC driver builds event times, so a ramp end and a digital
// event at the same nominal time are the same double.
static double ramp_end(double t0, double rise)
{
   return (double)(llround(t0 * 1e15) + llround(rise * 1e15)) / 1e15;
}

static void d2a_ramp_init(d2a_ramp *r, double v)
{
   r->t0 = -1.0;
   r->v0 = r->v1 = v;
}

static double d2a_ramp_value(const d2a_ramp *r, double rise, double t)
{
   double t1 = ramp_end(r->t0, rise);
   if (t >= t1)
      return r->v1;
   if (t <= r->t0)
      return r->v0;
   return r->v0 + (r->v1 - r->v0) * (t - r->t0) / (t1 - r->t0);
}

// Start a new ramp at the signal's change time if NVC changed the value
static void d2a_ramp_follow(d2a_ramp *r, const bridge_signal *sig)
{
   if (fabs(sig->voltage - r->v1) > 1e-9) {
      r->v0 = d2a_ramp_value(r, sig->rise_time, sig->change_time_s);
      r->t0 = sig->change_time_s;
      r->v1 = sig->voltage;
   }
}


// --- Xyce DPWL callback interface ---

class PWLinDynData;
class DeviceInstance;

typedef std::vector<std::pair<double, double>> tTVVEC;

// Function table indices (N_DEV_SourceDataExt.inc order)
enum {
   FN_GET_TVVEC = 0,
   FN_GET_TIME,
   FN_RESET_NUM,
   FN_REDO_BREAKS,
   FN_GET_SRC_NAME,
   FN_GET_TYP_NAME,
   FN_GET_PRM_NAME,
   FN_ADD_BREAK,
   FN_GET_PARAM,
   FN_INSTANCE_GET_NAME,
   FN_INSTANCE_GET_VSRC_V,
   FN_INSTANCE_GET_ISRC_V,
};

typedef tTVVEC* (*fn_get_tvvec_t)(PWLinDynData *);
typedef double  (*fn_get_time_t)(PWLinDynData *);
typedef void    (*fn_reset_num_t)(PWLinDynData *);
typedef int     (*fn_add_break_t)(PWLinDynData *, double);
typedef int     (*fn_get_isrc_v_t)(DeviceInstance *, double *);

// Bridge operations (Xyce N_DEV_BridgeOp.inc order)
enum { OP_Init = 0, OP_Update = 1 };

// Last point of the PWL tables handed to Xyce.  Xyce turns every table time
// into a breakpoint, so the end of the table must lie beyond any simulation:
// a table ending a fixed interval after the current time made a breakpoint
// per step that interval ahead, and the step density grew without bound.
#define TVVEC_END 1e30

// Per-source context
struct bridge_ctx {
   bridge_signal   *sig;
   void           **fns;          // Xyce function pointer table
   DeviceInstance  *dev_inst;     // Xyce device instance (for reading node V)
   d2a_ramp         ramp;         // D2A
};

static std::vector<bridge_ctx *> g_xyce_ctx;

// D2A callback: the Xyce PWL source follows the NVC value with a ramp.
// Update is called once per step attempt, before the Newton solve; Xyce's
// "time" is then the end of the attempted step.
static int d2a_callback(PWLinDynData *pwl, void *ext_data,
                        int op, void *op_data)
{
   bridge_ctx *ctx = (bridge_ctx *)ext_data;
   if (!ctx) return -1;

   if (op == OP_Init) {
      ctx->dev_inst = (DeviceInstance *)op_data;
      return 0;
   }
   if (op != OP_Update)
      return 0;

   void **fns = ctx->fns;
   double now = ((fn_get_time_t)fns[FN_GET_TIME])(pwl);
   tTVVEC *tvvec = ((fn_get_tvvec_t)fns[FN_GET_TVVEC])(pwl);

   bridge_signal *sig = ctx->sig;
   d2a_ramp *r = &ctx->ramp;
   d2a_ramp_follow(r, sig);

   tvvec->clear();
   double t1 = ramp_end(r->t0, sig->rise_time);
   if (now < t1) {
      // Active ramp transition
      tvvec->push_back({0.0, r->v0});
      tvvec->push_back({r->t0, r->v0});
      tvvec->push_back({t1, r->v1});
      tvvec->push_back({TVVEC_END, r->v1});
      // Land on the end of the ramp
      ((fn_add_break_t)fns[FN_ADD_BREAK])(pwl, t1);
   }
   else {
      // Steady state
      tvvec->push_back({0.0, r->v1});
      tvvec->push_back({TVVEC_END, r->v1});
   }

   // Land on the next scheduled digital event, where this source may change
   if (sig->next_time_s > now)
      ((fn_add_break_t)fns[FN_ADD_BREAK])(pwl, sig->next_time_s);

   ((fn_reset_num_t)fns[FN_RESET_NUM])(pwl);
   return 0;
}

// A2D callback: a zero-current probe.  Its values reach NVC through
// xyce_bridge_step.
static int a2d_callback(PWLinDynData *pwl, void *ext_data,
                        int op, void *op_data)
{
   bridge_ctx *ctx = (bridge_ctx *)ext_data;
   if (!ctx) return -1;

   if (op == OP_Init) {
      ctx->dev_inst = (DeviceInstance *)op_data;
      return 0;
   }
   if (op != OP_Update)
      return 0;

   void **fns = ctx->fns;

   // Zero current — don't disturb the circuit
   tTVVEC *tvvec = ((fn_get_tvvec_t)fns[FN_GET_TVVEC])(pwl);
   tvvec->clear();
   tvvec->push_back({0.0, 0.0});
   tvvec->push_back({TVVEC_END, 0.0});
   ((fn_reset_num_t)fns[FN_RESET_NUM])(pwl);

   return 0;
}

extern "C" {

// Init function — called by Xyce BindCB via dlsym("nvc_bridge_init")
// URI args: "d2a:signal_name" or "a2d:signal_name"
void *nvc_bridge_init(PWLinDynData *pwl, void **cb_data, const char *args)
{
   void **fns = (void **)*cb_data;

   if (!args || !*args) {
      fprintf(stderr, "[cosim_bridge] missing URI args\n");
      return nullptr;
   }

   sig_dir_t dir;
   const char *sig_name;

   if (strncmp(args, "d2a:", 4) == 0) {
      dir = DIR_D2A;
      sig_name = args + 4;
   }
   else if (strncmp(args, "a2d:", 4) == 0) {
      dir = DIR_A2D;
      sig_name = args + 4;
   }
   else {
      fprintf(stderr, "[cosim_bridge] bad URI args '%s'\n", args);
      return nullptr;
   }

   bridge_signal *sig = find_signal(sig_name);
   if (!sig) {
      fprintf(stderr, "[cosim_bridge] signal '%s' not registered\n", sig_name);
      return nullptr;
   }

   bridge_ctx *ctx = new bridge_ctx;
   ctx->sig = sig;
   ctx->fns = fns;
   ctx->dev_inst = nullptr;  // set on Init op
   d2a_ramp_init(&ctx->ramp, sig->voltage);
   g_xyce_ctx.push_back(ctx);

   *cb_data = ctx;

   fprintf(stderr, "[cosim_bridge] bound %s DPWL to '%s'\n",
           dir == DIR_D2A ? "D2A" : "A2D", sig_name);

   return (void *)(dir == DIR_D2A ? d2a_callback : a2d_callback);
}

// Called by Xyce for every converged candidate step, before it is accepted.
// t is the end of the step.  Returns 1 if the step must be redone to end at
// *t_evt (an analog input changed there).
int xyce_bridge_step(double t, double *t_evt)
{
   for (bridge_ctx *ctx : g_xyce_ctx) {
      if (ctx->sig->dir != DIR_A2D || !ctx->dev_inst)
         continue;
      // ret[]: 0=step end time, 1=V(pos) 2=V(neg) at the step start (last
      // accepted), 3=end+h, 4=V(pos) 5=V(neg) at the step end (candidate)
      double ret[6] = {0};
      int sts = ((fn_get_isrc_v_t)ctx->fns[FN_INSTANCE_GET_ISRC_V])(
         ctx->dev_inst, ret);
      if (!(sts & 2))
         continue;
      double tend = ret[0], h = ret[3] - ret[0];
      a2d_stage(ctx->sig, tend - h, ret[1] - ret[2], tend, ret[4] - ret[5]);
   }
   return cosim_bridge_step(t, t_evt);
}

} // extern "C"


// --- VACASK external-source interface ---
//
// Must match VacaskExtSource in VACASK include/extsource.h (ABI 2).
// value() is called at every Newton evaluation; candidate() for every
// converged step before it is accepted; vacask_extsource_step() once per
// converged step after all candidates.

#define VACASK_EXTSRC_ABI 2

struct VacaskExtSource {
   int abi;
   void *ctx;
   double (*value)(void *ctx, double t, double *next_break);
   void (*candidate)(void *ctx, double tprev, double vprev, double t, double v);
   void (*destroy)(void *ctx);
};

struct vacask_ctx {
   bridge_signal *sig;
   d2a_ramp       ramp;      // D2A
};

static double vacask_d2a_value(void *p, double t, double *next_break)
{
   vacask_ctx *ctx = (vacask_ctx *)p;
   bridge_signal *sig = ctx->sig;
   double nb = 0.0;

   d2a_ramp_follow(&ctx->ramp, sig);

   double t1 = ramp_end(ctx->ramp.t0, sig->rise_time);
   if (t < t1)
      nb = t1;
   if (sig->next_time_s > t && (nb == 0.0 || sig->next_time_s < nb))
      nb = sig->next_time_s;

   *next_break = nb;
   return d2a_ramp_value(&ctx->ramp, sig->rise_time, t);
}

// A2D: zero-current probe; its values reach NVC through candidate()
static double vacask_a2d_value(void *, double, double *next_break)
{
   *next_break = 0.0;
   return 0.0;
}

static void vacask_candidate(void *p, double tprev, double vprev, double t, double v)
{
   vacask_ctx *ctx = (vacask_ctx *)p;
   a2d_stage(ctx->sig, tprev, vprev, t, v);
}

static void vacask_destroy(void *p)
{
   delete (vacask_ctx *)p;
}

extern "C" {

// Init function — called by VACASK via the file="code:..." URI
// URI args: "d2a:signal_name" or "a2d:signal_name"
int vacask_bridge_init(const char *args, int is_vsource, VacaskExtSource *src)
{
   if (!src || src->abi != VACASK_EXTSRC_ABI) {
      fprintf(stderr, "[cosim_bridge] VACASK external-source ABI mismatch\n");
      return 0;
   }
   if (!args) args = "";

   bool d2a;
   if (strncmp(args, "d2a:", 4) == 0)
      d2a = true;
   else if (strncmp(args, "a2d:", 4) == 0)
      d2a = false;
   else {
      fprintf(stderr, "[cosim_bridge] bad URI args '%s'\n", args);
      return 0;
   }
   const char *sig_name = args + 4;

   if (d2a && !is_vsource)
      fprintf(stderr, "[cosim_bridge] warning: D2A '%s' bound to an isource\n", sig_name);
   if (!d2a && is_vsource)
      fprintf(stderr, "[cosim_bridge] warning: A2D '%s' bound to a vsource\n", sig_name);

   bridge_signal *sig = find_signal(sig_name);
   if (!sig) {
      fprintf(stderr, "[cosim_bridge] signal '%s' not registered\n", sig_name);
      return 0;
   }

   vacask_ctx *ctx = new vacask_ctx;
   ctx->sig = sig;
   d2a_ramp_init(&ctx->ramp, sig->voltage);

   src->ctx = ctx;
   src->value = d2a ? vacask_d2a_value : vacask_a2d_value;
   src->candidate = vacask_candidate;
   src->destroy = vacask_destroy;

   fprintf(stderr, "[cosim_bridge] bound %s VACASK source to '%s'\n",
           d2a ? "D2A" : "A2D", sig_name);
   return 1;
}

int vacask_extsource_step(double t, double *t_evt)
{
   return cosim_bridge_step(t, t_evt);
}

} // extern "C"
