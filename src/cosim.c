//
//  Copyright (C) 2025  sv2ghdl contributors
//
//  NVC <-> analog (Xyce / VACASK) mixed-signal co-simulation driver.
//
//  The analog solver is the master scheduler.  NVC loads the engine's C
//  interface library and runs its transient with one simulateUntil(stop).
//  At every converged candidate step the engine's bridged sources call back
//  into libcosim_bridge.so, which advances NVC to the candidate time through
//  the stepper registered here (cosim_advance): the digital runs one event
//  time at a time, and if it changes an analog input (a D2A boundary) before
//  the candidate time, the analog redoes its step to end exactly there.
//
//  D2A boundary: NVC signal value -> bridge registry -> engine source ramp.
//  A2D boundary: engine node voltage -> bridge -> NVC deposit, made inside
//                the model at the digital time the analog has reached.
//
//  Both NVC and the engine dlopen the same libcosim_bridge.so, sharing the
//  global signal registry.  The engine finds it via the code: URI; NVC loads
//  it here before engine init so signals are registered first.
//

#include "util.h"
#include "diag.h"
#include "ident.h"
#include "option.h"
#include "tree.h"
#include "rt/model.h"
#include "rt/structs.h"
#include "rt/rt.h"
#include "cosim.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

// Verbose co-sim tracing.  Flip to 1 (or build with -DCOSIM_DEBUG=1) to enable;
// 0 lets the compiler dead-strip every trace.  See also cosim_bridge.cpp.
#ifndef COSIM_DEBUG
#define COSIM_DEBUG 0
#endif

// Bridge function pointers (loaded via dlopen of libcosim_bridge.so),
// see cosim_bridge.h
typedef void (*bridge_deposit_fn)(void *ctx, double voltage);
typedef int  (*bridge_apply_fn)(void *ctx, double frac);
typedef int  (*bridge_stepper_fn)(void *ctx, double t_prev_s, double t_s,
                                  int nsamples, bridge_apply_fn apply,
                                  void *apply_ctx, double *t_evt_s);

typedef int  (*bridge_register_fn)(const char *name, int dir,
                                    double initial_voltage,
                                    bridge_deposit_fn deposit_fn,
                                    void *deposit_ctx);
typedef int  (*bridge_update_d2a_fn)(int idx, double voltage,
                                      double next_time_s, double now_s);
typedef void (*bridge_reset_fn)(void);
typedef void (*bridge_set_stepper_fn)(bridge_stepper_fn fn, void *ctx);

static struct {
   void                 *lib;
   bridge_register_fn    reg;
   bridge_update_d2a_fn  update;
   bridge_reset_fn       reset;
   bridge_set_stepper_fn set_stepper;
} bridge;

static bool bridge_load(void)
{
   const char *names[] = {
      "libcosim_bridge.so",
      NULL
   };

   for (const char **p = names; *p; p++) {
      bridge.lib = dlopen(*p, RTLD_NOW | RTLD_GLOBAL);
      if (bridge.lib) break;
   }

   if (!bridge.lib) {
      warnf("cannot load cosim bridge: %s", dlerror());
      return false;
   }

   bridge.reg    = (bridge_register_fn)dlsym(bridge.lib,
                                              "cosim_bridge_register");
   bridge.update = (bridge_update_d2a_fn)dlsym(bridge.lib,
                                                "cosim_bridge_update_d2a");
   bridge.reset  = (bridge_reset_fn)dlsym(bridge.lib,
                                           "cosim_bridge_reset");
   bridge.set_stepper = (bridge_set_stepper_fn)dlsym(bridge.lib,
                                           "cosim_bridge_set_stepper");

   if (!bridge.reg || !bridge.update || !bridge.reset || !bridge.set_stepper) {
      warnf("cosim bridge missing symbols: %s", dlerror());
      dlclose(bridge.lib);
      bridge.lib = NULL;
      return false;
   }

   notef("loaded libcosim_bridge.so");
   return true;
}

// NVC time is in femtoseconds (1e-15 s)
#define FS_PER_SEC 1e15

// Xyce C interface function pointers (loaded via dlopen)
typedef void   (*xyce_open_fn)(void **);
typedef int    (*xyce_initialize_fn)(void **, int, char **);
typedef int    (*xyce_simulateUntil_fn)(void **, double, double *);
typedef void   (*xyce_close_fn)(void **);
typedef int    (*xyce_simulationComplete_fn)(void **);
typedef double (*xyce_getTime_fn)(void **);

typedef struct {
   void *lib;     // dlopen handle
   void *ptr;     // Xyce instance pointer (passed to all Xyce calls)

   xyce_open_fn                    open;
   xyce_initialize_fn              initialize;
   xyce_simulateUntil_fn           simulateUntil;
   xyce_close_fn                   close;
   xyce_simulationComplete_fn      simulationComplete;
   xyce_getTime_fn                 getTime;
} xyce_handle_t;

// Boundary signal direction
typedef enum {
   BOUNDARY_D2A,   // NVC digital driver → Xyce PWL source (via bridge)
   BOUNDARY_A2D,   // Xyce analog node → NVC receiver deposit
} boundary_dir_t;

// Single boundary mapping
typedef struct {
   boundary_dir_t dir;
   char          *nvc_path;     // NVC hierarchical signal path
   char          *xyce_name;    // Xyce response variable (A2D) or bridge signal name (D2A)
   rt_signal_t   *signal;       // resolved NVC signal handle
   int            bridge_idx;   // cosim_bridge index for D2A signals
} boundary_t;

// Co-simulation state
typedef struct {
   xyce_handle_t xyce;
   boundary_t   *boundaries;
   int            nboundaries;
   rt_model_t    *model;
} cosim_state_t;

// Analog engine name (for messages) and C-interface symbol prefix
static const char *engine_name(cosim_engine_t engine)
{
   return engine == COSIM_VACASK ? "VACASK" : "Xyce";
}

// Load the analog engine's C-interface shared library via dlopen.  VACASK's
// libvacaskcinterface mirrors Xyce's libxycecinterface with a vacask_ prefix.
static bool xyce_load(xyce_handle_t *x, cosim_engine_t engine)
{
   const char *xyce_libnames[] = {
      "libxycecinterface.so",
      "/usr/local/lib/libxycecinterface.so",
      "/usr/lib/libxycecinterface.so",
      NULL
   };
   const char *vacask_libnames[] = {
      "libvacaskcinterface.so",
      "/usr/local/lib/libvacaskcinterface.so",
      "/usr/lib/libvacaskcinterface.so",
      NULL
   };
   const char **libnames =
      engine == COSIM_VACASK ? vacask_libnames : xyce_libnames;
   const char *prefix = engine == COSIM_VACASK ? "vacask_" : "xyce_";

   for (const char **p = libnames; *p != NULL; p++) {
      // RTLD_LAZY (not RTLD_NOW): libXyceLib pulls in the Trilinos amesos2/
      // tpetra chain transitively, and that chain has uninstantiated Kokkos
      // ETI symbols (e.g. KokkosBlas::Impl::NrmInf) that are never called on
      // Xyce's default KLU/basker solver path. The Xyce executable itself
      // loads these lazily and runs fine; RTLD_NOW here would force-resolve
      // them and fail the load. Match Xyce's own lazy binding.
      x->lib = dlopen(*p, RTLD_LAZY | RTLD_GLOBAL);
      if (x->lib != NULL)
         break;
   }

   if (x->lib == NULL) {
      warnf("cannot load %s: %s", engine_name(engine), dlerror());
      return false;
   }

#define LOAD_SYM(name) do { \
      char sym[64]; \
      snprintf(sym, sizeof(sym), "%s%s", prefix, #name); \
      x->name = (xyce_##name##_fn)dlsym(x->lib, sym); \
      if (x->name == NULL) { \
         warnf("missing %s symbol %s: %s", engine_name(engine), sym, \
               dlerror()); \
         dlclose(x->lib); \
         return false; \
      } \
   } while (0)

   LOAD_SYM(open);
   LOAD_SYM(initialize);
   LOAD_SYM(simulateUntil);
   LOAD_SYM(close);
   LOAD_SYM(simulationComplete);
   LOAD_SYM(getTime);

#undef LOAD_SYM

   notef("loaded %s C interface", engine_name(engine));
   return true;
}

// Parse boundary configuration file
//   Format: D2A <nvc_path> <bridge_signal_name>
//           A2D <nvc_path> <xyce_response_var>
//   Lines starting with # are comments
static bool parse_boundary_config(cosim_state_t *cs, const char *filename)
{
   FILE *f = fopen(filename, "r");
   if (f == NULL) {
      warnf("cannot open boundary config: %s", filename);
      return false;
   }

   int capacity = 16;
   cs->boundaries = xmalloc(capacity * sizeof(boundary_t));
   cs->nboundaries = 0;

   char line[1024];
   int lineno = 0;
   while (fgets(line, sizeof(line), f) != NULL) {
      lineno++;

      char *nl = strchr(line, '\n');
      if (nl) *nl = '\0';

      char *p = line;
      while (*p == ' ' || *p == '\t') p++;
      if (*p == '#' || *p == '\0') continue;

      char dir_str[8], nvc_path[512], xyce_name[512];
      if (sscanf(p, "%7s %511s %511s", dir_str, nvc_path, xyce_name) != 3) {
         warnf("%s:%d: malformed boundary line", filename, lineno);
         continue;
      }

      boundary_dir_t dir;
      if (strcasecmp(dir_str, "D2A") == 0)
         dir = BOUNDARY_D2A;
      else if (strcasecmp(dir_str, "A2D") == 0)
         dir = BOUNDARY_A2D;
      else {
         warnf("%s:%d: unknown direction '%s' (expected D2A or A2D)",
               filename, lineno, dir_str);
         continue;
      }

      if (cs->nboundaries >= capacity) {
         capacity *= 2;
         cs->boundaries = xrealloc(cs->boundaries,
                                    capacity * sizeof(boundary_t));
      }

      boundary_t *b = &cs->boundaries[cs->nboundaries++];
      b->dir = dir;
      b->nvc_path = xstrdup(nvc_path);
      b->xyce_name = xstrdup(xyce_name);
      b->signal = NULL;
      b->bridge_idx = -1;
   }

   fclose(f);
   notef("loaded %d boundary mappings from %s", cs->nboundaries, filename);
   return true;
}

// Find child scope by local name
static rt_scope_t *find_child_scope_by_name(rt_scope_t *parent, ident_t name)
{
   for (int i = 0; i < parent->children.count; i++) {
      rt_scope_t *child = parent->children.items[i];
      if (child->where != NULL && tree_ident(child->where) == name)
         return child;
   }
   return NULL;
}

// Find signal in scope by name
static rt_signal_t *find_signal_by_name(rt_scope_t *scope, ident_t name)
{
   for (int i = 0; i < scope->signals.count; i++) {
      rt_signal_t *s = scope->signals.items[i];
      if (tree_ident(s->where) == name)
         return s;
   }
   return NULL;
}

// Debug: dump scope tree
static void dump_scope_tree(rt_scope_t *scope, int depth)
{
   static const char *kind_names[] = {
      "ROOT", "INSTANCE", "PACKAGE", "ARRAY", "RECORD"
   };

   for (int i = 0; i < depth; i++) fprintf(stderr, "  ");
   fprintf(stderr, "[%s] name=%s",
           kind_names[scope->kind],
           scope->where ? istr(tree_ident(scope->where)) : "(null)");
   if (scope->signals.count > 0) {
      fprintf(stderr, " signals={");
      for (int s = 0; s < scope->signals.count; s++) {
         if (s > 0) fprintf(stderr, ",");
         fprintf(stderr, "%s",
                 istr(tree_ident(scope->signals.items[s]->where)));
      }
      fprintf(stderr, "}");
   }
   fprintf(stderr, "\n");

   for (int i = 0; i < scope->children.count; i++)
      dump_scope_tree(scope->children.items[i], depth + 1);
}

// Find top-level entity scope (first non-package child of root)
static rt_scope_t *find_top_scope(rt_scope_t *root)
{
   for (int i = 0; i < root->children.count; i++) {
      rt_scope_t *child = root->children.items[i];
      if (child->kind == SCOPE_INSTANCE)
         return child;
   }
   return root;
}

// Resolve NVC signal handles for all boundary signals
static bool resolve_boundary_signals(cosim_state_t *cs, rt_model_t *m)
{
   rt_scope_t *root = find_top_scope(root_scope(m));
   int resolved = 0;

   for (int i = 0; i < cs->nboundaries; i++) {
      boundary_t *b = &cs->boundaries[i];

      char *path = xstrdup(b->nvc_path);
      rt_scope_t *scope = root;
      rt_signal_t *sig = NULL;

      char *save = NULL;
      char *tok = strtok_r(path + (path[0] == '.' ? 1 : 0), ".", &save);
      char *next = strtok_r(NULL, ".", &save);

      while (tok != NULL && next != NULL) {
         for (char *c = tok; *c; c++) *c = toupper((unsigned char)*c);

         ident_t id = ident_new(tok);
         rt_scope_t *child = find_child_scope_by_name(scope, id);
         if (child == NULL) {
            warnf("cannot find scope '%s' in hierarchy for %s",
                  tok, b->nvc_path);
            break;
         }
         scope = child;
         tok = next;
         next = strtok_r(NULL, ".", &save);
      }

      if (tok != NULL && next == NULL) {
         for (char *c = tok; *c; c++) *c = toupper((unsigned char)*c);

         ident_t sig_id = ident_new(tok);
         sig = find_signal_by_name(scope, sig_id);

         // If not found as a direct signal, check for RECORD child scope
         // (record-type signals like logic3da are decomposed into scopes)
         // and pick the VOLTAGE field from it
         if (sig == NULL) {
            for (int j = 0; j < scope->children.count; j++) {
               rt_scope_t *child = scope->children.items[j];
               if (child->kind == SCOPE_RECORD
                   && child->where != NULL
                   && tree_ident(child->where) == sig_id) {
                  ident_t vid = ident_new("VOLTAGE");
                  sig = find_signal_by_name(child, vid);
                  if (sig != NULL)
                     notef("  resolved %s via RECORD.VOLTAGE", tok);
                  break;
               }
            }
         }
      }

      free(path);

      if (sig != NULL) {
         b->signal = sig;
         resolved++;
         notef("  %s %s <-> %s [OK]",
               b->dir == BOUNDARY_D2A ? "D2A" : "A2D",
               b->nvc_path, b->xyce_name);
      }
      else {
         warnf("  %s %s <-> %s [FAILED: signal not found]",
               b->dir == BOUNDARY_D2A ? "D2A" : "A2D",
               b->nvc_path, b->xyce_name);
      }
   }

   notef("resolved %d/%d boundary signals", resolved, cs->nboundaries);
   return resolved > 0;
}

// Read voltage from NVC signal
// If the signal is the VOLTAGE field of a record, size==8 (one double).
// If it's the full logic3da record, voltage is the first double field.
// If it's std_logic, size==1 (enum).
static double signal_to_voltage(const void *value, uint8_t size)
{
   if (size == 1) {
      uint8_t v = *(const uint8_t *)value;
      // std_logic: 3='1'/7='H' → VDD, 2='0'/6='L' → GND
      switch (v) {
      case 3: case 7: return 1.8;
      case 2: case 6: return 0.0;
      default:        return 0.9;
      }
   }
   // double (VOLTAGE field) or logic3da record (voltage is first field)
   if (size >= sizeof(double)) {
      double v = *(const double *)value;
      // Clamp uninitialized values (±DBL_MAX, NaN) to 0.0
      if (!isfinite(v) || fabs(v) > 1e6)
         return 0.0;
      return v;
   }
   return 0.0;
}

// A2D deposit context: carries model + signal + size for the callback
typedef struct {
   rt_model_t  *model;
   rt_signal_t *signal;
   uint8_t      size;
} a2d_deposit_ctx_t;

// Deposit with event semantics (receivers see S'event in the delta they run
// in).  Defined in rt/model.c for the JIT; it needs the model entered, which
// cosim_advance guarantees for every apply() call.
void x_deposit_signal(sig_shared_t *ss, uint32_t offset, int32_t count,
                      void *values);

// A2D deposit callback — called by the bridge from the stepper's apply()
static void a2d_deposit(void *ctx, double voltage)
{
   a2d_deposit_ctx_t *dc = (a2d_deposit_ctx_t *)ctx;

   if (dc->size == 1) {
      uint8_t sl = (voltage > 0.9) ? 3 : 2;  // std_logic '1' or '0'
      x_deposit_signal(&dc->signal->shared, 0, 1, &sl);
   }
   else if (dc->size == sizeof(double)) {
      // Scalar VOLTAGE field from record decomposition
      x_deposit_signal(&dc->signal->shared, 0, 1, &voltage);
   }
   else {
      // Full logic3da record: { voltage, resistance, flags }
      struct { double v; double r; int flags; } la = {
         .v = voltage, .r = 50.0, .flags = 0
      };
      x_deposit_signal(&dc->signal->shared, 0, 1, &la);
   }
}

// Register all boundary signals with the cosim bridge.
// Called after signal resolution, before Xyce init.
static void register_bridges(cosim_state_t *cs, rt_model_t *m)
{
   for (int i = 0; i < cs->nboundaries; i++) {
      boundary_t *b = &cs->boundaries[i];

      double v0 = 0.0;
      bridge_deposit_fn dep_fn = NULL;
      void *dep_ctx = NULL;

      if (b->dir == BOUNDARY_D2A && b->signal != NULL) {
         const void *val = signal_value(b->signal);
         uint8_t sz = signal_size(b->signal);
         v0 = signal_to_voltage(val, sz);
      }
      else if (b->dir == BOUNDARY_A2D && b->signal != NULL) {
         // Create deposit context for this A2D signal
         a2d_deposit_ctx_t *dc = xmalloc(sizeof(a2d_deposit_ctx_t));
         dc->model = m;
         dc->signal = b->signal;
         dc->size = signal_size(b->signal);
         dep_fn = a2d_deposit;
         dep_ctx = dc;
      }

      int dir = (b->dir == BOUNDARY_D2A) ? 0 : 1;  // BRIDGE_D2A / BRIDGE_A2D
      b->bridge_idx = bridge.reg(b->xyce_name, dir, v0,
                                             dep_fn, dep_ctx);
      if (b->bridge_idx >= 0)
         notef("  bridge %s: %s <-> '%s' (V=%.3f)",
               b->dir == BOUNDARY_D2A ? "D2A" : "A2D",
               b->nvc_path, b->xyce_name, v0);
      else
         warnf("  bridge %s: %s <-> '%s' FAILED (registry full)",
               b->dir == BOUNDARY_D2A ? "D2A" : "A2D",
               b->nvc_path, b->xyce_name);
   }
}

// Give the bridge the D2A signals' current values (and the next scheduled
// digital event time, which the analog sources use as a breakpoint).
// Returns true if any of them changed, i.e. an analog input changed at now.
static bool update_d2a_bridges(cosim_state_t *cs, rt_model_t *m, int64_t now_fs)
{
   int64_t next_evt = model_next_time(m);
   double next_time_s = (next_evt > now_fs && next_evt < TIME_HIGH)
      ? (double)next_evt / FS_PER_SEC : -1.0;
   double now_s = (double)now_fs / FS_PER_SEC;

   bool changed = false;
   for (int i = 0; i < cs->nboundaries; i++) {
      boundary_t *b = &cs->boundaries[i];
      if (b->dir != BOUNDARY_D2A || b->signal == NULL || b->bridge_idx < 0)
         continue;

      const void *val = signal_value(b->signal);
      uint8_t sz = signal_size(b->signal);
      double voltage = signal_to_voltage(val, sz);

      if (bridge.update(b->bridge_idx, voltage, next_time_s, now_s))
         changed = true;
   }
   return changed;
}

// --- The stepper the bridge calls at every converged analog step ---

typedef struct {
   cosim_state_t   *cs;
   bridge_apply_fn  apply;
   void            *apply_ctx;
   double           frac;
   int              count;
} advance_ctx_t;

static void advance_apply(void *arg)
{
   advance_ctx_t *a = arg;
   a->count = (*a->apply)(a->apply_ctx, a->frac);
}

static void advance_timeout(rt_model_t *m, void *arg)
{
   advance_apply(arg);
}

// Advance the digital through the analog candidate step t_prev_s -> t_s (see
// cosim_bridge.h).  The digital runs one time point at a time -- its own
// events and the probes' interpolated samples -- and stops at the first D2A
// change before t_s; the analog then redoes its step to end there.
static int cosim_advance(void *ctx, double t_prev_s, double t_s, int nsamples,
                         bridge_apply_fn apply, void *apply_ctx, double *t_evt_s)
{
   cosim_state_t *cs = ctx;
   rt_model_t *m = cs->model;
   const int64_t T0 = (int64_t)llround(t_prev_s * FS_PER_SEC);
   const int64_t T = (int64_t)llround(t_s * FS_PER_SEC);
   advance_ctx_t a = { cs, apply, apply_ctx, -1.0, 0 };

   // Step-start values the digital has not seen yet (first step): deposit
   // them at its current time and let it react; a reaction that changes an
   // input means the step must be redone from its start.
   int64_t now = model_now(m, NULL);
   call_with_model(m, advance_apply, &a);
   if (a.count > 0) {
      model_step_to(m, now);
      if (update_d2a_bridges(cs, m, now)) {
         *t_evt_s = (double)now / FS_PER_SEC;
         return 1;
      }
   }

   // Time points strictly before T: digital events and interpolated samples
   if (nsamples < 1) nsamples = 1;
   int k = 1;
   for (;;) {
      now = model_now(m, NULL);
      int64_t sample = TIME_HIGH;
      while (k < nsamples) {
         sample = T0 + (int64_t)llround((double)(T - T0) * k / nsamples);
         if (sample > now) break;
         k++;
         sample = TIME_HIGH;
      }
      int64_t next = model_next_time(m);
      if (sample < next) next = sample;
      if (next >= T)
         break;
      if (next == sample) {
         a.frac = (double)(next - T0) / (double)(T - T0);
         model_set_timeout_cb(m, next, advance_timeout, &a);
         k++;
      }
      model_step_to(m, next);      // inclusive: all events and deltas at next
      if (update_d2a_bridges(cs, m, next)) {
         *t_evt_s = (double)next / FS_PER_SEC;
         return 1;
      }
   }

   // The candidate time: deposit the candidate values there, then run the
   // digital at T.  A change at T is not a veto: the source ramps from T.
   now = model_now(m, NULL);
   a.frac = 1.0;
   if (T > now) {
      model_set_timeout_cb(m, T, advance_timeout, &a);
      model_step_to(m, T);
   }
   else if (T == now) {
      call_with_model(m, advance_apply, &a);
      model_step_to(m, T);
   }
   else {
      // The digital is ahead of the analog (the analog is still working its
      // way to a veto time): nothing for it here.
      return 0;
   }
   update_d2a_bridges(cs, m, T);
   return 0;
}

// Free co-simulation resources
static void cosim_free(cosim_state_t *cs)
{
   for (int i = 0; i < cs->nboundaries; i++) {
      free(cs->boundaries[i].nvc_path);
      free(cs->boundaries[i].xyce_name);
   }
   free(cs->boundaries);
   bridge.reset();
}

// Main co-simulation entry point
int cosim_run(rt_model_t *m, cosim_engine_t engine, const char *xyce_netlist,
              const char *xyce_config, uint64_t stop_time)
{
   const char *ename = engine_name(engine);

   cosim_state_t cs = {
      .boundaries = NULL,
      .nboundaries = 0,
      .model = NULL,
   };

   // 1. Load shared libraries
   notef("initializing %s co-simulation", ename);
   if (!bridge_load()) {
      fatal("failed to load libcosim_bridge.so");
      return EXIT_FAILURE;
   }
   if (!xyce_load(&cs.xyce, engine)) {
      fatal("failed to load the %s C interface library", ename);
      return EXIT_FAILURE;
   }

   // 2. Parse boundary configuration
   if (xyce_config != NULL) {
      if (!parse_boundary_config(&cs, xyce_config)) {
         fatal("failed to parse boundary config: %s", xyce_config);
         return EXIT_FAILURE;
      }
   }

   // 3. Start NVC simulation (resolvers, initial values)
   model_run_init(m);

   // 4. Resolve NVC signal handles
   notef("scope tree:");
   dump_scope_tree(root_scope(m), 1);

   if (cs.nboundaries > 0) {
      if (!resolve_boundary_signals(&cs, m))
         warnf("some boundary signals could not be resolved");
   }

   // 5. Register D2A signals with bridge (before Xyce init,
   //    so they're available when DPWL sources call nvc_bridge_init)
   register_bridges(&cs, m);

   // 6. Initialize Xyce — this triggers DPWL URI binding
   cs.xyce.open(&cs.xyce.ptr);

   char *xyce_argv[] = { "Xyce", (char *)xyce_netlist };
   int rc = cs.xyce.initialize(&cs.xyce.ptr, 2, xyce_argv);
   if (rc == 0) {
      fatal("%s initialize failed for netlist %s", ename, xyce_netlist);
      cosim_free(&cs);
      return EXIT_FAILURE;
   }
   notef("%s initialized with netlist: %s", ename, xyce_netlist);

   // 7. Run.  The engine's transient is the schedule: it calls back into
   //    the bridge at every converged step, which advances the digital through
   //    cosim_advance.  One simulateUntil(stop) normally covers the whole run;
   //    the loop only re-enters if the engine returns early.
   double xyce_time = 0.0;
   double stop_time_s = (double)stop_time / FS_PER_SEC;
   int cycle = 0;

   cs.model = m;
   bridge.set_stepper(cosim_advance, &cs);

   // Settle the digital at time zero (its own t=0 processes), then hand the
   // engine the initial D2A values.
   model_step_to(m, 0);
   update_d2a_bridges(&cs, m, 0);

   notef("starting co-simulation (stop_time=%.3g s)", stop_time_s);

   // COSIM_TRACE: every analog advance (from, requested target, reached).
   const bool trace = getenv("COSIM_TRACE") != NULL;

   while (xyce_time < stop_time_s) {
      double actual_time = 0.0;
      rc = cs.xyce.simulateUntil(&cs.xyce.ptr, stop_time_s, &actual_time);
      if (trace)
         notef("[cosim] cycle %d: %.12g -> target %.12g reached %.12g%s", cycle,
               xyce_time, stop_time_s, actual_time, rc == 0 ? " (rc=0)" : "");
      if (rc == 0) {
         if (cs.xyce.simulationComplete(&cs.xyce.ptr))
            notef("%s simulation complete at time %.6g s", ename, xyce_time);
         else
            warnf("%s simulateUntil failed at cycle %d (t=%.6g)", ename, cycle,
                  xyce_time);
         break;
      }
      if (actual_time <= xyce_time) {
         notef("co-simulation stalled at %.6g s (no progress)", xyce_time);
         break;
      }
      xyce_time = actual_time;
      cycle++;
   }

   // Let the digital finish the last interval
   model_step_to(m, (uint64_t)llround(xyce_time * FS_PER_SEC));

   notef("co-simulation complete: %d cycles, final_time=%.6g s",
         cycle, xyce_time);

   // 8. Cleanup
   model_run_fini(m);
   cs.xyce.close(&cs.xyce.ptr);
   dlclose(cs.xyce.lib);
   cosim_free(&cs);
   if (bridge.lib) dlclose(bridge.lib);

   return model_exit_status(m);
}
