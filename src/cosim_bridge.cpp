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
//  Build:
//    c++ -shared -fPIC -o libcosim_bridge.so cosim_bridge.cpp
//

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <utility>

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
      fprintf(stderr, "[a2d] Init '%s' dev_inst=%p\n",
              ctx->sig->name, (void *)op_data);
      return 0;
   }

   if (op != OP_Update)
      return 0;

   void **fns = ctx->fns;
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

         // Deposit into NVC if voltage changed
         if (fabs(v_now - ctx->sig->voltage) > 1e-6) {
            ctx->sig->voltage = v_now;
            if (ctx->sig->deposit_fn)
               ctx->sig->deposit_fn(ctx->sig->deposit_ctx,
                                    v_now, t_now);
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
