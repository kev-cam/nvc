//
//  Copyright (C) 2025-2026  Kevin Cameron
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
//  nvc_cocotb_bridge.c -- Direct bridge between CocoTB and NVC model API.
//

#include "nvc_cocotb_bridge.h"
#include "rt/model.h"
#include "rt/structs.h"
#include "tree.h"
#include "ident.h"
#include "common.h"

#include <Python.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// ---- Handle table ----

typedef enum {
   HDL_SCOPE,    // rt_scope_t*
   HDL_SIGNAL,   // rt_signal_t*
   HDL_ITERATOR, // iteration state
} hdl_kind_t;

typedef struct {
   hdl_kind_t   kind;
   union {
      rt_scope_t  *scope;
      rt_signal_t *signal;
   } u;
   char *name;       // cached name string
   char *fullname;    // cached full path
   int   gpi_type;    // NVCB_MODULE, NVCB_LOGIC_ARRAY, etc.
} nvcb_handle_t;

// Iterator state
typedef struct {
   nvcb_hdl_t parent;
   int        mode;
   int        sig_idx;    // current index in signals array
   int        child_idx;  // current index in children array
   bool       done_signals;
} nvcb_iter_t;

// Callback state
typedef struct {
   nvcb_cb_kind_t kind;
   nvcb_hdl_t     signal;     // for value-change callbacks
   int            edge;       // for value-change callbacks
   uint64_t       time;       // for timed callbacks (absolute)
   bool           fired;
   bool           active;
   rt_watch_t    *watch;      // NVC watch handle (for value-change)
} nvcb_callback_t;

#define MAX_HANDLES    16384
#define MAX_ITERATORS  64
#define MAX_CALLBACKS  16384

static rt_model_t    *g_model = NULL;
static nvcb_handle_t  g_handles[MAX_HANDLES];
static int            g_num_handles = 0;
static nvcb_iter_t    g_iters[MAX_ITERATORS];
static int            g_num_iters = 0;
static nvcb_callback_t g_callbacks[MAX_CALLBACKS];
static int            g_num_callbacks = 0;
static int            g_cb_freelist[MAX_CALLBACKS];
static int            g_cb_free_count = 0;
static bool           g_running = false;
static nvcb_cb_t      g_last_fired = -1;

// Allocate a callback slot, reusing freed ones first
static int alloc_cb_slot(void)
{
   if (g_cb_free_count > 0)
      return g_cb_freelist[--g_cb_free_count];
   if (g_num_callbacks >= MAX_CALLBACKS)
      return -1;
   return g_num_callbacks++;
}

// Mark a callback slot as free
static void free_cb_slot(int id)
{
   if (id < 0 || id >= MAX_CALLBACKS) return;
   g_callbacks[id].active = false;
   g_callbacks[id].fired = false;
   g_cb_freelist[g_cb_free_count++] = id;
}

// Scratch buffer for value conversions
static char g_valbuf[65536];

// Forward declaration
static void run_settling_deltas(void);

// NVC std_logic encoding → ASCII character
static const char nvc_to_char[] = "UX01ZWLH-";
//  0=U, 1=X, 2='0', 3='1', 4=Z, 5=W, 6=L, 7=H, 8='-'

// ASCII character → NVC std_logic encoding
static uint8_t char_to_nvc(char c)
{
   switch (c) {
   case '0': return 2;
   case '1': return 3;
   case 'x': case 'X': return 1;
   case 'z': case 'Z': return 4;
   case 'u': case 'U': return 0;
   case 'w': case 'W': return 5;
   case 'l': case 'L': return 6;
   case 'h': case 'H': return 7;
   case '-': return 8;
   default:  return 1; // X for unknown
   }
}

// ---- Handle helpers ----

static nvcb_hdl_t alloc_scope_handle(rt_scope_t *scope, int gpi_type)
{
   if (g_num_handles >= MAX_HANDLES) return -1;
   nvcb_hdl_t h = g_num_handles++;
   g_handles[h].kind = HDL_SCOPE;
   g_handles[h].u.scope = scope;
   g_handles[h].name = NULL;
   g_handles[h].fullname = NULL;
   g_handles[h].gpi_type = gpi_type;
   return h;
}

static nvcb_hdl_t alloc_signal_handle(rt_signal_t *sig, int gpi_type)
{
   if (g_num_handles >= MAX_HANDLES) return -1;
   nvcb_hdl_t h = g_num_handles++;
   g_handles[h].kind = HDL_SIGNAL;
   g_handles[h].u.signal = sig;
   g_handles[h].name = NULL;
   g_handles[h].fullname = NULL;
   g_handles[h].gpi_type = gpi_type;
   return h;
}

static int classify_signal(rt_signal_t *sig)
{
   uint32_t width = signal_width(sig);
   uint8_t size = signal_size(sig);

   if (size == 8)
      return NVCB_REAL;
   else if (size == 4)
      return NVCB_INTEGER;
   else if (width == 1)
      return NVCB_LOGIC;
   else
      return NVCB_LOGIC_ARRAY;
}

// ---- Hierarchy helpers (from cosim.c pattern) ----

static rt_scope_t *find_child_scope(rt_scope_t *parent, ident_t name)
{
   for (int i = 0; i < parent->children.count; i++) {
      rt_scope_t *child = parent->children.items[i];
      if (child->where != NULL && tree_ident(child->where) == name)
         return child;
   }
   return NULL;
}

static rt_signal_t *find_sig_by_name(rt_scope_t *scope, ident_t name)
{
   for (int i = 0; i < scope->signals.count; i++) {
      rt_signal_t *s = scope->signals.items[i];
      if (tree_ident(s->where) == name)
         return s;
   }
   return NULL;
}

static rt_scope_t *find_top_scope(rt_scope_t *root)
{
   for (int i = 0; i < root->children.count; i++) {
      rt_scope_t *child = root->children.items[i];
      if (child->kind == SCOPE_INSTANCE)
         return child;
   }
   return root;
}

// ---- Public API ----

void nvcb_init(void *model)
{
   g_model = (rt_model_t *)model;
   g_num_handles = 0;
   g_num_iters = 0;
   g_num_callbacks = 0;
   g_cb_free_count = 0;
   g_running = true;
   g_last_fired = -1;
   // Run START_OF_SIMULATION callbacks
   model_run_init(g_model);
}

void nvcb_fini(void)
{
   for (int i = 0; i < g_num_handles; i++) {
      free(g_handles[i].name);
      free(g_handles[i].fullname);
   }
   g_num_handles = 0;
   g_running = false;
   g_model = NULL;
}

nvcb_hdl_t nvcb_get_root(const char *name)
{
   if (!g_model) return -1;

   rt_scope_t *root = root_scope(g_model);
   rt_scope_t *top = find_top_scope(root);

   if (name != NULL && name[0] != '\0') {
      // Try to find named scope under root
      char upper[256];
      snprintf(upper, sizeof(upper), "%s", name);
      for (char *c = upper; *c; c++) *c = toupper((unsigned char)*c);

      ident_t id = ident_new(upper);
      rt_scope_t *found = find_child_scope(root, id);
      if (found) top = found;
   }

   return alloc_scope_handle(top, NVCB_MODULE);
}

nvcb_hdl_t nvcb_get_handle_by_name(nvcb_hdl_t parent, const char *name)
{
   if (parent < 0 || parent >= g_num_handles) return -1;
   nvcb_handle_t *ph = &g_handles[parent];
   if (ph->kind != HDL_SCOPE) return -1;

   rt_scope_t *scope = ph->u.scope;

   // Convert name to uppercase for VHDL case-insensitivity
   char upper[256];
   snprintf(upper, sizeof(upper), "%s", name);
   for (char *c = upper; *c; c++) *c = toupper((unsigned char)*c);

   ident_t id = ident_new(upper);

   // Try signal first
   rt_signal_t *sig = find_sig_by_name(scope, id);
   if (sig)
      return alloc_signal_handle(sig, classify_signal(sig));

   // Try child scope
   rt_scope_t *child = find_child_scope(scope, id);
   if (child) {
      int type = NVCB_MODULE;
      if (child->kind == SCOPE_PACKAGE) type = NVCB_PACKAGE;
      else if (child->kind == SCOPE_RECORD) type = NVCB_STRUCTURE;
      else if (child->kind == SCOPE_ARRAY) type = NVCB_GENARRAY;
      return alloc_scope_handle(child, type);
   }

   return -1;  // not found
}

nvcb_hdl_t nvcb_get_handle_by_index(nvcb_hdl_t parent, int index)
{
   if (parent < 0 || parent >= g_num_handles) return -1;
   nvcb_handle_t *ph = &g_handles[parent];

   if (ph->kind == HDL_SCOPE) {
      rt_scope_t *scope = ph->u.scope;
      if (index >= 0 && index < scope->children.count)
         return alloc_scope_handle(scope->children.items[index], NVCB_MODULE);
   }
   else if (ph->kind == HDL_SIGNAL) {
      // Index into array signal — not directly supported, return the signal
      // with offset info. For now, return -1.
   }

   return -1;
}

// ---- Handle metadata ----

const char *nvcb_get_name(nvcb_hdl_t hdl)
{
   if (hdl < 0 || hdl >= g_num_handles) return "";
   nvcb_handle_t *h = &g_handles[hdl];

   if (h->name) return h->name;

   ident_t id = NULL;
   if (h->kind == HDL_SCOPE && h->u.scope->where)
      id = tree_ident(h->u.scope->where);
   else if (h->kind == HDL_SIGNAL && h->u.signal->where)
      id = tree_ident(h->u.signal->where);

   if (id) {
      const char *s = istr(id);
      h->name = strdup(s);
   }
   else
      h->name = strdup("?");

   return h->name;
}

const char *nvcb_get_fullname(nvcb_hdl_t hdl)
{
   // TODO: build full hierarchical path
   return nvcb_get_name(hdl);
}

int nvcb_get_type(nvcb_hdl_t hdl)
{
   if (hdl < 0 || hdl >= g_num_handles) return NVCB_UNKNOWN;
   return g_handles[hdl].gpi_type;
}

const char *nvcb_get_type_string(nvcb_hdl_t hdl)
{
   switch (nvcb_get_type(hdl)) {
   case NVCB_MODULE:       return "MODULE";
   case NVCB_LOGIC:        return "LOGIC";
   case NVCB_LOGIC_ARRAY:  return "LOGIC_ARRAY";
   case NVCB_REAL:         return "REAL";
   case NVCB_INTEGER:      return "INTEGER";
   case NVCB_ENUM:         return "ENUM";
   case NVCB_STRING:       return "STRING";
   case NVCB_STRUCTURE:    return "STRUCTURE";
   case NVCB_PACKAGE:      return "PACKAGE";
   default:                return "UNKNOWN";
   }
}

int nvcb_get_num_elems(nvcb_hdl_t hdl)
{
   if (hdl < 0 || hdl >= g_num_handles) return 0;
   nvcb_handle_t *h = &g_handles[hdl];

   if (h->kind == HDL_SIGNAL)
      return signal_width(h->u.signal);
   else if (h->kind == HDL_SCOPE)
      return h->u.scope->signals.count + h->u.scope->children.count;

   return 0;
}

bool nvcb_is_const(nvcb_hdl_t hdl)
{
   return false;  // signals are not constants
}

bool nvcb_is_indexable(nvcb_hdl_t hdl)
{
   if (hdl < 0 || hdl >= g_num_handles) return false;
   nvcb_handle_t *h = &g_handles[hdl];
   if (h->kind == HDL_SIGNAL)
      return signal_width(h->u.signal) > 1;
   return false;
}

void nvcb_get_range(nvcb_hdl_t hdl, int *left, int *right, int *dir)
{
   if (hdl < 0 || hdl >= g_num_handles) {
      *left = 0; *right = 0; *dir = NVCB_RANGE_NO_DIR;
      return;
   }
   nvcb_handle_t *h = &g_handles[hdl];
   if (h->kind == HDL_SIGNAL) {
      int w = signal_width(h->u.signal);
      *left = w - 1;
      *right = 0;
      *dir = NVCB_RANGE_DOWN;
   }
   else {
      *left = 0; *right = 0; *dir = NVCB_RANGE_NO_DIR;
   }
}

const char *nvcb_get_definition_name(nvcb_hdl_t hdl)
{
   return nvcb_get_name(hdl);
}

const char *nvcb_get_definition_file(nvcb_hdl_t hdl)
{
   if (hdl < 0 || hdl >= g_num_handles) return "";
   nvcb_handle_t *h = &g_handles[hdl];

   tree_t where = NULL;
   if (h->kind == HDL_SCOPE) where = h->u.scope->where;
   else if (h->kind == HDL_SIGNAL) where = h->u.signal->where;

   if (where) {
      const loc_t *loc = tree_loc(where);
      if (loc && loc->file_ref)
         return loc_file_str(loc);
   }
   return "";
}

// ---- Signal value access ----

const char *nvcb_get_signal_val_binstr(nvcb_hdl_t hdl)
{
   if (hdl < 0 || hdl >= g_num_handles) return "";
   nvcb_handle_t *h = &g_handles[hdl];
   if (h->kind != HDL_SIGNAL) return "";

   rt_signal_t *sig = h->u.signal;
   const uint8_t *data = (const uint8_t *)signal_value(sig);
   uint32_t width = signal_width(sig);
   uint8_t size = signal_size(sig);

   if (size == 1) {
      // std_logic or std_logic_vector: byte-per-bit
      // CocoTB expects MSB first
      if (width > sizeof(g_valbuf) - 1) width = sizeof(g_valbuf) - 1;
      for (uint32_t i = 0; i < width; i++)
         g_valbuf[i] = nvc_to_char[data[width - 1 - i] & 0x0F];
      g_valbuf[width] = '\0';
   }
   else {
      // Integer or other: convert to binary
      snprintf(g_valbuf, sizeof(g_valbuf), "0");
   }

   return g_valbuf;
}

int64_t nvcb_get_signal_val_long(nvcb_hdl_t hdl)
{
   if (hdl < 0 || hdl >= g_num_handles) return 0;
   nvcb_handle_t *h = &g_handles[hdl];
   if (h->kind != HDL_SIGNAL) return 0;

   rt_signal_t *sig = h->u.signal;
   const uint8_t *data = (const uint8_t *)signal_value(sig);
   uint32_t width = signal_width(sig);
   uint8_t size = signal_size(sig);

   int64_t val = 0;
   if (size == 4) {
      int32_t v;
      memcpy(&v, data, 4);
      val = v;
   }
   else if (size == 1) {
      for (int i = (int)width - 1; i >= 0; i--) {
         val <<= 1;
         if (data[i] == 3 || data[i] == 7)
            val |= 1;
      }
   }

   return val;
}

double nvcb_get_signal_val_real(nvcb_hdl_t hdl)
{
   if (hdl < 0 || hdl >= g_num_handles) return 0.0;
   nvcb_handle_t *h = &g_handles[hdl];
   if (h->kind != HDL_SIGNAL) return 0.0;

   rt_signal_t *sig = h->u.signal;
   const uint8_t *data = (const uint8_t *)signal_value(sig);

   if (signal_size(sig) == 8) {
      double val;
      memcpy(&val, data, 8);
      return val;
   }
   return (double)nvcb_get_signal_val_long(hdl);
}

const char *nvcb_get_signal_val_str(nvcb_hdl_t hdl)
{
   return nvcb_get_signal_val_binstr(hdl);
}

void nvcb_set_signal_val_binstr(nvcb_hdl_t hdl, int action, const char *val)
{
   if (hdl < 0 || hdl >= g_num_handles || !g_model) return;
   nvcb_handle_t *h = &g_handles[hdl];
   if (h->kind != HDL_SIGNAL) return;

   rt_signal_t *sig = h->u.signal;
   uint32_t width = signal_width(sig);
   size_t vlen = strlen(val);


   // Allocate byte-per-bit buffer
   uint8_t *buf = alloca(width);

   // CocoTB sends MSB first, NVC stores LSB first
   for (uint32_t i = 0; i < width; i++) {
      if (i < vlen)
         buf[width - 1 - i] = char_to_nvc(val[i]);
      else
         buf[width - 1 - i] = 2;  // pad with '0'
   }

   if (action == NVCB_RELEASE) {
      release_signal(g_model, sig, 0, width);
   } else {
      sched_deposit(g_model, sig, buf, 0, width, 0, false);
   }
}

// Run delta cycles until pending deposits settle.
// Called after each set_signal_val* — applies the deposit and runs
// any combinational logic that depends on the changed signal.
// We do NOT advance time here, only run delta cycles.
static void run_settling_deltas(void)
{
   if (!g_model) return;
   // model_step runs one delta cycle (or advances time if delta-stable).
   // Run a few steps until either time advances or we've settled.
   unsigned d;
   int64_t start = model_now(g_model, &d);
   unsigned start_iter = d;
   for (int i = 0; i < 32; i++) {
      if (model_step(g_model)) {
         g_running = false;
         break;
      }
      int64_t now = model_now(g_model, &d);
      // Stop when we've moved past the current delta cycle
      // (either time advanced, or delta count went up and back to 0)
      if (now > start || (now == start && d == 0 && start_iter > 0))
         break;
   }
}

void nvcb_set_signal_val_int(nvcb_hdl_t hdl, int action, int64_t val)
{
   if (hdl < 0 || hdl >= g_num_handles || !g_model) return;
   nvcb_handle_t *h = &g_handles[hdl];
   if (h->kind != HDL_SIGNAL) return;

   rt_signal_t *sig = h->u.signal;
   uint32_t width = signal_width(sig);
   uint8_t size = signal_size(sig);

   // sched_deposit is the proper API for external signal updates.
   // It queues a SOURCE_DEPOSIT pseudo-source which is processed at
   // the next delta cycle.
   if (size == 4) {
      int32_t ival = (int32_t)val;
      sched_deposit(g_model, sig, &ival, 0, 1, 0, false);
   }
   else if (size == 1) {
      uint8_t *buf = alloca(width);
      for (uint32_t i = 0; i < width; i++) {
         buf[i] = (val & 1) ? 3 : 2;
         val >>= 1;
      }
      sched_deposit(g_model, sig, buf, 0, width, 0, false);
   }
   // No settling here — let the next wait_time/wait_edge run the deltas
}

void nvcb_set_signal_val_real(nvcb_hdl_t hdl, int action, double val)
{
   if (hdl < 0 || hdl >= g_num_handles || !g_model) return;
   nvcb_handle_t *h = &g_handles[hdl];
   if (h->kind != HDL_SIGNAL) return;

   if (signal_size(h->u.signal) == 8) {
      if (action == NVCB_DEPOSIT)
         deposit_signal(g_model, h->u.signal, &val, 0, 1);
      else if (action == NVCB_FORCE)
         force_signal(g_model, h->u.signal, &val, 0, 1);
   }
}

void nvcb_set_signal_val_str(nvcb_hdl_t hdl, int action, const char *val)
{
   nvcb_set_signal_val_binstr(hdl, action, val);
}

// ---- Iteration ----

// Iterator IDs are encoded with offset 0x40000000 to distinguish from handles
#define ITER_OFFSET 0x40000000

nvcb_hdl_t nvcb_iterate(nvcb_hdl_t hdl, int mode)
{
   if (hdl < 0 || hdl >= g_num_handles) return -1;
   if (g_num_iters >= MAX_ITERATORS) return -1;

   nvcb_handle_t *h = &g_handles[hdl];
   if (h->kind != HDL_SCOPE) return -1;

   int idx = g_num_iters++;
   g_iters[idx].parent = hdl;
   g_iters[idx].mode = mode;
   g_iters[idx].sig_idx = 0;
   g_iters[idx].child_idx = 0;
   g_iters[idx].done_signals = false;

   return ITER_OFFSET + idx;
}

nvcb_hdl_t nvcb_next(nvcb_hdl_t iter)
{
   if (iter < ITER_OFFSET) return -1;
   int idx = iter - ITER_OFFSET;
   if (idx < 0 || idx >= g_num_iters) return -1;

   nvcb_iter_t *it = &g_iters[idx];
   nvcb_handle_t *ph = &g_handles[it->parent];
   rt_scope_t *scope = ph->u.scope;

   // Yield signals first
   if (!it->done_signals) {
      while (it->sig_idx < scope->signals.count) {
         rt_signal_t *sig = scope->signals.items[it->sig_idx++];
         return alloc_signal_handle(sig, classify_signal(sig));
      }
      it->done_signals = true;
   }

   // Then child scopes
   while (it->child_idx < scope->children.count) {
      rt_scope_t *child = scope->children.items[it->child_idx++];
      int type = NVCB_MODULE;
      if (child->kind == SCOPE_PACKAGE) type = NVCB_PACKAGE;
      else if (child->kind == SCOPE_RECORD) type = NVCB_STRUCTURE;
      return alloc_scope_handle(child, type);
   }

   return -1;  // end of iteration
}

// ---- Time ----

void nvcb_get_sim_time(uint32_t *high, uint32_t *low)
{
   if (!g_model) { *high = 0; *low = 0; return; }

   unsigned deltas;
   int64_t now = model_now(g_model, &deltas);
   // NVC time is in femtoseconds, return as 64-bit split
   *low = (uint32_t)(now & 0xFFFFFFFF);
   *high = (uint32_t)((now >> 32) & 0xFFFFFFFF);
}

int nvcb_get_precision(void)
{
   return -15;  // femtoseconds
}

// ---- Python dispatch ----
// The Python shim registers a C function pointer (via ctypes CFUNCTYPE)
// that we call from C trampolines. ctypes converts the Python callable
// into a real C function we can call directly.

typedef void (*nvcb_dispatch_fn)(int64_t cb_id);
static nvcb_dispatch_fn g_dispatcher = NULL;

void nvcb_set_dispatcher(void *func)
{
   g_dispatcher = (nvcb_dispatch_fn)func;
}

static void dispatch_to_python(nvcb_cb_t cb_id)
{
   if (g_dispatcher != NULL)
      g_dispatcher((int64_t)cb_id);
}

// ---- Callback trampolines ----

static void timed_cb_trampoline(uint64_t now, rt_signal_t *s,
                                rt_watch_t *w, void *user)
{
   nvcb_cb_t cb_id = (nvcb_cb_t)(intptr_t)user;
   if (cb_id >= 0 && cb_id < MAX_CALLBACKS && g_callbacks[cb_id].active) {
      g_callbacks[cb_id].fired = true;
      g_last_fired = cb_id;
      dispatch_to_python(cb_id);
      // Timed callbacks are one-shot
      free_cb_slot(cb_id);
   }
}

static void phase_cb_trampoline(rt_model_t *m, void *user)
{
   nvcb_cb_t cb_id = (nvcb_cb_t)(intptr_t)user;
   if (cb_id >= 0 && cb_id < MAX_CALLBACKS && g_callbacks[cb_id].active) {
      nvcb_cb_kind_t kind = g_callbacks[cb_id].kind;
      g_callbacks[cb_id].fired = true;
      g_last_fired = cb_id;
      dispatch_to_python(cb_id);
      // Timed callbacks are one-shot at the NVC level.
      // Phase callbacks fire every cycle in NVC but CocoTB treats them as
      // one-shot. Mark inactive so subsequent firings are ignored;
      // CocoTB will re-register when needed.
      if (kind == NVCB_CB_TIMED) {
         free_cb_slot(cb_id);
      } else {
         // Phase callback: mark inactive but don't free (NVC has no
         // way to remove a registered phase callback)
         g_callbacks[cb_id].active = false;
      }
   }
}

static void value_change_trampoline(uint64_t now, rt_signal_t *s,
                                    rt_watch_t *w, void *user)
{
   nvcb_cb_t cb_id = (nvcb_cb_t)(intptr_t)user;
   if (cb_id >= 0 && cb_id < g_num_callbacks) {
      nvcb_callback_t *cb = &g_callbacks[cb_id];
      bool fire = false;

      if (cb->edge == NVCB_VALUE_CHANGE) {
         fire = true;
      }
      else {
         const uint8_t *val = (const uint8_t *)signal_value(s);
         const uint8_t *last = (const uint8_t *)signal_last_value(s);
         bool is_rising = (last[0] == 2 && val[0] == 3);
         bool is_falling = (last[0] == 3 && val[0] == 2);
         if ((cb->edge == NVCB_RISING && is_rising) ||
             (cb->edge == NVCB_FALLING && is_falling)) {
            fire = true;
         }
      }

      if (fire) {
         cb->fired = true;
         g_last_fired = cb_id;
         dispatch_to_python(cb_id);
      }
   }
}

// ---- Callback registration ----

nvcb_cb_t nvcb_register_timed_cb(uint64_t time_steps)
{
   if (!g_model) return -1;
   int id = alloc_cb_slot();
   if (id < 0) return -1;

   g_callbacks[id].kind = NVCB_CB_TIMED;
   g_callbacks[id].fired = false;
   g_callbacks[id].active = true;
   g_callbacks[id].watch = NULL;

   unsigned deltas;
   int64_t now = model_now(g_model, &deltas);
   g_callbacks[id].time = now + time_steps;

   model_set_timeout_cb(g_model, g_callbacks[id].time,
                        (rt_event_fn_t)phase_cb_trampoline,
                        (void *)(intptr_t)id);
   return id;
}

nvcb_cb_t nvcb_register_value_change_cb(nvcb_hdl_t signal, int edge)
{
   if (!g_model) return -1;
   if (signal < 0 || signal >= g_num_handles) return -1;
   if (g_handles[signal].kind != HDL_SIGNAL) return -1;

   int id = alloc_cb_slot();
   if (id < 0) return -1;
   g_callbacks[id].kind = NVCB_CB_VALUE_CHANGE;
   g_callbacks[id].signal = signal;
   g_callbacks[id].edge = edge;
   g_callbacks[id].fired = false;
   g_callbacks[id].active = true;

   rt_watch_t *w = watch_new(g_model, value_change_trampoline,
                             (void *)(intptr_t)id, WATCH_EVENT, 1);
   g_callbacks[id].watch = model_set_event_cb(g_model,
      g_handles[signal].u.signal, w);

   return id;
}

nvcb_cb_t nvcb_register_readonly_cb(void)
{
   if (!g_model) return -1;
   int id = alloc_cb_slot();
   if (id < 0) return -1;
   g_callbacks[id].kind = NVCB_CB_READONLY;
   g_callbacks[id].fired = false;
   g_callbacks[id].active = true;
   g_callbacks[id].watch = NULL;

   model_set_phase_cb(g_model, END_OF_PROCESSES,
                      phase_cb_trampoline, (void *)(intptr_t)id);
   return id;
}

nvcb_cb_t nvcb_register_readwrite_cb(void)
{
   if (!g_model) return -1;
   int id = alloc_cb_slot();
   if (id < 0) return -1;
   g_callbacks[id].kind = NVCB_CB_READWRITE;
   g_callbacks[id].fired = false;
   g_callbacks[id].active = true;
   g_callbacks[id].watch = NULL;

   model_set_phase_cb(g_model, LAST_KNOWN_DELTA_CYCLE,
                      phase_cb_trampoline, (void *)(intptr_t)id);
   return id;
}

nvcb_cb_t nvcb_register_nextstep_cb(void)
{
   if (!g_model) return -1;
   int id = alloc_cb_slot();
   if (id < 0) return -1;
   g_callbacks[id].kind = NVCB_CB_NEXTSTEP;
   g_callbacks[id].fired = false;
   g_callbacks[id].active = true;
   g_callbacks[id].watch = NULL;

   model_set_phase_cb(g_model, NEXT_TIME_STEP,
                      phase_cb_trampoline, (void *)(intptr_t)id);
   return id;
}

void nvcb_deregister_cb(nvcb_cb_t cb)
{
   if (cb < 0 || cb >= g_num_callbacks) return;
   g_callbacks[cb].active = false;
   if (g_callbacks[cb].watch) {
      watch_free(g_model, g_callbacks[cb].watch);
      g_callbacks[cb].watch = NULL;
   }
}

// ---- Event loop ----

nvcb_cb_t nvcb_run_until_cb(void)
{
   if (!g_model || !g_running) return -1;

   // Clear fired flags
   g_last_fired = -1;

   // Step simulation until a callback fires
   while (g_last_fired < 0 && g_running) {
      if (!model_step(g_model)) {
         g_running = false;
         return -1;  // simulation ended
      }
   }

   return g_last_fired;
}

// ---- Synchronous blocking helpers ----

// Sentinel callback for blocking waits — sets a flag the caller polls.
static volatile bool g_wait_done = false;

static void wait_timeout_trampoline(rt_model_t *m, void *user)
{
   g_wait_done = true;
}

static void wait_edge_trampoline(uint64_t now, rt_signal_t *s,
                                  rt_watch_t *w, void *user)
{
   int edge = (int)(intptr_t)user;
   const uint8_t *val  = (const uint8_t *)signal_value(s);
   const uint8_t *last = (const uint8_t *)signal_last_value(s);

   if (edge == NVCB_VALUE_CHANGE) {
      g_wait_done = true;
      return;
   }
   bool is_rising  = (last[0] == 2 && val[0] == 3);
   bool is_falling = (last[0] == 3 && val[0] == 2);
   if ((edge == NVCB_RISING && is_rising) ||
       (edge == NVCB_FALLING && is_falling)) {
      g_wait_done = true;
   }
}

void nvcb_wait_time(uint64_t delta_fs)
{
   if (!g_model) return;
   unsigned deltas;
   int64_t now = model_now(g_model, &deltas);
   model_step_to(g_model, now + delta_fs);
}

void nvcb_wait_edge(nvcb_hdl_t signal, int edge)
{
   if (!g_model) return;
   if (signal < 0 || signal >= g_num_handles) return;
   if (g_handles[signal].kind != HDL_SIGNAL) return;

   rt_signal_t *sig = g_handles[signal].u.signal;

   g_wait_done = false;
   rt_watch_t *w = watch_new(g_model, wait_edge_trampoline,
                             (void *)(intptr_t)edge, WATCH_EVENT, 1);
   model_set_event_cb(g_model, sig, w);

   while (!g_wait_done && g_running) {
      if (model_step(g_model)) {
         g_running = false;
         break;
      }
   }

   watch_free(g_model, w);
}

// ---- Free-running clock (driven by NVC, no Python) ----

#define MAX_CLOCKS 16

typedef struct {
   bool         active;
   rt_signal_t *signal;
   uint64_t     half_period;
   bool         high;
} nvcb_clock_t;

static nvcb_clock_t g_clocks[MAX_CLOCKS];
static int g_num_clocks = 0;

static void clock_toggle_trampoline(rt_model_t *m, void *user)
{
   int idx = (int)(intptr_t)user;
   if (idx < 0 || idx >= MAX_CLOCKS) return;
   nvcb_clock_t *clk = &g_clocks[idx];
   if (!clk->active) return;

   // Toggle via sched_deposit (proper API for external updates)
   uint8_t new_val = clk->high ? 2 : 3;
   clk->high = !clk->high;
   sched_deposit(m, clk->signal, &new_val, 0, 1, 0, false);

   // Schedule next toggle
   unsigned deltas;
   int64_t now = model_now(m, &deltas);
   model_set_timeout_cb(m, now + clk->half_period,
                        clock_toggle_trampoline, (void *)(intptr_t)idx);
}

int64_t nvcb_start_clock(nvcb_hdl_t signal, uint64_t period_fs)
{
   if (!g_model) return -1;
   if (signal < 0 || signal >= g_num_handles) return -1;
   if (g_handles[signal].kind != HDL_SIGNAL) return -1;
   if (g_num_clocks >= MAX_CLOCKS) return -1;

   int idx = g_num_clocks++;
   g_clocks[idx].active = true;
   g_clocks[idx].signal = g_handles[signal].u.signal;
   g_clocks[idx].half_period = period_fs / 2;
   g_clocks[idx].high = true;  // start with first toggle going high

   // Drive initial value low
   uint8_t low = 2;  // '0'
   sched_deposit(g_model, g_clocks[idx].signal, &low, 0, 1, 0, false);

   unsigned deltas;
   int64_t now = model_now(g_model, &deltas);
   model_set_timeout_cb(g_model, now + g_clocks[idx].half_period,
                        clock_toggle_trampoline, (void *)(intptr_t)idx);

   return idx;
}

void nvcb_stop_clock(int64_t clock_id)
{
   if (clock_id < 0 || clock_id >= MAX_CLOCKS) return;
   g_clocks[clock_id].active = false;
}

// ---- Simulation control ----

bool nvcb_is_running(void)
{
   return g_running;
}

void nvcb_stop(void)
{
   if (g_model) model_stop(g_model);
   g_running = false;
}

// ---- Info ----

const char *nvcb_get_simulator_product(void)
{
   return "NVC";
}

const char *nvcb_get_simulator_version(void)
{
#ifdef PACKAGE_VERSION
   return PACKAGE_VERSION;
#else
   return "1.19-dev";
#endif
}

// ---- VHPI entry point ----
// NVC loads this via --load and calls vhpi_startup_routines[]

static char *g_test_module = NULL;  // from COCOTB_TEST_MODULE env

static void cocotb_sim_event(rt_model_t *m, void *user)
{
   // Called at START_OF_SIMULATION
   // Initialize Python and run CocoTB tests

   nvcb_init(m);

   if (!Py_IsInitialized()) {
      Py_Initialize();
   }

   // Add build dir to Python path
   const char *pypath = getenv("PYTHONPATH");
   if (pypath) {
      char cmd[4096];
      snprintf(cmd, sizeof(cmd),
               "import sys; sys.path[0:0] = '%s'.split(':')", pypath);
      PyRun_SimpleString(cmd);
   }

   // Import and run the test module
   const char *module = g_test_module ? g_test_module
                        : getenv("COCOTB_TEST_MODULE");
   if (!module) module = "test_basic";

   fprintf(stderr, "nvc-cocotb: loading test module '%s'\n", module);

   // Sync mode: import the translated test module, find _NVCB_TESTS list,
   // and call each test function sequentially. Each test runs to completion
   // because all triggers are now blocking C calls.
   const char *pycmd =
      "import sys, os\n"
      "# In sync mode, writes need to happen immediately (no scheduler)\n"
      "os.environ['COCOTB_TRUST_INERTIAL_WRITES'] = '1'\n"
      "sys.stdout.flush()\n"
      "\n"
      "import cocotb\n"
      "from cocotb import simulator\n"
      "from cocotb._init import init_package_from_simulation\n"
      "init_package_from_simulation([])\n"
      "\n"
      "test_module = os.environ.get('COCOTB_TEST_MODULES', 'test_basic')\n"
      "import importlib\n"
      "mod = importlib.import_module(test_module)\n"
      "\n"
      "tests = getattr(mod, '_NVCB_TESTS', [])\n"
      "if not tests:\n"
      "    # Fallback: find functions starting with 'test_'\n"
      "    tests = [getattr(mod, n) for n in dir(mod)\n"
      "             if n.startswith('test_') and callable(getattr(mod, n))]\n"
      "\n"
      "print(f'nvc-cocotb: running {len(tests)} test(s)')\n"
      "sys.stdout.flush()\n"
      "\n"
      "passed = 0\n"
      "failed = 0\n"
      "for test_func in tests:\n"
      "    name = test_func.__name__\n"
      "    print(f'\\n=== {name} ===')\n"
      "    sys.stdout.flush()\n"
      "    try:\n"
      "        test_func(cocotb.top)\n"
      "        print(f'  PASS')\n"
      "        passed += 1\n"
      "    except Exception as e:\n"
      "        import traceback\n"
      "        traceback.print_exc()\n"
      "        print(f'  FAIL: {e}')\n"
      "        failed += 1\n"
      "    sys.stdout.flush()\n"
      "\n"
      "print(f'\\nnvc-cocotb: {passed} passed, {failed} failed')\n"
      "sys.stdout.flush()\n";

   if (PyRun_SimpleString(pycmd) != 0) {
      fprintf(stderr, "nvc-cocotb: Python error in test runner\n");
      PyErr_Print();
   }
   // Return — NVC's model_run() will drive simulation and fire callbacks
}

#include <dlfcn.h>

// Direct entry point called by NVC's --cocotb option after model_reset.
// No VHPI needed — direct access to the rt_model.
void nvc_cocotb_entry(rt_model_t *m)
{
   // Re-open libpython with RTLD_GLOBAL so dlopen-loaded extension
   // modules (like _contextvars.so) can resolve PyContextVar_Type etc.
   dlopen("libpython3.10.so.1.0", RTLD_NOW | RTLD_GLOBAL);

   fprintf(stderr, "nvc-cocotb: entry point called with model=%p\n", (void *)m);
   cocotb_sim_event(m, NULL);
}
