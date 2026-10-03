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
//  Start-up: the digital settles its t=0 processes first, so the analog
//  operating point sees the D2A values the digital drives at t=0.
//
//  Finish protocol: once the digital has stopped (std.env.stop/finish, a
//  failure report, a fatal error, an interrupt) the stepper answers every
//  candidate step with COSIM_STEP_FINISH, after at most one veto that cuts
//  the analog step to the digital's stop time; the engine accepts that point
//  and ends its transient, and the engine is never entered again.  Every run
//  ends with exactly one of these lines (backends that classify the end of a
//  run match them):
//    ** Note: co-simulation finished: digital stop at 0 s (before the first analog step)
//    ** Note: co-simulation finished: digital stop at <t> s     (the digital's own time)
//    ** Note: co-simulation finished: analog end at <t> s       (deck stop or --stop-time)
//    ** Error: <engine> transient failed at <t> s
//    ** Error: co-simulation stalled at <t> s
//    ** Error: co-simulation interrupted at <t> s               (SIGINT, the digital's time)
//  unless it fails before the engine runs (a "** Fatal: ..." line: libraries,
//  ABI, boundary file, registration, engine initialisation).
//
//  Times: <t> is a time on the digital's femtosecond clock (an engine time is
//  rounded to it), printed exactly in the style of %g -- the text %.15g gives
//  whenever that is exact ("1.2e-07", "0.002000000006", "0"), with more digits
//  when the count needs them ("3600.000000000000001").  A digital stop is the
//  digital's own stop time; the analog's last point is then the engine's time
//  at the finish, which stopped_step accepts from 1 fs + 1e-14 relative (plus
//  0.5 fs of rounding to the clock) before the stop time on: a check that the
//  analog output reaches the digital stop must allow 2 fs or its own relative
//  tolerance, whichever is larger.
//
//  An interrupt (SIGINT, model_interrupt) stops the digital as a finish does,
//  so the engine still ends its transient (its output is finished), but the
//  run is a failure, never a stop: the interrupted line replaces the
//  digital-stop line and the exit status is COSIM_EXIT_INTERRUPTED (130,
//  128 + SIGINT), whatever the digital's own status.
//
//  ABI: libcosim_bridge.so must export cosim_bridge_abi() and the engine's C
//  interface vacask_cosim_abi() or xyce_cosim_abi(), each returning at least
//  COSIM_BRIDGE_ABI (2: finish protocol, per-boundary ramps, 8192-entry
//  registry).  An engine without the finish protocol reads a finish as an
//  accepted step and would never end, so a missing or older symbol is fatal:
//    ** Fatal: co-simulation ABI mismatch: <library> ...
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
#include "cosim_bridge.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/types.h>

// Verbose co-sim tracing.  Flip to 1 (or build with -DCOSIM_DEBUG=1) to enable;
// 0 lets the compiler dead-strip every trace.  See also cosim_bridge.cpp.
#ifndef COSIM_DEBUG
#define COSIM_DEBUG 0
#endif

// COSIM_TRACE: every analog advance, the scope tree, every boundary binding
static bool trace_on(void)
{
   static int t = -1;
   if (t < 0)
      t = getenv("COSIM_TRACE") != NULL;
   return t == 1;
}

// Bridge function pointers (loaded via dlopen of libcosim_bridge.so); the
// callback types are in cosim_bridge.h
typedef int  (*bridge_register_fn)(const char *name, int dir,
                                    double initial_voltage,
                                    bridge_deposit_fn deposit_fn,
                                    void *deposit_ctx);
typedef int  (*bridge_update_d2a_fn)(int idx, double voltage,
                                      double next_time_s, double now_s);
typedef int  (*bridge_set_ramp_fn)(int idx, double rise, double fall);
typedef void (*bridge_reset_fn)(void);
typedef void (*bridge_set_stepper_fn)(bridge_stepper_fn fn, void *ctx);
typedef int  (*cosim_abi_fn)(void);

static struct {
   void                 *lib;
   bridge_register_fn    reg;
   bridge_update_d2a_fn  update;
   bridge_set_ramp_fn    set_ramp;
   bridge_reset_fn       reset;
   bridge_set_stepper_fn set_stepper;
} bridge;

// The file a loaded symbol comes from, for messages
static const char *lib_file(void *sym, const char *dflt)
{
   Dl_info dli;
   if (sym != NULL && dladdr(sym, &dli) && dli.dli_fname != NULL)
      return dli.dli_fname;
   return dflt;
}

// The ABI handshake: <lib> must export <sym>() >= COSIM_BRIDGE_ABI
static void check_abi(void *lib, const char *sym, void *any_sym,
                      const char *libname, const char *rebuild)
{
   dlerror();
   cosim_abi_fn fn = (cosim_abi_fn)dlsym(lib, sym);
   if (fn == NULL)
      fatal("co-simulation ABI mismatch: %s does not export %s() (it predates "
            "the co-simulation finish protocol); %s",
            lib_file(any_sym, libname), sym, rebuild);

   const int abi = (*fn)();
   if (abi < COSIM_BRIDGE_ABI)
      fatal("co-simulation ABI mismatch: %s has %s() = %d, need %d or "
            "later; %s", lib_file(any_sym, libname), sym, abi,
            COSIM_BRIDGE_ABI, rebuild);
}

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

   check_abi(bridge.lib, "cosim_bridge_abi", (void *)bridge.reg,
             "libcosim_bridge.so", "rebuild it from nvc src/cosim_bridge.cpp "
             "(c++ -O2 -shared -fPIC)");

   bridge.set_ramp = (bridge_set_ramp_fn)dlsym(bridge.lib,
                                                "cosim_bridge_set_ramp");
   if (bridge.set_ramp == NULL)
      fatal("co-simulation ABI mismatch: %s does not export "
            "cosim_bridge_set_ramp(); rebuild it from nvc "
            "src/cosim_bridge.cpp", lib_file((void *)bridge.reg,
                                             "libcosim_bridge.so"));

   notef("loaded libcosim_bridge.so");
   return true;
}

// NVC time is in femtoseconds (1e-15 s)
#define FS_PER_SEC 1e15

// The exit status of an interrupted co-simulation (128 + SIGINT)
#define COSIM_EXIT_INTERRUPTED 130

// Room for any end-line time (see fs_text)
#define TIME_TEXT_MAX 40

// A femtosecond count as an end line prints it: the time in seconds, exact,
// in the style of %g -- the text %.15g gives whenever that is exact (counts
// of at most 15 significant digits: "0", "1.2e-07", "9.95123e-08",
// "0.002000000006"), with as many digits as the count has otherwise
// ("3600.000000000000001").  %.15g alone would round a count of 16 or more
// digits (any time from 1 s on that is not round).
static const char *fs_text(char buf[TIME_TEXT_MAX], int64_t fs)
{
   char digits[24];
   char *p = buf;
   uint64_t u = (uint64_t)fs;
   if (fs < 0) {
      *p++ = '-';
      u = -(uint64_t)fs;
   }
   if (u == 0) {
      strcpy(p, "0");
      return buf;
   }

   const int ndigits = snprintf(digits, sizeof(digits), "%llu",
                                (unsigned long long)u);
   int nsig = ndigits;   // significant digits: trailing zeros dropped
   while (nsig > 1 && digits[nsig - 1] == '0')
      nsig--;

   // The decimal exponent of the first digit in seconds, and %g's choice
   // between the two notations (precision 15, or the digits needed)
   const int x = ndigits - 1 - 15;
   const int prec = nsig > 15 ? nsig : 15;
   if (x < -4 || x >= prec) {
      *p++ = digits[0];
      if (nsig > 1) {
         *p++ = '.';
         memcpy(p, digits + 1, nsig - 1);
         p += nsig - 1;
      }
      snprintf(p, TIME_TEXT_MAX - (p - buf), "e%c%02d", x < 0 ? '-' : '+',
               x < 0 ? -x : x);
   }
   else if (x >= 0) {
      memcpy(p, digits, x + 1);   // the whole seconds (zeros included)
      p += x + 1;
      if (nsig > x + 1) {
         *p++ = '.';
         memcpy(p, digits + x + 1, nsig - x - 1);
         p += nsig - x - 1;
      }
      *p = '\0';
   }
   else {
      *p++ = '0';
      *p++ = '.';
      for (int i = 0; i < -x - 1; i++)
         *p++ = '0';
      memcpy(p, digits, nsig);
      p += nsig;
      *p = '\0';
   }
   return buf;
}

// An engine time for an end line: rounded to the digital's femtosecond clock
// (the co-simulation's time base; an engine's double noise below 1 fs, such
// as 1.50000000000001e-07 for 150 ns, is not part of it) and printed as
// fs_text does.  Beyond the clock (about 9e3 s) or not finite: %.15g.
static const char *time_text(char buf[TIME_TEXT_MAX], double t)
{
   if (isfinite(t) && fabs(t) < 9.0e3)
      return fs_text(buf, (int64_t)llround(t * FS_PER_SEC));
   checked_sprintf(buf, TIME_TEXT_MAX, "%.15g", t);
   return buf;
}

// Engine C interface function pointers (loaded via dlopen).  VACASK's
// libvacaskcinterface mirrors Xyce's libxycecinterface with a vacask_
// prefix; only simulationComplete differs in type: Xyce returns a C++ bool,
// which must not be read through an int (the upper bits of the register are
// garbage, so a failed transient read as complete).
typedef void   (*engine_open_fn)(void **);
typedef int    (*engine_initialize_fn)(void **, int, char **);
typedef int    (*engine_simulateUntil_fn)(void **, double, double *);
typedef void   (*engine_close_fn)(void **);
typedef bool   (*xyce_simulationComplete_fn)(void **);
typedef int    (*vacask_simulationComplete_fn)(void **);
typedef double (*engine_getTime_fn)(void **);

typedef struct {
   void           *lib;     // dlopen handle
   void           *ptr;     // engine instance pointer (passed to all calls)
   cosim_engine_t  kind;

   engine_open_fn                open;
   engine_initialize_fn          initialize;
   engine_simulateUntil_fn       simulateUntil;
   engine_close_fn               close;
   engine_getTime_fn             getTime;
   xyce_simulationComplete_fn    xyce_complete;     // COSIM_XYCE
   vacask_simulationComplete_fn  vacask_complete;   // COSIM_VACASK
} engine_t;

// Boundary signal direction
typedef enum {
   BOUNDARY_D2A,   // NVC digital driver → engine source (via bridge)
   BOUNDARY_A2D,   // engine analog node → NVC receiver deposit
} boundary_dir_t;

// Single boundary mapping
typedef struct {
   boundary_dir_t dir;
   char          *nvc_path;     // NVC hierarchical signal path
   char          *xyce_name;    // bridge signal name (the code: URI's <name>)
   rt_signal_t   *signal;       // resolved NVC signal handle
   int            bridge_idx;   // cosim_bridge index
   double         rise, fall;   // D2A ramp durations (s), < 0 = bridge default
   int            lineno;       // boundary file line
} boundary_t;

// Co-simulation state
typedef struct {
   engine_t       eng;
   boundary_t    *boundaries;
   int            nboundaries;
   rt_model_t    *model;
   int64_t        last_acc_fs;  // last candidate time answered "accept" (fs)
   int64_t        veto_fs;      // digital stop time already vetoed to (fs)
} cosim_state_t;

// Analog engine name (for messages)
static const char *engine_name(cosim_engine_t engine)
{
   return engine == COSIM_VACASK ? "VACASK" : "Xyce";
}

static bool engine_complete(engine_t *e)
{
   if (e->kind == COSIM_VACASK)
      return (*e->vacask_complete)(&e->ptr) != 0;
   else
      return (*e->xyce_complete)(&e->ptr);
}

// Load the analog engine's C-interface shared library via dlopen.
static bool engine_load(engine_t *x, cosim_engine_t engine)
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

   x->kind = engine;

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

#define LOAD_SYM(field, type, name) do { \
      char sym[64]; \
      checked_sprintf(sym, sizeof(sym), "%s%s", prefix, name); \
      x->field = (type)dlsym(x->lib, sym); \
      if (x->field == NULL) { \
         warnf("missing %s symbol %s: %s", engine_name(engine), sym, \
               dlerror()); \
         dlclose(x->lib); \
         x->lib = NULL; \
         return false; \
      } \
   } while (0)

   LOAD_SYM(open, engine_open_fn, "open");
   LOAD_SYM(initialize, engine_initialize_fn, "initialize");
   LOAD_SYM(simulateUntil, engine_simulateUntil_fn, "simulateUntil");
   LOAD_SYM(close, engine_close_fn, "close");
   LOAD_SYM(getTime, engine_getTime_fn, "getTime");
   if (engine == COSIM_VACASK)
      LOAD_SYM(vacask_complete, vacask_simulationComplete_fn,
               "simulationComplete");
   else
      LOAD_SYM(xyce_complete, xyce_simulationComplete_fn,
               "simulationComplete");

#undef LOAD_SYM

   char abi_sym[64];
   checked_sprintf(abi_sym, sizeof(abi_sym), "%scosim_abi", prefix);
   check_abi(x->lib, abi_sym, (void *)x->open,
             engine == COSIM_VACASK ? "libvacaskcinterface.so"
             : "libxycecinterface.so",
             engine == COSIM_VACASK
             ? "rebuild VACASK with the vamos cosim patches (finish protocol)"
             : "rebuild Xyce with the vamos cosim patches (finish protocol)");

   notef("loaded %s C interface", engine_name(engine));
   return true;
}

// --- Boundary configuration ---
//
//   D2A|A2D <nvc path> <bridge name> [rise=<s>] [fall=<s>] [# comment]
//
// Fields are separated by blanks; '#' as the first character of a field
// starts a comment (a line whose first field is a comment is skipped).  The
// direction is case-insensitive, the path is '.'-separated labels relative
// to the top instance (case-insensitive), the bridge name is matched exactly
// against the engine's code: URI.  rise= and fall= (seconds, a C
// floating-point number) set the D2A ramp durations (default 1 ns each); on
// an A2D line they are checked and have no effect.  Lines may be any
// length; a bridge name longer than COSIM_BRIDGE_NAME_MAX, a path longer
// than BOUNDARY_PATH_MAX, a missing field, an unknown column, a repeated
// column or a bad value is an error ("malformed boundary line"), and any
// error ends the run before the engine starts.

#define BOUNDARY_PATH_MAX 4095

// One rise= / fall= value.  NaN, infinities, negative values, trailing junk
// and times beyond NVC's clock are malformed; values below 1 fs are clamped
// to 1 fs with a warning.
static bool parse_ramp(const char *file, int lineno, const char *key,
                       const char *text, double *out)
{
   char *end = NULL;
   const double v = strtod(text, &end);
   if (*text == '\0' || end == text || *end != '\0' || !isfinite(v)
       || v < 0.0 || v > 9.0e3) {
      errorf("%s:%d: malformed boundary line: %s=%s is not a time in seconds "
             "(a finite number >= 0)", file, lineno, key, text);
      return false;
   }

   if (v < COSIM_BRIDGE_MIN_RAMP) {
      warnf("%s:%d: %s=%s is below 1 fs: clamped to %g s", file, lineno, key,
            text, COSIM_BRIDGE_MIN_RAMP);
      *out = COSIM_BRIDGE_MIN_RAMP;
   }
   else
      *out = v;

   return true;
}

static bool parse_boundary_config(cosim_state_t *cs, const char *filename)
{
   FILE *f = fopen(filename, "r");
   if (f == NULL) {
      errorf("cannot open boundary config %s: %s", filename, strerror(errno));
      return false;
   }

   int capacity = 16;
   cs->boundaries = xmalloc_array(capacity, sizeof(boundary_t));
   cs->nboundaries = 0;

   static const char ws[] = " \t\r\n\v\f";
   char *line = NULL;
   size_t linecap = 0;
   ssize_t len;
   int lineno = 0, nerrors = 0;
   while ((len = getline(&line, &linecap, f)) != -1) {
      lineno++;

      if ((size_t)len != strlen(line)) {
         errorf("%s:%d: malformed boundary line: NUL character", filename,
                lineno);
         nerrors++;
         continue;
      }

      char *save = NULL;
      char *tok = strtok_r(line, ws, &save);
      if (tok == NULL || tok[0] == '#')
         continue;   // Blank line or comment

      char *fields[3] = { tok, NULL, NULL };
      int nf = 1;
      while (nf < 3 && (tok = strtok_r(NULL, ws, &save)) != NULL
             && tok[0] != '#')
         fields[nf++] = tok;

      if (nf < 3) {
         errorf("%s:%d: malformed boundary line: expected D2A|A2D <nvc path> "
                "<bridge name>", filename, lineno);
         nerrors++;
         continue;
      }

      boundary_dir_t dir;
      if (strcasecmp(fields[0], "D2A") == 0)
         dir = BOUNDARY_D2A;
      else if (strcasecmp(fields[0], "A2D") == 0)
         dir = BOUNDARY_A2D;
      else {
         errorf("%s:%d: unknown direction '%s' (expected D2A or A2D)",
                filename, lineno, fields[0]);
         nerrors++;
         continue;
      }

      const size_t plen = strlen(fields[1]), nlen = strlen(fields[2]);
      if (plen > BOUNDARY_PATH_MAX) {
         errorf("%s:%d: malformed boundary line: nvc path of %zu characters "
                "exceeds %d", filename, lineno, plen, BOUNDARY_PATH_MAX);
         nerrors++;
         continue;
      }
      if (nlen > COSIM_BRIDGE_NAME_MAX) {
         errorf("%s:%d: malformed boundary line: bridge name of %zu "
                "characters exceeds %d", filename, lineno, nlen,
                COSIM_BRIDGE_NAME_MAX);
         nerrors++;
         continue;
      }

      // Optional key=value columns
      double rise = -1.0, fall = -1.0;
      bool bad = false;
      while (!bad && (tok = strtok_r(NULL, ws, &save)) != NULL) {
         if (tok[0] == '#')
            break;   // Trailing comment

         char *eq = strchr(tok, '=');
         if (eq == NULL || eq == tok) {
            errorf("%s:%d: malformed boundary line: unexpected field '%s' "
                   "(optional columns are rise=<s> fall=<s>)", filename,
                   lineno, tok);
            bad = true;
            break;
         }
         *eq = '\0';

         double *slot;
         if (strcasecmp(tok, "rise") == 0)
            slot = &rise;
         else if (strcasecmp(tok, "fall") == 0)
            slot = &fall;
         else {
            errorf("%s:%d: malformed boundary line: unknown column '%s' "
                   "(optional columns are rise=<s> fall=<s>)", filename,
                   lineno, tok);
            bad = true;
            break;
         }

         if (*slot >= 0.0) {
            errorf("%s:%d: malformed boundary line: %s= given twice",
                   filename, lineno, tok);
            bad = true;
         }
         else if (!parse_ramp(filename, lineno, tok, eq + 1, slot))
            bad = true;
      }

      if (bad) {
         nerrors++;
         continue;
      }

      if (cs->nboundaries >= capacity) {
         capacity *= 2;
         cs->boundaries = xrealloc_array(cs->boundaries, capacity,
                                         sizeof(boundary_t));
      }

      boundary_t *b = &cs->boundaries[cs->nboundaries++];
      b->dir = dir;
      b->nvc_path = xstrdup(fields[1]);
      b->xyce_name = xstrdup(fields[2]);
      b->signal = NULL;
      b->bridge_idx = -1;
      b->rise = rise;
      b->fall = fall;
      b->lineno = lineno;
   }

   if (ferror(f)) {
      errorf("error reading boundary config %s: %s", filename,
             strerror(errno));
      nerrors++;
   }

   free(line);
   fclose(f);

   if (nerrors > 0) {
      errorf("%d error%s in boundary config %s", nerrors,
             nerrors == 1 ? "" : "s", filename);
      return false;
   }

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
                  if (sig != NULL && trace_on())
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
         if (trace_on())
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
      // Only "no value" becomes 0 V: NaN, infinities and real'low/real'high
      // (an uninitialised real); every finite voltage passes through
      if (!isfinite(v) || v == DBL_MAX || v == -DBL_MAX)
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

// Register all boundary signals with the cosim bridge, with their current
// values and ramp times.  Called after the t=0 settle and before engine
// init, so the operating point sees the digital's t=0 values and the
// engine's code: sources find their names.  False if any registration
// failed (every failure is reported).
static bool register_bridges(cosim_state_t *cs, rt_model_t *m,
                             const char *config)
{
   int nfailed = 0;
   for (int i = 0; i < cs->nboundaries; i++) {
      boundary_t *b = &cs->boundaries[i];
      const char *dname = b->dir == BOUNDARY_D2A ? "D2A" : "A2D";

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

      const int dir = (b->dir == BOUNDARY_D2A) ? BRIDGE_D2A : BRIDGE_A2D;
      b->bridge_idx = bridge.reg(b->xyce_name, dir, v0, dep_fn, dep_ctx);
      if (b->bridge_idx < 0) {
         if (++nfailed <= 10) {
            switch (b->bridge_idx) {
            case COSIM_BRIDGE_ERR_FULL:
               errorf("  bridge %s: %s <-> '%s' FAILED (registry full: at "
                      "most %d boundary signals)", dname, b->nvc_path,
                      b->xyce_name, COSIM_BRIDGE_MAX_SIGNALS);
               break;
            case COSIM_BRIDGE_ERR_DUP:
               errorf("%s:%d: duplicate bridge name '%s'", config,
                      b->lineno, b->xyce_name);
               break;
            default:
               errorf("  bridge %s: %s <-> '%s' FAILED (bad bridge name)",
                      dname, b->nvc_path, b->xyce_name);
               break;
            }
         }
         continue;
      }

      if (b->dir == BOUNDARY_D2A && (b->rise >= 0.0 || b->fall >= 0.0)) {
         const double rise =
            b->rise >= 0.0 ? b->rise : COSIM_BRIDGE_DEFAULT_RAMP;
         const double fall =
            b->fall >= 0.0 ? b->fall : COSIM_BRIDGE_DEFAULT_RAMP;
         if (!(*bridge.set_ramp)(b->bridge_idx, rise, fall)) {
            errorf("%s:%d: malformed boundary line: the bridge refused "
                   "rise=%g fall=%g", config, b->lineno, rise, fall);
            nfailed++;
            continue;
         }
      }

      if (trace_on())
         notef("  bridge %s: %s <-> '%s' (V=%.3f)", dname, b->nvc_path,
               b->xyce_name, v0);
   }

   if (nfailed > 10)
      errorf("... and %d more boundary registration failures", nfailed - 10);

   return nfailed == 0;
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

// The answer to a candidate step at T (fs) once the digital has stopped at
// t_f = model_now: accept while T is still before t_f; veto to t_f once,
// when the step ends after t_f and the analog has not yet accepted a point
// at or after it (no engine can shorten a step to its own start, so a stop
// at or before the last accepted point is never vetoed to); otherwise
// finish here, accepting T.
static int stopped_step(cosim_state_t *cs, rt_model_t *m, int64_t T,
                        double *t_evt_s)
{
   const int64_t tf = model_now(m, NULL);
   const int64_t tol = 1 + T / 100000000000000LL;   // 1 fs + 1e-14 relative
   *t_evt_s = -1.0;
   if (T < tf - tol) {
      cs->last_acc_fs = T;
      return COSIM_STEP_ACCEPT;
   }
   if (T > tf + tol && tf > cs->last_acc_fs + tol && cs->veto_fs != tf) {
      cs->veto_fs = tf;
      *t_evt_s = (double)tf / FS_PER_SEC;
      return COSIM_STEP_VETO;
   }
   cs->last_acc_fs = T;
   return COSIM_STEP_FINISH;
}

// Advance the digital through the analog candidate step t_prev_s -> t_s (see
// cosim_bridge.h).  The digital runs one time point at a time -- its own
// events and the probes' interpolated samples -- and stops at the first D2A
// change before t_s; the analog then redoes its step to end there.  A
// stopped digital never runs again: stopped_step answers.
static int cosim_advance(void *ctx, double t_prev_s, double t_s, int nsamples,
                         bridge_apply_fn apply, void *apply_ctx, double *t_evt_s)
{
   cosim_state_t *cs = ctx;
   rt_model_t *m = cs->model;
   const int64_t T0 = (int64_t)llround(t_prev_s * FS_PER_SEC);
   const int64_t T = (int64_t)llround(t_s * FS_PER_SEC);
   advance_ctx_t a = { cs, apply, apply_ctx, -1.0, 0 };

   *t_evt_s = -1.0;

   if (model_stopped(m))
      return stopped_step(cs, m, T, t_evt_s);

   // Step-start values the digital has not seen yet (first step): deposit
   // them at its current time and let it react; a reaction that changes an
   // input means the step must be redone from its start.
   int64_t now = model_now(m, NULL);
   call_with_model(m, advance_apply, &a);
   if (a.count > 0) {
      model_step_to(m, now);
      if (model_stopped(m))
         return stopped_step(cs, m, T, t_evt_s);
      if (update_d2a_bridges(cs, m, now)) {
         *t_evt_s = (double)now / FS_PER_SEC;
         return COSIM_STEP_VETO;
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
      if (model_stopped(m))
         return stopped_step(cs, m, T, t_evt_s);
      if (update_d2a_bridges(cs, m, next)) {
         *t_evt_s = (double)next / FS_PER_SEC;
         return COSIM_STEP_VETO;
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
      cs->last_acc_fs = T;
      return COSIM_STEP_ACCEPT;
   }
   if (model_stopped(m))
      return stopped_step(cs, m, T, t_evt_s);
   update_d2a_bridges(cs, m, T);
   cs->last_acc_fs = T;
   return COSIM_STEP_ACCEPT;
}

// Free co-simulation resources
static void cosim_free(cosim_state_t *cs)
{
   for (int i = 0; i < cs->nboundaries; i++) {
      free(cs->boundaries[i].nvc_path);
      free(cs->boundaries[i].xyce_name);
   }
   free(cs->boundaries);
   cs->boundaries = NULL;
   cs->nboundaries = 0;
   bridge.reset();
}

// Main co-simulation entry point
int cosim_run(rt_model_t *m, cosim_engine_t engine, const char *xyce_netlist,
              const char *xyce_config, uint64_t stop_time)
{
   const char *ename = engine_name(engine);

   cosim_state_t cs = {
      .boundaries  = NULL,
      .nboundaries = 0,
      .model       = NULL,
      .last_acc_fs = 0,
      .veto_fs     = -1,
   };

   // 1. Load shared libraries (each must pass the ABI handshake)
   notef("initializing %s co-simulation", ename);
   if (!bridge_load())
      fatal("failed to load libcosim_bridge.so");
   if (!engine_load(&cs.eng, engine))
      fatal("failed to load the %s C interface library", ename);

   // 2. Parse boundary configuration
   if (xyce_config != NULL && !parse_boundary_config(&cs, xyce_config))
      fatal("failed to parse boundary config: %s", xyce_config);

   // 3. Start NVC simulation (resolvers, initial values), then settle the
   //    digital at t=0 (its own t=0 processes) so the analog operating
   //    point sees the values the digital drives at t=0
   model_run_init(m);
   cs.model = m;
   model_step_to(m, 0);

   // 4. Resolve NVC signal handles
   if (trace_on()) {
      notef("scope tree:");
      dump_scope_tree(root_scope(m), 1);
   }

   if (cs.nboundaries > 0) {
      if (!resolve_boundary_signals(&cs, m))
         warnf("some boundary signals could not be resolved");
   }

   // 5. Register the boundary signals with the bridge (before engine init,
   //    so they're available when the code: sources bind), with their
   //    settled t=0 values
   if (!register_bridges(&cs, m, xyce_config))
      fatal("failed to register the boundary signals with "
            "libcosim_bridge.so");
   update_d2a_bridges(&cs, m, 0);

   // The stepper is in place before the engine starts: VACASK offers its
   // t=0 point (the operating point) to the digital during initialisation
   bridge.set_stepper(cosim_advance, &cs);

   // 6. Initialize the engine — this triggers the code: URI binding
   cs.eng.open(&cs.eng.ptr);

   char *xyce_argv[] = { "Xyce", (char *)xyce_netlist };
   if (cs.eng.initialize(&cs.eng.ptr, 2, xyce_argv) == 0)
      fatal("%s initialize failed for netlist %s", ename, xyce_netlist);
   notef("%s initialized with netlist: %s", ename, xyce_netlist);

   // 7. Run.  The engine's transient is the schedule: it calls back into
   //    the bridge at every converged step, which advances the digital through
   //    cosim_advance.  One simulateUntil(stop) normally covers the whole run;
   //    the loop only re-enters if the engine returns early, and never after
   //    the digital has stopped.  The digital's stop time is its own, never
   //    the engine's.  A stop an interrupt caused is no stop: that run ends
   //    with the interrupted line instead, and fails.
   const double stop_time_s = (double)stop_time / FS_PER_SEC;
   int status = EXIT_SUCCESS;
   bool interrupted = false;
   char tbuf[TIME_TEXT_MAX];

   if (model_stopped(m)) {
      // Stopped during the t=0 settle, or by the operating point
      if ((interrupted = model_interrupted(m)))
         errorf("co-simulation interrupted at %s s",
                fs_text(tbuf, model_now(m, NULL)));
      else
         notef("co-simulation finished: digital stop at 0 s (before the "
               "first analog step)");
   }
   else {
      notef("starting co-simulation (stop_time=%.3g s)", stop_time_s);

      double t = 0.0, prev = 0.0;
      for (int cycle = 0;; cycle++) {
         double r = t;
         const int rc = cs.eng.simulateUntil(&cs.eng.ptr, stop_time_s, &r);
         t = r;
         if (trace_on())
            notef("[cosim] cycle %d: target %.12g reached %.12g%s", cycle,
                  stop_time_s, t, rc == 0 ? " (rc=0)" : "");

         if (model_stopped(m)) {
            fs_text(tbuf, model_now(m, NULL));
            if ((interrupted = model_interrupted(m)))
               errorf("co-simulation interrupted at %s s", tbuf);
            else
               notef("co-simulation finished: digital stop at %s s", tbuf);
            break;
         }

         const bool done = engine_complete(&cs.eng);
         if (rc == 0 && !done) {
            errorf("%s transient failed at %s s", ename, time_text(tbuf, t));
            status = EXIT_FAILURE;
            break;
         }
         if (done || t >= stop_time_s * (1 - 1e-12)) {
            notef("co-simulation finished: analog end at %s s",
                  time_text(tbuf, t));
            // Let the digital finish the last interval
            model_step_to(m, (uint64_t)llround(t * FS_PER_SEC));
            break;
         }
         if (t <= prev) {
            errorf("co-simulation stalled at %s s", time_text(tbuf, t));
            status = EXIT_FAILURE;
            break;
         }
         prev = t;
      }
   }

   // 8. Cleanup
   model_run_fini(m);
   cs.eng.close(&cs.eng.ptr);
   dlclose(cs.eng.lib);
   cosim_free(&cs);
   if (bridge.lib) dlclose(bridge.lib);
   bridge.lib = NULL;

   // An interrupted run fails whatever the digital's status says (an
   // interrupt between processes records none, one inside a process 1)
   if (interrupted)
      return COSIM_EXIT_INTERRUPTED;

   const int drc = model_exit_status(m);
   return drc ? drc : status;
}
