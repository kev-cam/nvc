//
//  cosim_bridge.cpp — NVC <-> analog engine bridge (Xyce DPWL / VACASK)
//
//  The analog solver is the master scheduler.  Every converged candidate step
//  t_n -> t_{n+1} is offered to the digital before the analog accepts it:
//  the A2D probes stage V(t_n) and V(t_{n+1}), the engine glue calls
//  cosim_bridge_step(t_{n+1}), and the stepper the NVC driver registered
//  advances the digital to t_{n+1}.  If the digital changes an analog input
//  (a D2A source) at t_evt < t_{n+1}, the analog redoes the step so that it
//  ends at t_evt, where the D2A source starts its ramp.  If the digital has
//  stopped, the result is COSIM_STEP_FINISH and the engine accepts the point
//  and ends its transient.  The analog never stops anywhere else.
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
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

#include "cosim_bridge.h"

// COSIM_TRACE=1: trace vetoes, finishes and D2A changes
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

typedef enum { DIR_D2A = BRIDGE_D2A, DIR_A2D = BRIDGE_A2D } sig_dir_t;

static const char *dir_name(sig_dir_t d)
{
   return d == DIR_D2A ? "D2A" : "A2D";
}

struct bridge_signal {
   std::string       name;
   sig_dir_t         dir;
   double            voltage;        // D2A: value from NVC; A2D: last value deposited into NVC
   double            change_time_s;  // D2A: digital time at which NVC last changed it
   double            next_time_s;    // next scheduled digital event, -1 = none
   double            rise, fall;     // D2A: ramp durations up / down (seconds)
   bridge_deposit_fn deposit_fn;     // A2D: NVC deposit callback
   void             *deposit_ctx;    // A2D: opaque context for callback
   // A2D: the candidate step staged by the engine glue
   bool              staged;
   bool              deposited;      // the digital has received a value
   double            st_tprev, st_vprev, st_t, st_v;
   bool              short_warned;   // D2A: a ramp too short was reported
};

// A deque never moves its elements: the engines' sources keep pointers to
// them.  Lookup by name is hashed (up to COSIM_BRIDGE_MAX_SIGNALS entries).
static std::deque<bridge_signal> g_signals;
static std::unordered_map<std::string, int> g_index;
static std::vector<bridge_signal *> g_a2d;    // the A2D entries, in order

static bridge_stepper_fn g_stepper = nullptr;
static void *g_stepper_ctx = nullptr;

// The registered signal a source binds to, or nullptr (with a message) if
// there is none of that direction
static bridge_signal *find_signal(const char *name, sig_dir_t dir)
{
   auto it = g_index.find(name);
   if (it == g_index.end()) {
      fprintf(stderr, "[cosim_bridge] signal '%s' not registered\n", name);
      return nullptr;
   }
   bridge_signal *s = &g_signals[it->second];
   if (s->dir != dir) {
      fprintf(stderr, "[cosim_bridge] signal '%s' not registered as %s "
              "(the boundary file has it as %s)\n", name, dir_name(dir),
              dir_name(s->dir));
      return nullptr;
   }
   return s;
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
   for (bridge_signal *s : g_a2d) {
      if (!s->staged)
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

// A usable ramp time: finite and not negative.  Above ~9000 s the times
// would not fit NVC's femtosecond clock.
static bool ramp_ok(double v)
{
   return std::isfinite(v) && v >= 0.0 && v <= 9.0e3;
}

static double ramp_clamp(double v, const bridge_signal *s, const char *what)
{
   if (v >= COSIM_BRIDGE_MIN_RAMP)
      return v;
   fprintf(stderr, "[cosim_bridge] warning: '%s' %s=%g s clamped to %g s\n",
           s->name.c_str(), what, v, COSIM_BRIDGE_MIN_RAMP);
   return COSIM_BRIDGE_MIN_RAMP;
}

static void xyce_ctx_free(void);

// C API
extern "C" {

int cosim_bridge_abi(void)
{
   return COSIM_BRIDGE_ABI;
}

int cosim_bridge_register(const char *name, int dir,
                          double initial_voltage,
                          bridge_deposit_fn deposit_fn,
                          void *deposit_ctx)
{
   if (name == nullptr || *name == '\0'
       || strlen(name) > COSIM_BRIDGE_NAME_MAX
       || (dir != BRIDGE_D2A && dir != BRIDGE_A2D))
      return COSIM_BRIDGE_ERR_NAME;
   if (g_signals.size() >= COSIM_BRIDGE_MAX_SIGNALS)
      return COSIM_BRIDGE_ERR_FULL;
   if (g_index.count(name) != 0)
      return COSIM_BRIDGE_ERR_DUP;

   g_signals.emplace_back();
   bridge_signal *s = &g_signals.back();
   s->name = name;
   s->dir = (sig_dir_t)dir;
   s->voltage = initial_voltage;
   s->change_time_s = 0.0;
   s->next_time_s = -1.0;
   s->rise = s->fall = COSIM_BRIDGE_DEFAULT_RAMP;
   s->deposit_fn = deposit_fn;
   s->deposit_ctx = deposit_ctx;
   s->staged = false;
   s->deposited = false;
   s->st_tprev = s->st_vprev = s->st_t = s->st_v = 0.0;
   s->short_warned = false;

   const int idx = (int)g_signals.size() - 1;
   g_index.emplace(s->name, idx);
   if (s->dir == DIR_A2D)
      g_a2d.push_back(s);
   return idx;
}

int cosim_bridge_set_ramp(int idx, double rise, double fall)
{
   if (idx < 0 || idx >= (int)g_signals.size())
      return 0;
   bridge_signal *s = &g_signals[idx];
   if (!ramp_ok(rise) || !ramp_ok(fall)) {
      fprintf(stderr, "[cosim_bridge] '%s': bad ramp rise=%g fall=%g "
              "(need finite times >= 0 s)\n", s->name.c_str(), rise, fall);
      return 0;
   }
   s->rise = ramp_clamp(rise, s, "rise");
   s->fall = ramp_clamp(fall, s, "fall");
   return 1;
}

int cosim_bridge_update_d2a(int idx, double voltage, double next_time_s,
                            double now_s)
{
   if (idx < 0 || idx >= (int)g_signals.size())
      return 0;
   bridge_signal *s = &g_signals[idx];
   s->next_time_s = next_time_s;
   if (fabs(voltage - s->voltage) <= 1e-9)
      return 0;
   if (trace_on())
      fprintf(stderr, "[cosim] D2A '%s' %.4f -> %.4f V @ %.6g ns\n",
              s->name.c_str(), s->voltage, voltage, now_s * 1e9);
   s->voltage = voltage;
   s->change_time_s = now_s;
   return 1;
}

// After the engine has been closed: its sources' contexts point into the
// registry, so they go with it
void cosim_bridge_reset(void)
{
   xyce_ctx_free();
   g_a2d.clear();
   g_index.clear();
   g_signals.clear();
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
   int r = COSIM_STEP_ACCEPT;
   *t_evt = -1.0;

   // Samples the probes need inside this step
   int nsamples = 1;
   double t_prev = t;
   for (bridge_signal *s : g_a2d) {
      if (!s->staged)
         continue;
      t_prev = s->st_tprev;
      double n = ceil(fabs(s->st_v - s->st_vprev) / a2d_dv());
      if (n > nsamples)
         nsamples = n > A2D_MAX_SAMPLES ? A2D_MAX_SAMPLES : (int)n;
   }

   if (g_stepper != nullptr)
      r = g_stepper(g_stepper_ctx, t_prev, t, nsamples, cosim_apply, nullptr, t_evt);

   for (bridge_signal *s : g_a2d)
      s->staged = false;

   if (trace_on()) {
      if (r == COSIM_STEP_VETO)
         fprintf(stderr, "[cosim] step to %.6g ns vetoed: input changed at %.6g ns\n",
                 t * 1e9, *t_evt * 1e9);
      else if (r == COSIM_STEP_FINISH)
         fprintf(stderr, "[cosim] step to %.6g ns accepted: the digital has "
                 "stopped, finish\n", t * 1e9);
   }
   return r;
}

} // extern "C"


// --- D2A ramp ---
//
// Each D2A source ramps to the NVC value starting at the digital time NVC
// changed it (change_time_s): the analog step that is cut to end there sees
// nothing of it, the next step sees the ramp.  The ramp state is per source
// and a new ramp starts from the value the source is driving at that moment,
// so the driven voltage is always continuous.  Every ramp takes the signal's
// full rise time when it goes up and its full fall time when it goes down,
// whatever the swing (durations, not slew rates), or the shortest ramp the
// engine resolves at that time if that is longer (d2a_ramp_follow); its end
// time is part of the ramp, so a later change of direction never re-times a
// ramp in flight.  The ramp's end is an analog step boundary on both engines:
// VACASK asks for it as a breakpoint (vacask_d2a_value), Xyce's step across
// it is cut back to it (xyce_ramp_cut).

struct d2a_ramp {
   double t0, v0, v1, t1;   // from v0 at t0 to v1 at t1
};

// Times reported to the analog (ramp ends) are built from whole femtoseconds
// the same way the NVC driver builds event times, so a ramp end and a digital
// event at the same nominal time are the same double.
static double ramp_end(double t0, double dur)
{
   return (double)(llround(t0 * 1e15) + llround(dur * 1e15)) / 1e15;
}

static void d2a_ramp_init(d2a_ramp *r, double v)
{
   r->t0 = r->t1 = -1.0;
   r->v0 = r->v1 = v;
}

// The ramp's value at t.  t <= t0 comes first: the step cut to end at the
// change time sees the old value there.
static double d2a_ramp_value(const d2a_ramp *r, double t)
{
   if (t <= r->t0)
      return r->v0;
   if (t >= r->t1)
      return r->v1;
   return r->v0 + (r->v1 - r->v0) * (t - r->t0) / (r->t1 - r->t0);
}

// Whole femtoseconds: the NVC clock, on which ramp ends are built
static long long fs_of(double t)
{
   return llround(t * 1e15);
}

// The shortest ramp each engine resolves, relative to the time it starts,
// with a margin.  Xyce makes a veto time a breakpoint only beyond 2 x
// minTimeStep = 2 x 2e-14 x t from the step start (StepErrorControl::
// updateMinTimeStep, and the co-simulation veto in N_ANP_Transient.C).
// VACASK ignores a breakpoint within timeRelativeTolerance = 8 x DBL_EPSILON
// x t (1.8e-15 x t) of the point (ExtSource::nextBreakpoint), aborts on a
// shorter step, and after a breakpoint limits the step to tran_fbr (0.25)
// of the distance to the next one: 1e-13 x t keeps those steps 8 times above
// its limit.  Shorter ramps (1 fs from 10 ms on, 10 ps from 100 s on) cannot
// be resolved: the engine would skip the end and spread the change over a
// whole analog step, or fail on the jump ("time step too small").
#define XYCE_MIN_RAMP_REL   1e-13
#define VACASK_MIN_RAMP_REL 1e-13

// Start a new ramp at the signal's change time if NVC changed the value.  A
// ramp shorter than the engine resolves at that time (min_rel x t) is
// stretched to that, with a warning naming the signal (once per signal).
static void d2a_ramp_follow(d2a_ramp *r, bridge_signal *sig, double min_rel,
                            const char *engine)
{
   if (fabs(sig->voltage - r->v1) > 1e-9) {
      const double tc = sig->change_time_s;
      r->v0 = d2a_ramp_value(r, tc);
      r->t0 = tc;
      r->v1 = sig->voltage;
      const double dur = r->v1 >= r->v0 ? sig->rise : sig->fall;
      r->t1 = ramp_end(tc, dur);

      const long long Tc = fs_of(tc);
      const long long Dmin = (long long)ceil(min_rel * (double)Tc);
      if (fs_of(r->t1) - Tc < Dmin) {
         r->t1 = (double)(Tc + Dmin) / 1e15;
         if (!sig->short_warned) {
            sig->short_warned = true;
            fprintf(stderr, "[cosim_bridge] warning: D2A '%s': a %g s ramp at "
                    "%.15g s is shorter than %s resolves at that time; it "
                    "takes %.3g s (reported once per signal)\n",
                    sig->name.c_str(), dur, tc, engine, (double)Dmin / 1e15);
         }
      }
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
   long long        cut_fs;       // D2A: the ramp end a step was last cut to,
   long long        cut_from_fs;  //   and the start of that step (fs)
};

static std::vector<bridge_ctx *> g_xyce_ctx;

// The end of the last step the Xyce glue accepted, which is the start of
// the step Xyce offers next (whole femtoseconds)
static long long g_xyce_acc_fs = 0;

static void xyce_ctx_free(void)
{
   for (bridge_ctx *ctx : g_xyce_ctx)
      delete ctx;
   g_xyce_ctx.clear();
   g_xyce_acc_fs = 0;
}

// D2A callback: the Xyce PWL source follows the NVC value with a ramp.
// Update is called once per step attempt, before the Newton solve; Xyce's
// "time" is then the end of the attempted step.  That step is already
// sized: the first step after a change the digital made at the end of the
// last accepted step was sized before the source saw the change, and it can
// end past the whole ramp (the table below is then the final value, a jump
// across the step).  xyce_ramp_cut cuts such a step back to the ramp's end;
// while a ramp runs at an accepted point, its table times become Xyce
// breakpoints (FN_RESET_NUM), which bound the steps after it.
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
   d2a_ramp_follow(r, sig, XYCE_MIN_RAMP_REL, "Xyce");

   tvvec->clear();
   if (now < r->t1) {
      // Active ramp transition
      tvvec->push_back({0.0, r->v0});
      if (r->t0 > 0.0)
         tvvec->push_back({r->t0, r->v0});
      tvvec->push_back({r->t1, r->v1});
      tvvec->push_back({TVVEC_END, r->v1});
      // Land on the end of the ramp
      ((fn_add_break_t)fns[FN_ADD_BREAK])(pwl, r->t1);
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

// P3 on Xyce: the end of a D2A ramp is an analog step boundary, as on VACASK
// (whose nextBreakpoint asks the source after every accepted point, so it
// sizes the step after a change to end at the ramp's end).  Xyce sizes that
// step before the source has seen the change, so a converged candidate step
// to t that contains the end of a ramp (after the last accepted point,
// before t) is cut to end there: a veto, made before the digital is advanced
// (nothing is undone on the digital side), and Xyce makes the cut time a
// breakpoint and lands on it.  The step from the change to the cut is then
// the ramp itself, and the A2D probes see it as such (a ramp too short for
// Xyce at that time has been stretched to XYCE_MIN_RAMP_REL x t, which it
// lands on).  A ramp end is cut to at most once from one step start: if Xyce
// cannot land on it after all (a ramp ending within 2 x minTimeStep of the
// start), it retries the step from the same start, and that step is
// accepted as it is.  Returns the time to cut to (a ramp end, on the
// femtosecond clock), or -1.
static double xyce_ramp_cut(double t)
{
   const long long T = fs_of(t);
   long long cut = -1;
   const bridge_ctx *first = nullptr;
   for (bridge_ctx *ctx : g_xyce_ctx) {
      if (ctx->sig->dir != DIR_D2A)
         continue;
      d2a_ramp *r = &ctx->ramp;
      d2a_ramp_follow(r, ctx->sig, XYCE_MIN_RAMP_REL, "Xyce");
      if (r->t1 < 0.0)
         continue;   // No change yet
      const long long T1 = fs_of(r->t1);
      if (T1 <= g_xyce_acc_fs || T1 >= T)
         continue;   // Not inside this step
      if (T1 == ctx->cut_fs && g_xyce_acc_fs == ctx->cut_from_fs)
         continue;   // Cut to once from this start: Xyce could not land
      if (cut < 0 || T1 < cut) {
         cut = T1;
         first = ctx;
      }
   }
   if (first == nullptr)
      return -1.0;

   for (bridge_ctx *ctx : g_xyce_ctx) {
      if (ctx->sig->dir == DIR_D2A && ctx->ramp.t1 >= 0.0
          && fs_of(ctx->ramp.t1) == cut) {
         ctx->cut_fs = cut;
         ctx->cut_from_fs = g_xyce_acc_fs;
      }
   }

   if (trace_on())
      fprintf(stderr, "[cosim] step to %.9g ns cut to the end of the D2A "
              "'%s' ramp at %.9g ns\n", t * 1e9, first->sig->name.c_str(),
              (double)cut / 1e6);
   return (double)cut / 1e15;
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

// The callback for a source that could not be bound (bad URI arguments, an
// unregistered name): it reports the URI arguments and fails every
// operation, so Xyce stops at the first one ("Failed to connect URI ...")
// instead of calling through a NULL callback.  Its data is a copy of the
// URI arguments (see bind_fail).
static int bridge_fail(PWLinDynData *, void *ext_data, int op, void *)
{
   if (op == OP_Init)
      fprintf(stderr, "[cosim_bridge] code: source '%s' is not bound to a "
              "boundary signal; failing it\n",
              ext_data ? (const char *)ext_data : "");
   return -1;
}

// The init function's result for a source it cannot bind
static void *bind_fail(void **cb_data, const char *args)
{
   *cb_data = strdup(args ? args : "");
   return (void *)bridge_fail;
}

extern "C" {

// Init function — called by Xyce BindCB via dlsym("nvc_bridge_init")
// URI args: "d2a:signal_name" or "a2d:signal_name"
void *nvc_bridge_init(PWLinDynData *pwl, void **cb_data, const char *args)
{
   void **fns = (void **)*cb_data;

   if (!args || !*args) {
      fprintf(stderr, "[cosim_bridge] missing URI args\n");
      return bind_fail(cb_data, args);
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
      return bind_fail(cb_data, args);
   }

   bridge_signal *sig = find_signal(sig_name, dir);
   if (!sig)
      return bind_fail(cb_data, args);

   bridge_ctx *ctx = new bridge_ctx;
   ctx->sig = sig;
   ctx->fns = fns;
   ctx->dev_inst = nullptr;  // set on Init op
   d2a_ramp_init(&ctx->ramp, sig->voltage);
   ctx->cut_fs = ctx->cut_from_fs = -1;
   g_xyce_ctx.push_back(ctx);

   *cb_data = ctx;

   fprintf(stderr, "[cosim_bridge] bound %s DPWL to '%s'\n",
           dir_name(dir), sig_name);

   return (void *)(dir == DIR_D2A ? d2a_callback : a2d_callback);
}

// Called by Xyce for every converged candidate step, before it is accepted.
// t is the end of the step.  Returns COSIM_STEP_VETO if the step must be
// redone to end at *t_evt (an analog input changed there, or a D2A ramp ends
// there: xyce_ramp_cut), COSIM_STEP_FINISH if the point is to be accepted
// and the transient ended there (the digital has stopped), else
// COSIM_STEP_ACCEPT.
int xyce_bridge_step(double t, double *t_evt)
{
   const double cut = xyce_ramp_cut(t);
   if (cut >= 0.0) {
      *t_evt = cut;
      return COSIM_STEP_VETO;
   }

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
   const int r = cosim_bridge_step(t, t_evt);
   if (r != COSIM_STEP_VETO)
      g_xyce_acc_fs = fs_of(t);
   return r;
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

   d2a_ramp_follow(&ctx->ramp, sig, VACASK_MIN_RAMP_REL, "VACASK");

   if (t < ctx->ramp.t1)
      nb = ctx->ramp.t1;
   if (sig->next_time_s > t && (nb == 0.0 || sig->next_time_s < nb))
      nb = sig->next_time_s;

   *next_break = nb;
   return d2a_ramp_value(&ctx->ramp, t);
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

   bridge_signal *sig = find_signal(sig_name, d2a ? DIR_D2A : DIR_A2D);
   if (!sig)
      return 0;

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

// Called by VACASK once per converged candidate step (ExtSource::preAccept),
// at t = 0 too.  Same results as xyce_bridge_step.
int vacask_extsource_step(double t, double *t_evt)
{
   return cosim_bridge_step(t, t_evt);
}

} // extern "C"
