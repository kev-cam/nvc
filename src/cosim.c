//
//  Copyright (C) 2025  sv2ghdl contributors
//
//  NVC↔Xyce mixed-signal co-simulation driver.
//
//  Xyce is the analog master: it controls timestep advancement.
//  NVC is the digital slave: called to execute events at each analog time.
//
//  D2A boundary: NVC signal value → cosim_bridge → Xyce DPWL source callback.
//  A2D boundary: Xyce DPWL zero-current source detects voltage changes,
//                calls NVC deposit handle directly (push, no polling).
//
//  Both NVC and Xyce dlopen the same libcosim_bridge.so, sharing the
//  global signal registry.  Xyce finds it via DPWL URI; NVC loads it
//  here before Xyce init so signals are registered first.
//

#include "util.h"
#include "diag.h"
#include "ident.h"
#include "option.h"
#include "tree.h"
#include "rt/model.h"
#include "rt/structs.h"
#include "rt/rt.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Bridge function pointers (loaded via dlopen of libcosim_bridge.so)
typedef void (*bridge_deposit_fn)(void *ctx, double voltage, double time_s);

typedef int  (*bridge_register_fn)(const char *name, int dir,
                                    double initial_voltage,
                                    bridge_deposit_fn deposit_fn,
                                    void *deposit_ctx);
typedef void (*bridge_update_d2a_fn)(int idx, double voltage,
                                      double next_v, double next_time);
typedef void (*bridge_reset_fn)(void);

static struct {
   void               *lib;
   bridge_register_fn  reg;
   bridge_update_d2a_fn update;
   bridge_reset_fn     reset;
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

   if (!bridge.reg || !bridge.update || !bridge.reset) {
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
   double         dt_min;       // minimum analog timestep (seconds)
   double         dt_max;       // maximum analog timestep (seconds)
} cosim_state_t;

// Load Xyce shared library via dlopen
static bool xyce_load(xyce_handle_t *x)
{
   const char *libnames[] = {
      "libxycecinterface.so",
      "/usr/local/lib/libxycecinterface.so",
      "/usr/lib/libxycecinterface.so",
      NULL
   };

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
      warnf("cannot load Xyce: %s", dlerror());
      return false;
   }

#define LOAD_SYM(name) do { \
      x->name = (xyce_##name##_fn)dlsym(x->lib, "xyce_" #name); \
      if (x->name == NULL) { \
         warnf("missing Xyce symbol xyce_%s: %s", #name, dlerror()); \
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

   notef("loaded libxycecinterface.so");
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

// A2D deposit callback — called from Xyce DPWL callback inline
static void a2d_deposit(void *ctx, double voltage, double time_s)
{
   a2d_deposit_ctx_t *dc = (a2d_deposit_ctx_t *)ctx;

   if (dc->size == 1) {
      uint8_t sl = (voltage > 0.9) ? 3 : 2;  // std_logic '1' or '0'
      deposit_signal(dc->model, dc->signal, &sl, 0, 1);
   }
   else if (dc->size == sizeof(double)) {
      // Scalar VOLTAGE field from record decomposition
      deposit_signal(dc->model, dc->signal, &voltage, 0, 1);
   }
   else {
      // Full logic3da record: { voltage, resistance, flags }
      struct { double v; double r; int flags; } la = {
         .v = voltage, .r = 50.0, .flags = 0
      };
      deposit_signal(dc->model, dc->signal, &la, 0, 1);
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

// Update all D2A bridge signals from NVC state
static void update_d2a_bridges(cosim_state_t *cs, rt_model_t *m,
                               int64_t next_nvc_event)
{
   double next_time_s = (next_nvc_event > 0)
      ? (double)next_nvc_event / FS_PER_SEC
      : -1.0;

   for (int i = 0; i < cs->nboundaries; i++) {
      boundary_t *b = &cs->boundaries[i];
      if (b->dir != BOUNDARY_D2A || b->signal == NULL || b->bridge_idx < 0)
         continue;

      const void *val = signal_value(b->signal);
      uint8_t sz = signal_size(b->signal);
      double voltage = signal_to_voltage(val, sz);

      // For next_voltage we use the same value — the actual next value
      // isn't known until NVC processes the event.  The breakpoint
      // at next_time_s ensures a re-sync happens then.
      if (getenv("COSIM_DEBUG"))
         fprintf(stderr, "[d2a-upd] %s V=%.3f next_evt=%.3gns\n",
                 b->xyce_name, voltage, next_time_s * 1e9);
      bridge.update(b->bridge_idx, voltage, voltage, next_time_s);
   }
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
int cosim_run(rt_model_t *m, const char *xyce_netlist,
              const char *xyce_config, uint64_t stop_time)
{
   cosim_state_t cs = {
      .boundaries = NULL,
      .nboundaries = 0,
      .dt_min = 1e-12,
      .dt_max = 10e-9,
   };

   // 1. Load shared libraries
   notef("initializing Xyce co-simulation");
   if (!bridge_load()) {
      fatal("failed to load libcosim_bridge.so");
      return EXIT_FAILURE;
   }
   if (!xyce_load(&cs.xyce)) {
      fatal("failed to load libxycecinterface.so");
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
      fatal("xyce_initialize() failed for netlist %s", xyce_netlist);
      cosim_free(&cs);
      return EXIT_FAILURE;
   }
   notef("Xyce initialized with netlist: %s", xyce_netlist);

   // 7. Co-simulation loop
   double xyce_time = 0.0;
   double stop_time_s = (double)stop_time / FS_PER_SEC;
   int cycle = 0;

   notef("starting co-simulation loop (stop_time=%.3g s)", stop_time_s);

   // Event-driven lock-step (analog master, digital evaluated AFTER the analog
   // model eval so it sees the fresh node voltages).  Each cycle:
   //   1. Propose acceptance = xyce_time + dt_max, but PULL IT IN to the next
   //      pending digital event (the next D2A / PWL update) so the analog
   //      stops where the digital is about to change.
   //   2. Push the current D2A values (held across the analog step).
   //   3. Advance/accept Xyce to that time — A2D callbacks deposit the node
   //      voltages into NVC during the solve.
   //   4. Step the digital up to the new analog time: it processes the
   //      just-deposited A2D inputs and schedules its next D2A event, which
   //      caps the next analog step.  (The digital never runs ahead of the
   //      analog, so no A2D deposit is missed.)
   while (xyce_time < stop_time_s) {

      // 1. acceptance time = t + dt_max.  NB we do NOT pull in to the next
      //    *any* digital event: the A2D deposits during an analog ramp
      //    re-trigger digital processes (adc threshold re-eval, etc.) that do
      //    NOT change a D2A output, and capping on those pins the analog step
      //    tiny.  Only a real D2A change matters, and that is resolved within
      //    one dt_max step (the digital catches up in step 4 and the new value
      //    is applied next cycle).  [TODO: exact pull-in needs a per-signal
      //    next-event query to break only on scheduled D2A transitions.]
      double accept = xyce_time + cs.dt_max;
      if (accept > stop_time_s) accept = stop_time_s;
      if (accept <= xyce_time)   accept = xyce_time + cs.dt_min;

      // 2. push current D2A values (held over the step; -1 = no extra breakpoint)
      update_d2a_bridges(&cs, m, -1);

      // 3. advance/accept Xyce
      double actual_time = 0.0;
      rc = cs.xyce.simulateUntil(&cs.xyce.ptr, accept, &actual_time);
      if (rc == 0) {
         if (cs.xyce.simulationComplete(&cs.xyce.ptr))
            notef("Xyce simulation complete at time %.6g s", xyce_time);
         else
            warnf("xyce_simulateUntil(%.6g) failed at cycle %d", accept, cycle);
         break;
      }
      xyce_time = actual_time;

      // 4. evaluate the digital up to the accepted analog time (inclusive)
      model_step_to(m, (uint64_t)(xyce_time * FS_PER_SEC) + 1);

      cycle++;
      if (cycle <= 10 || cycle % 100 == 0)
         notef("cycle %d t=%.3gns", cycle, xyce_time * 1e9);
   }

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
