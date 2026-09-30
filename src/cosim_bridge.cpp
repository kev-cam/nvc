//
//  cosim_bridge.cpp — NVC↔Xyce DPWL bridge
//
//  D2A: Xyce DPWL voltage source reads NVC signal voltage via callback.
//    V_in n_in 0 PWL(0 0) URI="code:libcosim_bridge.so:nvc_bridge_init:d2a:name"
//
//  A2D: Xyce DPWL zero-current source detects voltage changes on its branch.
//    When a change is detected, calls through to NVC deposit handle directly.
//    I_out n_out 0 PWL(0 0) URI="code:libcosim_bridge.so:nvc_bridge_init:a2d:name"
//
//  VACASK uses the same registry through its external-source ABI
//  (VACASK include/extsource.h), no function tables involved:
//    v_in (n_in 0) vsource type="pwl" file="code:libcosim_bridge.so:vacask_bridge_init:d2a:name"
//    i_out (n_out 0) isource type="pwl" file="code:libcosim_bridge.so:vacask_bridge_init:a2d:name"
//
//  Build:
//    c++ -shared -fPIC -o libcosim_bridge.so cosim_bridge.cpp
//

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <utility>

// Verbose co-sim tracing.  Flip to 1 (or build with -DCOSIM_DEBUG=1) to enable;
// 0 lets the compiler dead-strip every trace.  See also cosim.c.
#ifndef COSIM_DEBUG
#define COSIM_DEBUG 0
#endif

// A2D voltage resolution: the I-PWL probes predict, from the node's dV/dt, when
// it will next move this many volts ("the next crossing point"); the co-sim
// loop reins its analog timestep in to that time -- so the digital is sampled
// finely where the analog moves and the step free-runs where it is flat. No
// fixed dt_max. Override with COSIM_A2D_DV (volts); default 0.1 V.
static double a2d_dv(void)
{
   static double v = -1.0;
   if (v < 0.0) {
      const char *e = getenv("COSIM_A2D_DV");
      v = (e != nullptr) ? atof(e) : 0.1;
   }
   return v;
}

// Floor on the predicted step-to-crossing (seconds).  Keeps a transient huge
// dV/dt (off Xyce's predictor near a breakpoint) from demanding a sub-ns step
// and spiralling.  Match the Xyce-side getMaxTimeStepSize floor (COSIM_A2D_DTMIN).
static double a2d_dtmin(void)
{
   static double v = -1.0;
   if (v < 0.0) {
      const char *e = getenv("COSIM_A2D_DTMIN");
      v = (e != nullptr) ? atof(e) : 1e-9;
   }
   return v;
}

// Earliest predicted "next significant A2D change" time (absolute seconds)
// across all A2D probes this analog step.  The co-sim loop reads-and-resets it
// to bound its next simulateUntil target.  < 0 => no node moving (free-run).
static double g_a2d_next = -1.0;
extern "C" double cosim_bridge_a2d_next_time(void)
{
   double v = g_a2d_next;
   g_a2d_next = -1.0;
   return v;
}

// --- Signal registry ---

#define MAX_BRIDGE_SIGNALS 256

// Callback type for depositing into NVC (matches cosim_bridge.h)
typedef void (*bridge_deposit_fn)(void *ctx, double voltage, double time_s);

typedef enum { DIR_D2A = 0, DIR_A2D = 1 } sig_dir_t;

struct bridge_signal {
   char              name[256];
   sig_dir_t         dir;
   double            voltage;        // D2A: from NVC; A2D: last seen from Xyce
   double            next_voltage;   // D2A: next scheduled value
   double            next_time_s;    // D2A: next event time, -1 = none
   double            prev_voltage;   // D2A: voltage before last change
   double            transition_start; // D2A: time when transition began
   double            rise_time;      // D2A: rise/fall time for ramps (seconds)
   bridge_deposit_fn deposit_fn;     // A2D: NVC deposit callback
   void             *deposit_ctx;    // A2D: opaque context for callback
   int               in_use;
};

static bridge_signal g_signals[MAX_BRIDGE_SIGNALS];
static int g_nsignals = 0;

static bridge_signal *find_signal(const char *name)
{
   for (int i = 0; i < g_nsignals; i++) {
      if (g_signals[i].in_use && strcmp(g_signals[i].name, name) == 0)
         return &g_signals[i];
   }
   return nullptr;
}

// C API for NVC cosim driver
extern "C" {

int cosim_bridge_register(const char *name, int dir,
                          double initial_voltage,
                          bridge_deposit_fn deposit_fn,
                          void *deposit_ctx)
{
   if (g_nsignals >= MAX_BRIDGE_SIGNALS)
      return -1;

   bridge_signal *s = &g_signals[g_nsignals];
   strncpy(s->name, name, sizeof(s->name) - 1);
   s->name[sizeof(s->name) - 1] = '\0';
   s->dir = (sig_dir_t)dir;
   s->voltage = initial_voltage;
   s->next_voltage = initial_voltage;
   s->prev_voltage = initial_voltage;
   s->transition_start = -1.0;
   s->rise_time = 1e-9;  // 1ns default rise/fall time
   s->next_time_s = -1.0;
   s->deposit_fn = deposit_fn;
   s->deposit_ctx = deposit_ctx;
   s->in_use = 1;

   return g_nsignals++;
}

void cosim_bridge_update_d2a(int idx, double voltage,
                             double next_v, double next_time)
{
   if (idx >= 0 && idx < g_nsignals && g_signals[idx].in_use) {
      bridge_signal *s = &g_signals[idx];
      // Detect voltage change — mark transition start
      if (fabs(voltage - s->voltage) > 1e-6) {
         s->prev_voltage = s->voltage;
         s->transition_start = -2.0;  // flag: set actual time on next callback
      }
      s->voltage = voltage;
      s->next_voltage = next_v;
      s->next_time_s = next_time;
   }
}

void cosim_bridge_reset(void)
{
   for (int i = 0; i < g_nsignals; i++)
      g_signals[i].in_use = 0;
   g_nsignals = 0;
}

} // extern "C"


// --- Xyce DPWL callback interface ---

class PWLinDynData;
class DeviceInstance;

typedef std::vector<std::pair<double, double>> tTVVEC;
typedef const char *c_string;

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

enum { OP_Init = 0, OP_Update = 1 };

// Per-source context
struct bridge_ctx {
   bridge_signal   *sig;
   void           **fns;          // Xyce function pointer table
   DeviceInstance  *dev_inst;     // Xyce device instance (for reading node V)
};


// D2A callback: Xyce reads NVC signal voltage for PWL source
static int d2a_callback(PWLinDynData *pwl, void *ext_data,
                        int op, void *op_data)
{
   bridge_ctx *ctx = (bridge_ctx *)ext_data;
   if (!ctx) return -1;

   if (op == OP_Init) {
      ctx->dev_inst = (DeviceInstance *)op_data;
      fprintf(stderr, "[d2a] Init '%s' dev_inst=%p\n",
              ctx->sig->name, (void *)op_data);
      return 0;
   }

   if (op != OP_Update)
      return 0;

   void **fns = ctx->fns;
   double now = ((fn_get_time_t)fns[FN_GET_TIME])(pwl);
   tTVVEC *tvvec = ((fn_get_tvvec_t)fns[FN_GET_TVVEC])(pwl);

   bridge_signal *sig = ctx->sig;
   double v = sig->voltage;

   // Latch transition start time on first callback after voltage change
   if (sig->transition_start == -2.0)
      sig->transition_start = now;

   tvvec->clear();

   if (sig->transition_start >= 0) {
      // Active ramp transition
      double t0 = sig->transition_start;
      double t1 = t0 + sig->rise_time;

      tvvec->push_back({0.0, sig->prev_voltage});
      tvvec->push_back({t0, sig->prev_voltage});
      tvvec->push_back({t1, v});
      tvvec->push_back({t1 + 1e-6, v});

      // Request breakpoint at end of ramp
      if (t1 > now)
         ((fn_add_break_t)fns[FN_ADD_BREAK])(pwl, t1);

      // Clear transition once ramp is complete
      if (now >= t1)
         sig->transition_start = -1.0;
   }
   else {
      // Steady state
      tvvec->push_back({0.0, v});
      tvvec->push_back({now + 1e-6, v});
   }

   if (sig->next_time_s > now) {
      ((fn_add_break_t)fns[FN_ADD_BREAK])(pwl, sig->next_time_s);
   }

   ((fn_reset_num_t)fns[FN_RESET_NUM])(pwl);
   return 0;
}


// A2D callback: read Xyce node voltage, push to NVC when it changes
static int a2d_callback(PWLinDynData *pwl, void *ext_data,
                        int op, void *op_data)
{
   bridge_ctx *ctx = (bridge_ctx *)ext_data;
   if (!ctx) return -1;

   if (op == OP_Init) {
      ctx->dev_inst = (DeviceInstance *)op_data;
      if (COSIM_DEBUG)
         fprintf(stderr, "[a2d] Init '%s' dev_inst=%p\n",
                 ctx->sig->name, (void *)op_data);
      return 0;
   }

   if (op != OP_Update)
      return 0;

   void **fns = ctx->fns;
   if (COSIM_DEBUG)
      fprintf(stderr, "[a2d] Update '%s'\n", ctx->sig->name);

   // Read node voltage via InstanceGetIsrcV
   // ret[]: 0=currTime, 1=V(pos)_curr, 2=V(neg)_curr,
   //        3=nextTime, 4=V(pos)_next, 5=V(neg)_next
   if (ctx->dev_inst) {
      double ret[6] = {0};
      int sts = ((fn_get_isrc_v_t)fns[FN_INSTANCE_GET_ISRC_V])(
         ctx->dev_inst, ret);
      if (sts) {
         double v_now  = ret[1] - ret[2];
         double t_now  = ret[0];
         double v_next = ret[4] - ret[5];   // Xyce's projected next-step voltage
         double t_next = ret[3];            // ... and its time

         if (COSIM_DEBUG)
            fprintf(stderr, "[a2d-rd] v_now=%.3f t_now=%.4gns v_next=%.3f "
                    "t_next=%.4gns cache=%.3f dep_fn=%p\n",
                    v_now, t_now*1e9, v_next, t_next*1e9,
                    ctx->sig->voltage, (void*)ctx->sig->deposit_fn);

         // Deposit into NVC if voltage changed (Xyce calls this probe on every
         // timestep, so the analog node is sampled each step automatically).
         if (fabs(v_now - ctx->sig->voltage) > 1e-6) {
            ctx->sig->voltage = v_now;
            if (ctx->sig->deposit_fn)
               ctx->sig->deposit_fn(ctx->sig->deposit_ctx,
                                    v_now, t_now);
         }

         // Predict the next "crossing point": from dV/dt, the time for this node
         // to move A2D_DV volts.  Record the earliest such time across all
         // probes so the co-sim loop can rein its next step in to it (the loop
         // target -- not add_break -- is what makes simulateUntil return).  Also
         // drop a breakpoint there so Xyce lands on it.  Flat node -> nothing.
         if (t_next > t_now) {
            double dvdt = fabs(v_next - v_now) / (t_next - t_now);
            if (dvdt > 1e-9) {
               // time for the node to move A2D_DV volts, but never closer than
               // A2D_DTMIN: a tiny prediction (huge transient dV/dt off Xyce's
               // not-yet-converged predictor) would place the breakpoint right
               // on top of currTime and collapse the step.  Small overshoot of
               // the threshold is expected of an analog solver anyway.
               double dt_pred = a2d_dv() / dvdt;
               if (dt_pred < a2d_dtmin()) dt_pred = a2d_dtmin();
               double t_pred = t_now + dt_pred;
               // Keep the earliest prediction across probes, but DISCARD a value
               // the simulation has already advanced past: a2d_callback fires at
               // every analog step, so an early step's t_pred (~start+dt) is
               // stale by the time the call returns.  Without this guard
               // g_a2d_next ends ~= the just-returned time, fails the loop's
               // "a2d_next > xyce_time" test, and the loop free-runs to stop --
               // letting the whole ramp pass in one cycle.  Refresh so it stays
               // ~dt_pred ahead of the latest analog time.
               if (g_a2d_next < 0.0 || t_pred < g_a2d_next || g_a2d_next <= t_now)
                  g_a2d_next = t_pred;
               ((fn_add_break_t)fns[FN_ADD_BREAK])(pwl, t_pred);
            }
         }
      }
   }

   // Zero current — don't disturb the circuit
   double now = ((fn_get_time_t)fns[FN_GET_TIME])(pwl);
   tTVVEC *tvvec = ((fn_get_tvvec_t)fns[FN_GET_TVVEC])(pwl);
   tvvec->clear();
   tvvec->push_back({0.0, 0.0});
   tvvec->push_back({now + 1e-6, 0.0});
   ((fn_reset_num_t)fns[FN_RESET_NUM])(pwl);

   return 0;
}


// Init function — called by Xyce BindCB via dlsym("nvc_bridge_init")
// URI args: "d2a:signal_name" or "a2d:signal_name"
extern "C"
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

   *cb_data = ctx;

   fprintf(stderr, "[cosim_bridge] bound %s DPWL to '%s'\n",
           dir == DIR_D2A ? "D2A" : "A2D", sig_name);

   return (void *)(dir == DIR_D2A ? d2a_callback : a2d_callback);
}


// --- VACASK external-source interface ---
//
// VACASK calls value() at every Newton evaluation (the timepoint may still be
// rejected) and accepted() at every accepted timepoint with v = V(p)-V(n).
// A2D deposits therefore only ever see accepted analog solutions.
// Must match VacaskExtSource in VACASK include/extsource.h.

#define VACASK_EXTSRC_ABI 1

struct VacaskExtSource {
   int abi;
   void *ctx;
   double (*value)(void *ctx, double t, double *next_break);
   int (*accepted)(void *ctx, double t, double v);   // nonzero: pause here
   void (*destroy)(void *ctx);
};

struct vacask_ctx {
   bridge_signal *sig;
   double         t_acc;     // last accepted time
   double         v_acc;     // A2D: voltage at t_acc
   double         v_sync;    // A2D: voltage when this probe last paused
   bool           have_acc;
   // D2A ramp from r_v0 at r_t0 to r_v1 at r_t0 + rise_time.  Kept per source
   // (not in the shared bridge_signal) so that a change arriving while a ramp
   // is still running starts from the value actually being driven -- the
   // source must stay continuous across a pause or the analog step collapses.
   double         r_t0, r_v0, r_v1;
   double         out_acc;   // D2A: output at t_acc
};

static double vacask_d2a_out(const vacask_ctx *ctx, double t)
{
   double t1 = ctx->r_t0 + ctx->sig->rise_time;
   if (t >= t1)
      return ctx->r_v1;
   if (t <= ctx->r_t0)
      return ctx->r_v0;
   return ctx->r_v0 + (ctx->r_v1 - ctx->r_v0) * (t - ctx->r_t0) / (t1 - ctx->r_t0);
}

// D2A: the NVC voltage, reached by a rise_time ramp that starts at the sync
// point where NVC changed it (the last accepted analog time).  Glitches that
// come and go between two sync points leave the target unchanged: no ramp.
static double vacask_d2a_value(void *p, double t, double *next_break)
{
   vacask_ctx *ctx = (vacask_ctx *)p;
   bridge_signal *sig = ctx->sig;
   double nb = 0.0;

   if (fabs(sig->voltage - ctx->r_v1) > 1e-9) {
      ctx->r_v0 = ctx->out_acc;
      ctx->r_t0 = ctx->t_acc;
      ctx->r_v1 = sig->voltage;
   }

   double t1 = ctx->r_t0 + sig->rise_time;
   if (t < t1)
      nb = t1;
   if (sig->next_time_s > t && (nb == 0.0 || sig->next_time_s < nb))
      nb = sig->next_time_s;

   *next_break = nb;
   return vacask_d2a_out(ctx, t);
}

static int vacask_d2a_accepted(void *p, double t, double v)
{
   vacask_ctx *ctx = (vacask_ctx *)p;
   ctx->t_acc = t;
   ctx->out_acc = vacask_d2a_out(ctx, t);
   return 0;
}

// A2D: zero-current probe
static double vacask_a2d_value(void *p, double t, double *next_break)
{
   *next_break = 0.0;
   return 0.0;
}

static int vacask_a2d_accepted(void *p, double t, double v)
{
   vacask_ctx *ctx = (vacask_ctx *)p;
   bridge_signal *sig = ctx->sig;

   // Once the node has moved COSIM_A2D_DV volts since this probe last paused
   // the analog, pause it here so the digital sees the change at this time
   // (VACASK only calls us at accepted points, so this is event-accurate
   // where the dV/dt prediction below cannot be: e.g. a step from a flat node).
   int pause = 0;
   if (!ctx->have_acc)
      ctx->v_sync = v;
   else if (fabs(v - ctx->v_sync) >= a2d_dv()) {
      ctx->v_sync = v;
      pause = 1;
   }

   if (fabs(v - sig->voltage) > 1e-6 || !ctx->have_acc) {
      if (COSIM_DEBUG)
         fprintf(stderr, "[a2d-vc] '%s' V=%.4f @%.4gns\n", sig->name, v, t * 1e9);
      sig->voltage = v;
      if (sig->deposit_fn)
         sig->deposit_fn(sig->deposit_ctx, v, t);
   }

   // Predict the next crossing from the slope between accepted points
   // (same policy as the Xyce probe, see a2d_callback).
   if (ctx->have_acc && t > ctx->t_acc) {
      double dvdt = fabs(v - ctx->v_acc) / (t - ctx->t_acc);
      if (dvdt > 1e-9) {
         double dt_pred = a2d_dv() / dvdt;
         if (dt_pred < a2d_dtmin()) dt_pred = a2d_dtmin();
         double t_pred = t + dt_pred;
         if (g_a2d_next < 0.0 || t_pred < g_a2d_next || g_a2d_next <= t)
            g_a2d_next = t_pred;
      }
   }

   ctx->t_acc = t;
   ctx->v_acc = v;
   ctx->have_acc = true;
   return pause;
}

static void vacask_destroy(void *p)
{
   delete (vacask_ctx *)p;
}

// Init function — called by VACASK via the file="code:..." URI
// URI args: "d2a:signal_name" or "a2d:signal_name"
extern "C"
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
   ctx->t_acc = 0.0;
   ctx->v_acc = 0.0;
   ctx->v_sync = 0.0;
   ctx->have_acc = false;
   // D2A: no ramp pending, drive the registered initial voltage
   ctx->r_t0 = -1.0;
   ctx->r_v0 = ctx->r_v1 = ctx->out_acc = sig->voltage;

   src->ctx = ctx;
   src->value = d2a ? vacask_d2a_value : vacask_a2d_value;
   src->accepted = d2a ? vacask_d2a_accepted : vacask_a2d_accepted;
   src->destroy = vacask_destroy;

   fprintf(stderr, "[cosim_bridge] bound %s VACASK source to '%s'\n",
           d2a ? "D2A" : "A2D", sig_name);
   return 1;
}
