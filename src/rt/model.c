//
//  Copyright (C) 2011-2024  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "array.h"
#include "common.h"
#include "debug.h"
#include "diag.h"
#include "hash.h"
#include "jit/jit-exits.h"
#include "jit/jit.h"
#include "jit/jit-priv.h"
#include "lib.h"
#include "option.h"
#include "printf.h"
#include "psl/psl-node.h"
#include "rt/assert.h"
#include "rt/copy.h"
#include "rt/heap.h"
#include "rt/model.h"
#include "vhdl2vlog.h"
#include "rt/random.h"
#include "rt/structs.h"
#include "thread.h"
#include "tree.h"
#include "type.h"
#include "vlog/vlog-node.h"

#include <assert.h>
#include <inttypes.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

typedef struct _rt_callback rt_callback_t;
typedef struct _memblock memblock_t;

typedef struct _rt_callback {
   rt_event_fn_t  fn;
   void          *user;
   rt_callback_t *next;
} rt_callback_t;

typedef enum {
   EVENT_TIMEOUT,
   EVENT_DRIVER,
   EVENT_PROCESS,
   EVENT_PSEUDO,
} event_kind_t;

#define MEMBLOCK_ALIGN   64
#define MEMBLOCK_PAGE_SZ 0x800000
#define TRIGGER_TAB_SIZE 64

#if ASAN_ENABLED
#define MEMBLOCK_REDZONE 16
#else
#define MEMBLOCK_REDZONE 0
#endif

typedef struct _memblock {
   memblock_t *chain;
   size_t      alloc;
   size_t      limit;
   uint8_t     data[];
} memblock_t;

STATIC_ASSERT(sizeof(memblock_t) <= MEMBLOCK_ALIGN);

typedef struct {
   waveform_t    *free_waveforms;
   tlab_t        *tlab;
   rt_wakeable_t *active_obj;
   rt_scope_t    *active_scope;
} __attribute__((aligned(64))) model_thread_t;

typedef void (*defer_fn_t)(rt_model_t *, void *);

typedef struct {
   defer_fn_t  fn;
   void       *arg;
} defer_task_t;

typedef struct {
   defer_task_t *tasks;
   unsigned      count;
   unsigned      max;
} deferq_t;

typedef struct _aj_defer_out aj_defer_out_t;   // NVC_ACCEL_BANK 2-bank output
typedef struct _aj_chunk     aj_chunk_t;       // one installed accel subtree

typedef struct _rt_model {
   tree_t             top;
   hash_t            *scopes;
   rt_scope_t        *root;
   mspace_t          *mspace;
   jit_t             *jit;
   rt_nexus_t        *nexuses;
   rt_nexus_t       **nexus_tail;
   delta_cycle_t      stop_delta;
   int                iteration;
   uint64_t           now;
   uint64_t           trigger_epoch;
   bool               can_create_delta;
   bool               next_is_delta;
   bool               force_stop;
   bool               blocking_update;
   unsigned           n_signals;
   heap_t            *eventq_heap;
   ihash_t           *res_memo;
   rt_watch_t        *watches;
   deferq_t           procq;
   deferq_t           next_procq;
   deferq_t           driverq;
   deferq_t           next_driverq;
   deferq_t           postponedq;
   deferq_t           implicitq;
   deferq_t           triggerq;
   deferq_t           inactiveq;
   deferq_t           next_inactiveq;
   deferq_t           nonblockq;
   heap_t            *driving_heap;
   heap_t            *effective_heap;
   rt_callback_t     *phase_cbs[END_OF_SIMULATION + 1];
   cover_data_t      *cover;
   nvc_rusage_t       ready_rusage;
   nvc_lock_t         memlock;
   memblock_t        *memblocks;
   model_thread_t    *threads[MAX_THREADS];
   signal_list_t      eventsigs;
   bool               shuffle;
   bool               liveness;
   rt_trigger_t      *triggertab[TRIGGER_TAB_SIZE];

   // Stage-1 parallel-sim profiling (NVC_PROFILE_PROCS): per-delta
   // process-queue depth distribution + time-in-bucket. The depth is the
   // parallelism width available that delta; the time-weighted view gives
   // the parallelizable fraction (Amdahl ceiling) for the SMP scheduler.
   bool               prof_enabled;
   uint64_t           prof_deltas;        // deltas that ran the proc queue
   uint64_t           prof_activations;   // total process activations
   uint64_t           prof_proc_ns;       // total ns in process execution
   uint64_t           prof_depth_hist[8]; // delta count by depth bucket
   uint64_t           prof_depth_ns[8];   // ns by depth bucket

   // NVC_FAST_CLK: flat posedge-fanout dispatch table. The clk-ONLY processes
   // that fan out from the clock nexus are skipped in wakeup_one (never queued)
   // and instead run directly via this table on the posedge — bypassing the
   // event-queue/procq round-trip (~67% of the per-cycle cost on synthesized
   // clocked RTL). Built once at accel install; off unless the env is set and
   // at least one clk-only proc survives the filter.
   bool               fastclk_on;
   bool               fastclk_hit;     // a clk-only proc woke this delta
   rt_proc_t        **fastclk_table;
   unsigned           fastclk_count;
   rt_nexus_t        *fastclk_nexus;
   uint8_t           *fastclk_data;    // clk effective bytes (bit0 = level)
   uint64_t           fastclk_auto_at; // NVC_FAST_CLK_AUTO: build table at this time (fs); 0=off
   rt_nexus_t       **fastclk_guard_nx;   // quiet-sensitivity guard nexuses
   const rt_nexus_vtable_t **fastclk_guard_orig; // their original vtables
   rt_nexus_vtable_t *fastclk_guard_vt;   // per-guard patched copies (notify -> dissolve)
   unsigned           fastclk_nguards;
   rt_nexus_t       **fastclk_bl;         // nexuses that ever dissolved the table
   unsigned           fastclk_nbl, fastclk_blmax;  // -> never guard again
   hash_t            *depositors;         // nexus -> last depositing rt_proc_t
                                          // (fused-cone force/release wakeups)

   // NVC_ACCEL_BANK: per-output 2-bank deferred write (VHDL delta / Verilog
   // NBA). The chunk STAGEs its computed output into a private shadow on the
   // posedge (not the signal); after the fast-clk table has dispatched (every
   // clk-reader read the OLD effective value), the swap publishes shadow->
   // effective. Replaces deposit_signal's lock/split/wakeup/extra-delta cost
   // with a pointer-cached cmp+copy. Only for outputs that pass the gate
   // (readers all clk-only fast-clk procs); others fall back to deposit_signal.
   // Per-chunk registry: each installed accel subtree is one aj_chunk_t, routed
   // to via its own vtable (recovered by container_of from proc->vtable) so >1
   // chunk can be live. Replaces the old single global g_aj_eval.
   aj_chunk_t       **aj_chunks;   // pointer array — chunks have stable address
   unsigned           aj_chunk_count;
   unsigned           aj_chunk_max;
} rt_model_t;

// Depth bucket: 0, 1, 2-3, 4-15, 16-63, 64-255, 256-1023, 1024+
static inline int prof_bucket(unsigned d)
{
   if (d == 0)    return 0;
   if (d == 1)    return 1;
   if (d < 4)     return 2;
   if (d < 16)    return 3;
   if (d < 64)    return 4;
   if (d < 256)   return 5;
   if (d < 1024)  return 6;
   return 7;
}

#define FMT_VALUES_SZ   128
#define NEXUS_INDEX_MIN 8
#define TRACE_SIGNALS   1
#define WAVEFORM_CHUNK  256
#define PENDING_MIN     4
#define MAX_RANK        UINT8_MAX

#define TRACE(...) do {                                 \
      if (unlikely(__trace_on))                         \
         __model_trace(get_model(), __VA_ARGS__);       \
   } while (0)

#define MODEL_ENTRY(m)                                                  \
   rt_model_t *__save __attribute__((unused, cleanup(__model_exit)));   \
   __model_entry(m, &__save);                                           \

#if USE_EMUTLS
static rt_model_t *__model = NULL;
#else
static __thread rt_model_t *__model = NULL;
#endif

static bool __trace_on = false;

static void *source_value(rt_nexus_t *nexus, rt_source_t *src);
static void free_value(rt_nexus_t *n, rt_value_t v);
static rt_nexus_t *clone_nexus(rt_model_t *m, rt_nexus_t *old, int offset);
static void put_driving(rt_model_t *m, rt_nexus_t *n, const void *value);
static void put_effective_impl(rt_model_t *m, rt_nexus_t *n, const void *value);
static void calculate_driving_value(rt_model_t *m, rt_nexus_t *n);
static void notify_event_default(rt_model_t *m, rt_nexus_t *n);
static void notify_event(rt_model_t *m, rt_nexus_t *n);

// Dispatch deposit through vtable — defined after inline helpers
static void put_effective(rt_model_t *m, rt_nexus_t *n, const void *value);

// Default nexus vtable — full general-case implementations
static const rt_nexus_vtable_t nexus_default_vtable = {
   .update_driving = calculate_driving_value,
   .deposit        = put_effective_impl,
   .read_source    = source_value,
   .notify         = notify_event_default,
};
static void calculate_driving_single(rt_model_t *m, rt_nexus_t *n);
static void calculate_driving_memo1(rt_model_t *m, rt_nexus_t *n);
static const rt_nexus_vtable_t nexus_single_driver_vtable;
static const rt_nexus_vtable_t nexus_memo1_vtable;
static void update_implicit_signal(rt_model_t *m, rt_implicit_t *imp);
static bool run_trigger(rt_model_t *m, rt_trigger_t *t);
static void wakeup_all(rt_model_t *m, void **pending);
static void reset_scope(rt_model_t *m, rt_scope_t *s);
static void async_run_process(rt_model_t *m, void *arg);
static void evproc_shutdown(void);
static void async_update_property(rt_model_t *m, void *arg);
static void async_update_driver(rt_model_t *m, void *arg);
static void async_fast_driver(rt_model_t *m, void *arg);
static void async_fast_all_drivers(rt_model_t *m, void *arg);
static void async_pseudo_source(rt_model_t *m, void *arg);
static void async_transfer_signal(rt_model_t *m, void *arg);
static void async_run_trigger(rt_model_t *m, void *arg);
static void async_update_implicit_signal(rt_model_t *m, void *arg);

static int fmt_time_r(char *buf, size_t len, int64_t t, const char *sep)
{
   static const struct {
      int64_t time;
      const char *unit;
   } units[] = {
      { INT64_C(1), "fs" },
      { INT64_C(1000), "ps" },
      { INT64_C(1000000), "ns" },
      { INT64_C(1000000000), "us" },
      { INT64_C(1000000000000), "ms" },
      { 0, NULL }
   };

   int u = 0;
   while (units[u + 1].unit && (t % units[u + 1].time == 0))
      ++u;

   return checked_sprintf(buf, len, "%"PRIi64"%s%s",
                          t / units[u].time, sep, units[u].unit);
}

__attribute__((format(printf, 2, 3)))
static void __model_trace(rt_model_t *m, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);

   static nvc_lock_t lock = 0;
   {
      SCOPED_LOCK(lock);

      ostream_t *os = nvc_stderr();
      if (m->iteration < 0)
         ostream_puts(os, "TRACE (init): ");
      else {
         char buf[64];
         fmt_time_r(buf, sizeof(buf), m->now, "");
         nvc_fprintf(os, "TRACE %s+%d: ", buf, m->iteration);
      }
      nvc_vfprintf(os, fmt, ap);
      ostream_puts(os, "\n");
      fflush(stderr);
   }

   va_end(ap);
}

static const char *trace_time(uint64_t value)
{
   static __thread char buf[2][32];
   static __thread int which = 0;

   which ^= 1;
   fmt_time_r(buf[which], 32, value, "");
   return buf[which];
}

static const char *trace_states(bit_mask_t *mask)
{
   static __thread text_buf_t *tb = NULL;

   if (tb == NULL)
      tb = tb_new();

   tb_rewind(tb);
   tb_append(tb, '{');

   size_t bit = -1;
   while (mask_iter(mask, &bit))
      tb_printf(tb, "%s%zd", tb_len(tb) > 1 ? "," : "", bit);

   tb_append(tb, '}');

   return tb_get(tb);
}

static const char *trace_nexus(rt_nexus_t *n)
{
   static __thread text_buf_t *tb = NULL;

   if (tb == NULL)
      tb = tb_new();

   tb_rewind(tb);

   if (is_signal_scope(n->signal->parent))
      tb_printf(tb, "%s.", istr(n->signal->parent->name));

   tb_istr(tb, tree_ident(n->signal->where));

   if (n->width * n->size < n->signal->shared.size) {
      tb_printf(tb, "[%d", n->offset);
      if (n->width > 1)
         tb_printf(tb, ":%d", n->offset + n->width - 1);
      tb_append(tb, ']');
   }

   return tb_get(tb);
}

static void model_diag_cb(diag_t *d, void *arg)
{
   rt_model_t *m = arg;

   if (m->iteration < 0)
      diag_printf(d, "(init): ");
   else  {
      char tmbuf[64];
      fmt_time_r(tmbuf, sizeof(tmbuf), m->now, "");

      diag_printf(d, "%s+%d: ", tmbuf, m->iteration);
   }
}

static void __model_entry(rt_model_t *m, rt_model_t **save)
{
   if (__model == NULL)
      diag_add_hint_fn(model_diag_cb, m);

   *save = __model;
   __model = m;
}

static void __model_exit(rt_model_t **save)
{
   __model = *save;
   *save = NULL;

   if (__model == NULL)
      diag_remove_hint_fn(model_diag_cb);
}

static char *fmt_values_r(const void *values, size_t len, char *buf, size_t max)
{
   char *p = buf;
   const uint8_t *vptr = values;

   for (unsigned i = 0; i < len; i++) {
      if (buf + max - p <= 5) {
         checked_sprintf(p, buf + max - p, "...");
         break;
      }
      else
         p += checked_sprintf(p, buf + max - p, "%02x", *vptr++);
   }

   return buf;
}

static const char *fmt_nexus(rt_nexus_t *n, const void *values)
{
   static char buf[FMT_VALUES_SZ*2 + 2];
   return fmt_values_r(values, n->size * n->width, buf, sizeof(buf));
}

static const char *fmt_values(const void *values, uint32_t len)
{
   static char buf[FMT_VALUES_SZ*2 + 2];
   return fmt_values_r(values, len, buf, sizeof(buf));
}

static const char *fmt_jit_value(jit_scalar_t value, bool scalar, uint32_t len)
{
   static char buf[FMT_VALUES_SZ*2 + 2];
   if (scalar) {
      checked_sprintf(buf, sizeof(buf), "%"PRIx64, value.integer);
      return buf;
   }
   else
      return fmt_values_r(value.pointer, len, buf, sizeof(buf));
}

static model_thread_t *model_thread(rt_model_t *m)
{
#if RT_MULTITHREADED
   const int my_id = thread_id();

   if (unlikely(m->threads[my_id] == NULL))
      return (m->threads[my_id] = xcalloc(sizeof(model_thread_t)));

   return m->threads[my_id];
#else
   assert(thread_id() == 0);
   return m->threads[0];
#endif
}

__attribute__((cold, noinline))
static void deferq_grow(deferq_t *dq)
{
   dq->max = MAX(dq->max * 2, 64);
   dq->tasks = xrealloc_array(dq->tasks, dq->max, sizeof(defer_task_t));
}

// Stage-1c parallel process dispatch: process eval runs lock-free (reads
// settled values, writes its own driver), but EVENT SCHEDULING into the
// global queues must be serialized while worker cores run process slices.
// g_par_active gates the (otherwise zero-cost) lock; g_sched_lock is the
// single global schedule lock taken only around the queue/heap appends.
static int        g_par_active = 0;
static nvc_lock_t g_sched_lock = 0;
// Count of nexus splits (clone_nexus). For synthesizable RTL the access
// pattern is static, so all splits happen in the first cycle(s); the parallel
// dispatch waits until this stops changing (structure frozen) before running
// process slices concurrently, so the nexus tree is immutable during eval.
static uint64_t   g_split_count = 0;
static uint64_t   g_split_last  = 0;   // sim time (fs) of the most recent split

static inline void deferq_append(deferq_t *dq, defer_fn_t fn, void *arg)
{
   if (unlikely(dq->count == dq->max))
      deferq_grow(dq);

   dq->tasks[dq->count++] = (defer_task_t){ fn, arg };
}

static inline void deferq_do(deferq_t *dq, defer_fn_t fn, void *arg)
{
   if (unlikely(relaxed_load(&g_par_active))) {
      nvc_lock(&g_sched_lock);
      deferq_append(dq, fn, arg);
      nvc_unlock(&g_sched_lock);
   }
   else
      deferq_append(dq, fn, arg);
}

static void deferq_scan(deferq_t *dq, scan_fn_t fn, void *arg)
{
   for (int i = 0; i < dq->count; i++)
      (*fn)(dq->tasks[i].fn, dq->tasks[i].arg, arg);
}

static void deferq_shuffle(deferq_t *dq)
{
   int cur = dq->count;
   while (cur > 0) {
      const int swap = get_random() % cur--;
      const defer_task_t tmp = dq->tasks[cur];
      dq->tasks[cur] = dq->tasks[swap];
      dq->tasks[swap] = tmp;
   }
}

static void deferq_run(rt_model_t *m, deferq_t *dq)
{
   const defer_task_t *tasks = dq->tasks;
   const int count = dq->count;

   int i = 0;
   for (; i < count - 1; i++) {
      // Prefetch ahead the next task argument to avoid cache misses
      // when we execute it
      prefetch_read(tasks[i + 1].arg);
      (*tasks[i].fn)(m, tasks[i].arg);
   }
   for (; i < count; i++)
      (*tasks[i].fn)(m, tasks[i].arg);

   assert(dq->tasks == tasks);
   assert(dq->count == count);

   dq->count = 0;
}

static void *static_alloc(rt_model_t *m, size_t size)
{
   const int total_bytes = ALIGN_UP(size + MEMBLOCK_REDZONE, MEMBLOCK_ALIGN);

   RT_LOCK(m->memlock);

   memblock_t *mb = m->memblocks;

   if (mb == NULL || mb->alloc + total_bytes > mb->limit) {
      const size_t pagesz =
         MAX(MEMBLOCK_PAGE_SZ, total_bytes + 2 * MEMBLOCK_ALIGN);

      mb = map_huge_pages(MEMBLOCK_ALIGN, pagesz);
      mb->alloc = MEMBLOCK_ALIGN;
      mb->limit = pagesz - MEMBLOCK_ALIGN;   // Allow overreading in intrinsics

      ASAN_POISON(mb->data, pagesz - sizeof(memblock_t));

      m->memblocks = mb;
   }

   assert((mb->alloc & (MEMBLOCK_ALIGN - 1)) == 0);

   void *ptr = (void *)mb + mb->alloc;
   mb->alloc += total_bytes;

   ASAN_UNPOISON(ptr, size);
   return ptr;
}

static void run_callbacks(rt_model_t *m, model_phase_t phase)
{
   rt_callback_t *list = m->phase_cbs[phase];
   m->phase_cbs[phase] = NULL;

   for (rt_callback_t *it = list, *tmp; it; it = tmp) {
      tmp = it->next;
      (*it->fn)(m, it->user);
      free(it);
   }
}

static void restore_scopes(rt_model_t *m, tree_t block, rt_scope_t *parent)
{
   rt_scope_t *s = create_scope(m, block, parent);

   const int nstmts = tree_stmts(block);
   for (int i = 0; i < nstmts; i++) {
      tree_t t = tree_stmt(block, i);
      if (tree_kind(t) == T_BLOCK)
         restore_scopes(m, t, s);
   }
}

rt_model_t *model_new(jit_t *jit, cover_data_t *cover)
{
   rt_model_t *m = xcalloc(sizeof(rt_model_t));
   m->scopes      = hash_new(256);
   m->mspace      = jit_get_mspace(jit);
   m->jit         = jit;
   m->nexus_tail  = &(m->nexuses);
   m->iteration   = -1;
   m->eventq_heap = heap_new(512);
   m->res_memo    = ihash_new(128);
   m->depositors  = hash_new(1024);
   m->cover       = cover;

   m->driving_heap   = heap_new(64);
   m->effective_heap = heap_new(64);

   m->can_create_delta = true;
   m->next_is_delta    = true;

   m->threads[thread_id()] = static_alloc(m, sizeof(model_thread_t));

   m->prof_enabled = (getenv("NVC_PROFILE_PROCS") != NULL);

   const char *fca = getenv("NVC_FAST_CLK_AUTO");
   if (fca != NULL)   // ns -> fs; default 1000ns if set empty
      m->fastclk_auto_at = (strtoull(fca, NULL, 10) ?: 1000) * UINT64_C(1000000);

   __trace_on = opt_get_int(OPT_RT_TRACE);

   return m;
}

rt_model_t *get_model(void)
{
   assert(__model != NULL);
#ifdef USE_EMUTLS
   assert(thread_id() == 0);
#endif
   return __model;
}

rt_model_t *get_model_or_null(void)
{
   return __model;
}

static rt_wakeable_t *get_active_wakeable(void)
{
   return __model ? model_thread(__model)->active_obj : NULL;
}

rt_proc_t *get_active_proc(void)
{
   rt_wakeable_t *obj = get_active_wakeable();
   if (obj == NULL)
      return NULL;

   assert(obj->kind == W_PROC || obj->kind == W_ASSIGN);
   return container_of(obj, rt_proc_t, wakeable);
}

rt_scope_t *get_active_scope(rt_model_t *m)
{
   return model_thread(m)->active_scope;
}

static void free_waveform(rt_model_t *m, waveform_t *w)
{
   model_thread_t *thread = model_thread(m);
   w->next = thread->free_waveforms;
   thread->free_waveforms = w;
}

static void cleanup_nexus(rt_model_t *m, rt_nexus_t *n)
{
   for (rt_source_t *s = &(n->sources); s; s = s->chain_input) {
      if (s->tag != SOURCE_PORT)
         continue;

      rt_conv_func_t *cf = s->u.port.conv_func;
      if (cf == NULL)
         continue;
      else if (cf->inputs != NULL && cf->inputs != cf->tail) {
         free(cf->inputs);
         s->u.port.conv_func->inputs = NULL;
      }
   }

   if (n->pending != NULL && pointer_tag(n->pending) == 0)
      free(n->pending);
}

static void cleanup_signal(rt_model_t *m, rt_signal_t *s)
{
   rt_nexus_t *n = &(s->nexus), *tmp;
   for (int i = 0; i < s->n_nexus; i++, n = tmp) {
      tmp = n->chain;
      cleanup_nexus(m, n);
   }

   free(s->index);
}

static void cleanup_scope(rt_model_t *m, rt_scope_t *scope)
{
   for (int i = 0; i < scope->procs.count; i++) {
      rt_proc_t *p = scope->procs.items[i];
      mptr_free(m->mspace, &(p->privdata));
      tlab_release(p->tlab);
      free(p);
   }
   ACLEAR(scope->procs);

   for (int i = 0; i < scope->signals.count; i++)
      cleanup_signal(m, scope->signals.items[i]);
   ACLEAR(scope->signals);

   for (int i = 0; i < scope->aliases.count; i++)
      free(scope->aliases.items[i]);
   ACLEAR(scope->aliases);

   for (int i = 0; i < scope->properties.count; i++) {
      rt_prop_t *p = scope->properties.items[i];
      mask_free(&p->state);
      mask_free(&p->newstate);
      mptr_free(m->mspace, &(p->privdata));
      free(p);
   }
   ACLEAR(scope->properties);

   for (int i = 0; i < scope->children.count; i++)
      cleanup_scope(m, scope->children.items[i]);
   ACLEAR(scope->children);

   mptr_free(m->mspace, &(scope->privdata));
   free(scope);
}

void model_free(rt_model_t *m)
{
   evproc_shutdown();

   if (unlikely(m->prof_enabled))
      notef("nexus splits: %"PRIu64" total, last at %"PRIu64" ns",
            g_split_count, g_split_last / 1000000);

   if (unlikely(m->prof_enabled) && m->prof_deltas > 0) {
      static const char *const label[8] = {
         "       0", "       1", "    2-3", "   4-15",
         "  16-63", " 64-255", "256-1023", "  1024+" };
      const double tot_ns = MAX(m->prof_proc_ns, 1);
      uint64_t wide_ns = 0, wide_act = 0;   // depth >= 16 (bucket >= 4)
      for (int i = 4; i < 8; i++) wide_ns += m->prof_depth_ns[i];
      notef("NVC_PROFILE_PROCS: %"PRIu64" proc-running deltas, "
            "%"PRIu64" activations, %.1f ms in process eval",
            m->prof_deltas, m->prof_activations, m->prof_proc_ns / 1e6);
      notef("  depth      deltas        activ?   %%eval-time");
      for (int i = 0; i < 8; i++) {
         if (m->prof_depth_hist[i] == 0) continue;
         notef("  %s  %10"PRIu64"   %10s   %6.2f%%", label[i],
               m->prof_depth_hist[i], "",
               100.0 * m->prof_depth_ns[i] / tot_ns);
      }
      notef("  parallelizable fraction (depth>=16): %.1f%% of eval time",
            100.0 * wide_ns / tot_ns);
      (void)wide_act;
   }

   if (opt_get_int(OPT_RT_STATS)) {
      nvc_rusage_t ru;
      nvc_rusage(&ru);

      unsigned mem = 0;
      for (memblock_t *mb = m->memblocks; mb; mb = mb->chain)
         mem += mb->alloc;

      notef("setup:%ums run:%ums user:%ums sys:%ums maxrss:%ukB static:%ukB",
            m->ready_rusage.ms, ru.ms, ru.user, ru.sys, ru.rss, mem / 1024);
   }

   while (heap_size(m->eventq_heap) > 0) {
      void *e = heap_extract_min(m->eventq_heap);
      if (pointer_tag(e) == EVENT_TIMEOUT)
         free(untag_pointer(e, rt_callback_t));
   }

   if (m->root != NULL)
      cleanup_scope(m, m->root);

   for (int i = 0; i < MAX_THREADS; i++) {
      model_thread_t *thread = m->threads[i];
      if (thread != NULL)
         tlab_release(thread->tlab);
   }

   free(m->procq.tasks);
   free(m->next_procq.tasks);
   free(m->postponedq.tasks);
   free(m->implicitq.tasks);
   free(m->triggerq.tasks);
   free(m->inactiveq.tasks);
   free(m->next_inactiveq.tasks);
   free(m->nonblockq.tasks);
   free(m->driverq.tasks);
   free(m->next_driverq.tasks);

   for (rt_watch_t *it = m->watches, *tmp; it; it = tmp) {
      tmp = it->chain_all;
      free(it);
   }

   for (int i = 0; i < ARRAY_LEN(m->phase_cbs); i++) {
      for (rt_callback_t *it = m->phase_cbs[i], *tmp; it; it = tmp) {
         tmp = it->next;
         free(it);
      }
   }

   for (memblock_t *mb = m->memblocks, *tmp; mb; mb = tmp) {
      tmp = mb->chain;
      nvc_munmap(mb, mb->limit + MEMBLOCK_ALIGN);
   }

   heap_free(m->effective_heap);
   heap_free(m->driving_heap);
   heap_free(m->eventq_heap);
   hash_free(m->scopes);
   ihash_free(m->res_memo);
   ACLEAR(m->eventsigs);
   free(m);
}

bool is_signal_scope(rt_scope_t *s)
{
   return s->kind == SCOPE_RECORD || s->kind == SCOPE_ARRAY;
}

rt_signal_t *find_signal(rt_scope_t *scope, tree_t decl)
{
   for (int i = 0; i < scope->signals.count; i++) {
      if (scope->signals.items[i]->where == decl)
         return scope->signals.items[i];
   }

   for (int i = 0; i < scope->aliases.count; i++) {
      if (scope->aliases.items[i]->where == decl)
         return scope->aliases.items[i]->signal;
   }

   return NULL;
}

rt_proc_t *find_proc(rt_scope_t *scope, tree_t proc)
{
   for (int i = 0; i < scope->procs.count; i++) {
      if (scope->procs.items[i]->where == proc)
         return scope->procs.items[i];
   }

   return NULL;
}

rt_watch_t *find_watch(rt_nexus_t *n, sig_event_fn_t fn)
{
   if (n->pending == NULL)
      return NULL;
   else if (pointer_tag(n->pending) == 1) {
      rt_wakeable_t *obj = untag_pointer(n->pending, rt_wakeable_t);
      if (obj->kind == W_WATCH) {
         rt_watch_t *w = container_of(obj, rt_watch_t, wakeable);
         if (w->fn == fn)
            return w;
      }

      return NULL;
   }
   else {
      rt_pending_t *p = untag_pointer(n->pending, rt_pending_t);

      for (int i = 0; i < p->count; i++) {
         rt_wakeable_t *obj = untag_pointer(p->wake[i], rt_wakeable_t);
         if (obj->kind == W_WATCH) {
            rt_watch_t *w = container_of(obj, rt_watch_t, wakeable);
            if (w->fn == fn)
               return w;
         }
      }

      return NULL;
   }
}

rt_scope_t *create_scope(rt_model_t *m, tree_t block, rt_scope_t *parent)
{
   if (parent == NULL) {
      assert(m->top == NULL);
      assert(tree_kind(block) == T_ELAB);

      m->top = block;

      m->root = xcalloc(sizeof(rt_scope_t));
      m->root->kind     = SCOPE_ROOT;
      m->root->where    = block;
      m->root->privdata = MPTR_INVALID;
      m->root->name     = lib_name(lib_work());

      if (tree_stmts(block) > 0)
         restore_scopes(m, tree_stmt(block, 0), m->root);

      return m->root;
   }
   else {
      rt_scope_t *s = xcalloc(sizeof(rt_scope_t));
      s->where    = block;
      s->kind     = SCOPE_INSTANCE;
      s->privdata = mptr_new(m->mspace, "block privdata");
      s->parent   = parent;
      s->name     = ident_prefix(parent->name, tree_ident(block), '.');

      APUSH(parent->children, s);

      hash_put(m->scopes, block, s);

      MODEL_ENTRY(m);

      TRACE("initialise scope %s", istr(s->name));

      model_thread_t *thread = model_thread(m);
      thread->active_scope = s;

      jit_handle_t handle = jit_lazy_compile(m->jit, s->name);
      if (handle == JIT_HANDLE_INVALID)
         fatal_trace("failed to compile %s", istr(s->name));

      jit_scalar_t result, context = { .pointer = NULL };
      jit_scalar_t p2 = { .integer = 0 };

      if (s->parent->kind != SCOPE_ROOT)
         context.pointer = *mptr_get(s->parent->privdata);

      tlab_t tlab = jit_null_tlab(m->jit);

      if (jit_fastcall(m->jit, handle, &result, context, p2, &tlab))
         *mptr_get(s->privdata) = result.pointer;
      else
         m->force_stop = true;

      assert(thread->active_scope == s);
      thread->active_scope = NULL;
      return s;
   }
}

rt_scope_t *find_scope(rt_model_t *m, tree_t container)
{
   return hash_get(m->scopes, container);
}

rt_scope_t *root_scope(rt_model_t *m)
{
   return m->root;
}

rt_scope_t *child_scope(rt_scope_t *scope, tree_t decl)
{
   for (int i = 0; i < scope->children.count; i++) {
      rt_scope_t *s = scope->children.items[i];
      if (s->where == decl)
         return s;
   }

   return NULL;
}

rt_scope_t *child_scope_at(rt_scope_t *scope, int index)
{
   return AGET(scope->children, index);
}

const void *signal_value(rt_signal_t *s)
{
   return s->shared.data;
}

const void *signal_last_value(rt_signal_t *s)
{
   return s->shared.data + s->shared.size;
}

uint8_t signal_size(rt_signal_t *s)
{
   return s->nexus.size;
}

uint32_t signal_width(rt_signal_t *s)
{
   return s->shared.size / s->nexus.size;
}

size_t signal_expand(rt_signal_t *s, uint64_t *buf, size_t max)
{
   const size_t total = s->shared.size / s->nexus.size;

#define SIGNAL_READ_EXPAND_U64(type) do {                               \
      const type *sp = (type *)s->shared.data;                          \
      for (int i = 0; i < max && i < total; i++)                        \
         buf[i] = sp[i];                                                \
   } while (0)

   FOR_ALL_SIZES(s->nexus.size, SIGNAL_READ_EXPAND_U64);

   return total;
}

static inline void set_pending(rt_wakeable_t *wake)
{
   assert(!wake->pending);
   assert(!wake->delayed);
   wake->pending = true;
}

static void deltaq_insert_proc(rt_model_t *m, uint64_t delta, rt_proc_t *proc)
{
   if (delta == 0) {
      set_pending(&proc->wakeable);
      deferq_do(&m->procq, async_run_process, proc);
      m->next_is_delta = true;
   }
   else {
      assert(!proc->wakeable.delayed);
      proc->wakeable.delayed = true;

      void *e = tag_pointer(proc, EVENT_PROCESS);
      if (unlikely(relaxed_load(&g_par_active))) {
         nvc_lock(&g_sched_lock);
         heap_insert(m->eventq_heap, m->now + delta, e);
         nvc_unlock(&g_sched_lock);
      }
      else
         heap_insert(m->eventq_heap, m->now + delta, e);
   }
}

static void deltaq_insert_driver(rt_model_t *m, uint64_t delta,
                                 rt_source_t *source)
{
   if (delta == 0) {
      deferq_do(&m->driverq, async_update_driver, source);
      m->next_is_delta = true;
   }
   else {
      void *e = tag_pointer(source, EVENT_DRIVER);
      heap_insert(m->eventq_heap, m->now + delta, e);
   }
}

static void deltaq_insert_pseudo_source(rt_model_t *m, rt_source_t *src)
{
   deferq_do(&m->driverq, async_pseudo_source, src);
   m->next_is_delta = true;
}

static void reset_process(rt_model_t *m, rt_proc_t *proc)
{
   TRACE("reset process %s", istr(proc->name));

   assert(proc->tlab == NULL);
   assert(model_thread(m)->tlab == NULL);   // Not used during reset

   model_thread_t *thread = model_thread(m);
   thread->active_obj = &(proc->wakeable);
   thread->active_scope = proc->scope;

   jit_scalar_t context = {
      .pointer = *mptr_get(proc->scope->privdata)
   };
   jit_scalar_t state = { .pointer = NULL };
   jit_scalar_t result;

   tlab_t tlab = jit_null_tlab(m->jit);

   if (jit_fastcall(m->jit, proc->handle, &result, state, context, &tlab))
      *mptr_get(proc->privdata) = result.pointer;
   else
      m->force_stop = true;

   thread->active_obj = NULL;
   thread->active_scope = NULL;

   // Schedule the process to run immediately
   set_pending(&proc->wakeable);
   deferq_do(&m->procq, async_run_process, proc);
}

static void reset_property(rt_model_t *m, rt_prop_t *prop)
{
   TRACE("reset property %s", istr(prop->name));

   assert(model_thread(m)->tlab == NULL);   // Not used during reset

   model_thread_t *thread = model_thread(m);
   thread->active_obj = &(prop->wakeable);
   thread->active_scope = prop->scope;

   tlab_t tlab = jit_null_tlab(m->jit);

   jit_scalar_t args[] = {
      { .pointer = NULL },
      { .pointer = *mptr_get(prop->scope->privdata) },
      { .integer = -1 },
   }, results[2];

   if (jit_vfastcall(m->jit, prop->handle, args, ARRAY_LEN(args),
                     results, ARRAY_LEN(results), &tlab))
      *mptr_get(prop->privdata) = results[0].pointer;
   else
      m->force_stop = true;

   TRACE("needs %"PRIi64" state bits", results[1].integer);

   mask_init(&prop->state, results[1].integer);
   mask_init(&prop->newstate, results[1].integer);

   mask_set(&prop->state, 0);
   mask_set(&prop->state, results[1].integer - 1);   // Update prev() variables

   thread->active_obj = NULL;
   thread->active_scope = NULL;
}

// Default process eval: run via JIT
static void proc_eval_jit(rt_model_t *m, rt_proc_t *proc)
{
   model_thread_t *thread = model_thread(m);
   assert(thread->tlab != NULL);
   assert(thread->tlab->alloc == 0);

   // Reclaim the previous eval's escaping unconstrained results in O(1) (no-op
   // unless the eval arena is enabled).
   jit_eval_arena_reset();

   thread->active_obj = &(proc->wakeable);
   thread->active_scope = proc->scope;

   jit_scalar_t state = {
      .pointer = *mptr_get(proc->privdata) ?: (void *)-1
   };

   jit_scalar_t result;
   jit_scalar_t context = {
      .pointer = *mptr_get(proc->scope->privdata)
   };

   if (!jit_fastcall(m->jit, proc->handle, &result, state, context,
                     proc->tlab ?: thread->tlab))
      m->force_stop = true;

   if (proc->tlab != NULL && result.pointer == NULL) {
      tlab_release(proc->tlab);
      proc->tlab = NULL;
   }
   else if (proc->tlab == NULL && result.pointer != NULL) {
      TRACE("claiming TLAB for private use (used %u/%u)",
            thread->tlab->alloc, thread->tlab->limit);
      proc->tlab = thread->tlab;
      thread->tlab = tlab_acquire(m->mspace);
   }
   else if (proc->tlab == NULL)
      tlab_reset(thread->tlab);

   thread->active_obj = NULL;
   thread->active_scope = NULL;
}

static void proc_reset_default(rt_proc_t *proc);

static const rt_proc_vtable_t proc_default_vtable = {
   .eval  = proc_eval_jit,
   .reset = proc_reset_default,
};

static void proc_reset_default(rt_proc_t *proc)
{
   proc->vtable = &proc_default_vtable;
}

void proc_set_vtable(rt_proc_t *proc, const rt_proc_vtable_t *vt)
{
   proc->vtable = vt;
}

// Load a compiled state machine .so and swap a process vtable.
// The .so must export:
//   sm_init_mapped(uint8_t **ptrs, int *widths, int n)
//   sm_eval_mapped(void)
//   sm_n_regs (int)
//   sm_reg_names (const char *[])
// Signal pointers are taken from the process scope's signals.
#include <dlfcn.h>

typedef void (*accel_eval_fn)(void);
typedef void (*accel_init_fn)(uint8_t **, int *, int);

#include <pthread.h>
#include <sys/stat.h>

// Background compile state
typedef struct {
   rt_model_t *model;
   char        module[256];
   char        src_file[512];
   char        so_path[512];
} accel_bg_t;

static void *accel_bg_thread(void *arg)
{
   accel_bg_t *bg = arg;

   // Ensure cache directory exists
   char *dir = xstrdup(bg->so_path);
   char *slash = strrchr(dir, '/');
   if (slash) { *slash = '\0'; mkdir(dir, 0755); }
   free(dir);

   // Build paths for intermediate files
   char c_path[512], nvc_path[512];
   snprintf(c_path, sizeof(c_path), "%.*s.c",
            (int)(strlen(bg->so_path) - 3), bg->so_path);
   snprintf(nvc_path, sizeof(nvc_path), "%.*s_nvc.c",
            (int)(strlen(bg->so_path) - 3), bg->so_path);

   // Step 1: Run gen_statemachine (Yosys synthesis + C codegen)
   // Look for gen_statemachine in common locations
   const char *gen_sm = getenv("GEN_STATEMACHINE");
   if (!gen_sm || access(gen_sm, X_OK) != 0) {
      const char *paths[] = {
         "gen_statemachine",
         "/usr/local/src/sv2ghdl/yosys/gen_statemachine",
         NULL
      };
      gen_sm = NULL;
      for (const char **p = paths; *p; p++) {
         if (access(*p, X_OK) == 0) { gen_sm = *p; break; }
      }
      if (!gen_sm) {
         // Try PATH
         gen_sm = "gen_statemachine";
      }
   }

   // Log file for compile output
   char log_path[512];
   snprintf(log_path, sizeof(log_path), "%.*s.log",
            (int)(strlen(bg->so_path) - 3), bg->so_path);

   char cmd[2048];
   snprintf(cmd, sizeof(cmd),
            "%s '%s' '%s' '%s' >>'%s' 2>&1",
            gen_sm, bg->src_file, bg->module, c_path, log_path);

   notef("accel: compiling %s (log: %s)", bg->module, log_path);

   int rc = system(cmd);
   if (rc != 0) {
      warnf("accel: synthesis failed for %s (see %s)", bg->module, log_path);
      free(bg);
      return NULL;
   }

   // Step 2: Compile .so from the NVC-mapped version
   if (access(nvc_path, F_OK) != 0) {
      warnf("accel: no NVC-mapped file generated for %s", bg->module);
      free(bg);
      return NULL;
   }

   const char *accel_cc = getenv("NVC_ACCEL_CC");
   if (!accel_cc) accel_cc = "gcc -g -O3";
   // NVC_ACCEL_SMDUMP: compile in gen_statemachine's sm_dump_comb (all internal
   // nets) so the bridge can dump the accel model's internal state in the REAL
   // sim (see aj_emit_bridge). Clear ~/.cache/nvc/accel when toggling it.
   const char *smdump = getenv("NVC_ACCEL_SMDUMP") ? "-DSM_DUMP" : "";
   snprintf(cmd, sizeof(cmd),
            "%s %s -shared -fPIC -o '%s' '%s' >>'%s' 2>&1",
            accel_cc, smdump, bg->so_path, nvc_path, log_path);

   rc = system(cmd);
   if (rc != 0) {
      warnf("accel: gcc failed for %s (rc=%d)", bg->module, rc);
      free(bg);
      return NULL;
   }

   notef("accel: compiled %s — loading", bg->so_path);

   // Step 3: Load and swap vtable
   accel_load(bg->model, bg->so_path);

   free(bg);
   return NULL;
}

static void accel_bg_compile(rt_model_t *m, const char *module,
                             const char *src_file, const char *so_path)
{
   accel_bg_t *bg = xcalloc(sizeof(accel_bg_t));
   bg->model = m;
   snprintf(bg->module, sizeof(bg->module), "%s", module);
   snprintf(bg->src_file, sizeof(bg->src_file), "%s", src_file);
   snprintf(bg->so_path, sizeof(bg->so_path), "%s", so_path);

   // Try smak for background build, fall back to synchronous
   typedef int (*smak_find_fn)(void);
   typedef int (*smak_connect_fn)(int);
   typedef int (*smak_submit_fn)(int, const char*, const char*, const char*);
   typedef void (*smak_disconnect_fn)(int);

   static void *smak_lib = NULL;
   static int smak_checked = 0;
   if (!smak_checked) {
      smak_lib = dlopen("libsmak-client.so", RTLD_NOW);
      if (!smak_lib)
         smak_lib = dlopen("/usr/local/src/smak/libsmak-client.so", RTLD_NOW);
      smak_checked = 1;
   }

   if (smak_lib) {
      smak_find_fn find = dlsym(smak_lib, "smak_find_server");
      smak_connect_fn conn = dlsym(smak_lib, "smak_connect");
      smak_submit_fn submit = dlsym(smak_lib, "smak_submit");
      smak_disconnect_fn disc = dlsym(smak_lib, "smak_disconnect");

      if (find && conn && submit && disc) {
         int port = find();
         if (port > 0) {
            int fd = conn(port);
            if (fd >= 0) {
               // Build the full command as a single shell command
               char cmd[2048];
               snprintf(cmd, sizeof(cmd),
                        "cd '%s' && gen_statemachine '%s' '%s' '%s' && "
                        "gcc -O2 -shared -fPIC -o '%s' '%s_nvc.c'",
                        getenv("HOME"),
                        bg->src_file, bg->module,
                        bg->so_path,
                        bg->so_path,
                        bg->so_path);
               // Remove .so suffix from the _nvc.c path
               // Actually the path is already correct from gen_statemachine

               submit(fd, bg->so_path, cmd, getenv("HOME"));
               notef("accel: submitted '%s' to smak (port %d)", module, port);
               disc(fd);
               free(bg);
               return;
            }
         }
      }
   }

   // Fallback: synchronous compile
   notef("accel: compiling module '%s' from %s (synchronous)", module, src_file);
   accel_bg_thread(bg);
}

// Per-process acceleration binding
typedef struct {
   rt_proc_vtable_t vtable;
   accel_eval_fn    eval;
   void            *dl_handle;
} accel_binding_t;

static void proc_eval_accel(rt_model_t *m, rt_proc_t *proc)
{
   // The vtable is the first field of accel_binding_t, so we can
   // recover the binding from the vtable pointer
   const accel_binding_t *binding =
      (const accel_binding_t *)proc->vtable;
   binding->eval();
}

bool accel_load(rt_model_t *m, const char *so_path)
{
   void *dl = dlopen(so_path, RTLD_NOW);
   if (!dl) {
      warnf("accel: cannot load %s: %s", so_path, dlerror());
      return false;
   }

   accel_eval_fn eval = dlsym(dl, "sm_eval_mapped");
   accel_init_fn init = dlsym(dl, "sm_init_mapped");
   int *n_regs = dlsym(dl, "sm_n_regs");
   const char **reg_names = dlsym(dl, "sm_reg_names");

   if (!eval || !init || !n_regs || !reg_names) {
      warnf("accel: %s missing sm_eval_mapped/sm_init_mapped/sm_n_regs/sm_reg_names",
            so_path);
      dlclose(dl);
      return false;
   }

   notef("accel: loaded %s (%d registers)", so_path, *n_regs);

   // A module that synthesised to zero sequential state has nothing to
   // accelerate (it is pure wiring / structural). Activating it would just
   // swap out an arbitrary process for a no-op eval — historically this grabbed
   // the testbench clock generator and stalled the whole run at time 0. Decline.
   if (*n_regs == 0) {
      notef("accel: %s has no registers — nothing to accelerate, staying in nvc",
            so_path);
      dlclose(dl);
      return false;
   }

   // Map signals: walk the scope tree, match register names to signals
   rt_scope_t *root = root_scope(m);
   uint8_t **ptrs = xcalloc_array(*n_regs, sizeof(uint8_t *));
   int *widths = xcalloc_array(*n_regs, sizeof(int));
   int mapped = 0;

   // Walk all scopes and their signals
   for (int ci = 0; ci < root->children.count; ci++) {
      rt_scope_t *child = root->children.items[ci];
      for (int si = 0; si < child->signals.count; si++) {
         rt_signal_t *sig = child->signals.items[si];
         const char *sname = istr(tree_ident(sig->where));
         for (int r = 0; r < *n_regs; r++) {
            if (ptrs[r]) continue;
            const char *rn = reg_names[r];
            while (*rn == '_') rn++;
            if (sname && strcasestr(sname, rn)) {
               ptrs[r] = (uint8_t *)sig->shared.data;
               widths[r] = sig->nexus.width;
               mapped++;
               notef("accel:   %s -> %s (%d bits)", reg_names[r], sname, widths[r]);
            }
         }
      }
   }

   if (mapped < *n_regs) {
      warnf("accel: only %d/%d registers mapped — not activating", mapped, *n_regs);
      free(ptrs);
      free(widths);
      dlclose(dl);
      return false;
   }

   init(ptrs, widths, *n_regs);

   // Find the process to swap — use the first always/assign process
   rt_proc_t *target = NULL;
   for (int ci = 0; ci < root->children.count && !target; ci++) {
      rt_scope_t *child = root->children.items[ci];
      for (int pi = 0; pi < child->procs.count; pi++) {
         rt_proc_t *p = child->procs.items[pi];
         if (p->wakeable.kind == W_PROC || p->wakeable.kind == W_ASSIGN) {
            target = p;
            break;
         }
      }
   }

   if (target) {
      accel_binding_t *binding = xcalloc(sizeof(accel_binding_t));
      binding->vtable.eval  = proc_eval_accel;
      binding->vtable.reset = proc_reset_default;
      binding->eval         = eval;
      binding->dl_handle    = dl;

      proc_set_vtable(target, &binding->vtable);
      notef("accel: swapped process %s — acceleration ACTIVE", istr(target->name));
   }
   else {
      warnf("accel: no process found to swap");
   }

   free(ptrs);
   free(widths);
   return target != NULL;
}

// Recover the original Verilog source path from the nvc_verilog_src attribute
// that iverilog's VHDL backend attaches to each translated entity ("file:line").
// Returns the file path (without the :line suffix) in `out`, or false if the
// attribute is absent. Lets --accel feed the original Verilog to yosys rather
// than regenerating it from the elaborated VHDL.
// Look for an NVC_VERILOG_SRC attribute spec directly on this unit's decls.
static bool accel_verilog_src_one(tree_t unit, char *out, size_t outsz)
{
   // Only entity/arch/block carry the NVC_VERILOG_SRC attr and have an I_DECLS
   // item; other scope kinds (e.g. T_COMPONENT) would assert in tree_decls.
   switch (tree_kind(unit)) {
   case T_ENTITY:
   case T_ARCH:
   case T_BLOCK:
      break;
   default:
      return false;
   }

   const int ndecls = tree_decls(unit);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(unit, i);
      if (tree_kind(d) != T_ATTR_SPEC)
         continue;
      if (!icmp(tree_ident(d), "NVC_VERILOG_SRC"))
         continue;

      tree_t val = tree_value(d);
      if (tree_kind(val) != T_STRING)
         return false;

      const unsigned nchars = tree_chars(val);
      size_t j = 0;
      for (unsigned k = 0; k < nchars && j + 1 < outsz; k++) {
         ident_t cid = tree_ident(tree_ref(tree_char(val, k)));
         out[j++] = ident_char(cid, 1);   // 'x' enum literal -> x
      }
      out[j] = '\0';

      char *colon = strrchr(out, ':');   // drop the trailing :line
      if (colon != NULL)
         *colon = '\0';

      return j > 0;
   }
   return false;
}

// Recover the original Verilog source file recorded by sv2vhdl as an
// NVC_VERILOG_SRC attribute.  The attribute is emitted on the entity, but
// accel sees the architecture, so check the architecture's primary unit too.
static bool accel_verilog_src(tree_t unit, char *out, size_t outsz)
{
   if (unit == NULL)
      return false;

   if (accel_verilog_src_one(unit, out, outsz))
      return true;

   if (tree_kind(unit) == T_ARCH) {
      tree_t prim = tree_primary(unit);
      if (prim != NULL && accel_verilog_src_one(prim, out, outsz))
         return true;
   }

   return false;
}

// Read the NVC_VERILOG_PARAMS attribute ("name=value name=value"), emitted by
// tgt-vhdl, so --accel re-synthesizes with the elaboration's actual generics.
static bool accel_verilog_params_one(tree_t unit, char *out, size_t outsz)
{
   const int ndecls = tree_decls(unit);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(unit, i);
      if (tree_kind(d) != T_ATTR_SPEC)
         continue;
      if (!icmp(tree_ident(d), "NVC_VERILOG_PARAMS"))
         continue;
      tree_t val = tree_value(d);
      if (tree_kind(val) != T_STRING)
         return false;
      const unsigned nchars = tree_chars(val);
      size_t j = 0;
      for (unsigned k = 0; k < nchars && j + 1 < outsz; k++)
         out[j++] = ident_char(tree_ident(tree_ref(tree_char(val, k))), 1);
      out[j] = '\0';
      return j > 0;
   }
   return false;
}

static bool accel_verilog_params(tree_t unit, char *out, size_t outsz)
{
   if (unit == NULL)
      return false;
   if (accel_verilog_params_one(unit, out, outsz))
      return true;
   if (tree_kind(unit) == T_ARCH) {
      tree_t prim = tree_primary(unit);
      if (prim != NULL && accel_verilog_params_one(prim, out, outsz))
         return true;
   }
   return false;
}

// ===================================================================
// JIT subtree acceleration  (NVC_ACCEL_JIT=1)
//
// cxxrtl-style: synthesize a whole RTL subtree (flattened) to native code
// via gen_statemachine, then reroute that subtree's process evaluation to it.
// State lives INSIDE the compiled model; only the subtree's top ports (ins &
// outs) are bridged. Because we run JIT, post-elaboration, the port signals
// already exist at known addresses — so the bridge BAKES those addresses in,
// with no load-time name mapping. logic3d<->bit is the only adaptation.
// ===================================================================

void x_deposit_signal(sig_shared_t *ss, uint32_t offset, int32_t count,
                      void *values);   // forward (defined later in this file)
void x_force(sig_shared_t *ss, uint32_t offset, int32_t count, void *values);
void deposit_signal(rt_model_t *m, rt_signal_t *s, const void *values,
                    int offset, size_t count);   // immediate (blocking) deposit

// One accel model per run is enough for the bet; rerouted procs all call this.
// One installed accel subtree. The vtable is the FIRST field so a rerouted proc
// recovers its chunk via (aj_chunk_t *)proc->vtable (the accel_binding_t
// pattern). Each chunk has its own compiled eval/state/reset and its own
// deferred-output table, so multiple chunks can be live at once.
typedef struct {
   char          name[64];     // lowercased port name, e.g. "a_data"
   bool          is_output;
   int           width;        // bits (sub-elements)
   int           elem;         // bytes per sub-element (logic3d = natural)
   uint8_t      *data;         // shared.data (read inputs)
   rt_signal_t  *sig;          // signal (force outputs via force_signal)
} aj_pin_t;

// NVC_ACCEL_HANDOFF: one packed chunk-to-chunk edge. When a chunk output's
// only readers are other chunks' rerouted procs, the producer writes its
// PACKED value straight into each consumer's packed in_live field (same bit-
// at-position-b layout on both sides — no logic3d translation in either
// direction, no deposit_signal), flags the consumer's ext_chg and schedules
// one of its procs for the next delta (deposit-equivalent wake semantics).
typedef struct {
   int            ord;       // producer output ordinal
   void          *dst;       // consumer packed input field (in_live._x)
   unsigned char *ext_chg;   // consumer external-change flag (in chunk state)
   rt_proc_t     *wake;      // a rerouted proc of the consumer to schedule
   unsigned       nbytes;    // packed bytes (8 scalar / 4*limbs wide)
   void          *stage;     // staged value — applied to dst BETWEEN deltas
   bool           dirty;     // stage holds a value awaiting application
} aj_hoff_edge_t;

// bridged pin summary kept for the post-install handoff link pass (only pins
// the bridge actually bound, in bridge ordinal order)
typedef struct {
   void        *data;        // input: signal shared.data the pin reads
   rt_signal_t *sig;         // output: driven signal
   int          width;
   int          elem;
} aj_bpin_t;

struct _aj_chunk {
   rt_proc_vtable_t vtable;          // FIRST — recover chunk from proc->vtable
   void           (*eval)(void *, void **);  // .so's accel_eval(state, bindtab)
   void           (*reset)(void *);          // this .so's accel_reset(state)
   void            *state;           // per-chunk state (sized by accel_state_size)
   void            *dl;              // dlopen handle
   rt_scope_t      *scope;           // installed subtree root
   aj_defer_out_t  *defer_outs;      // per-chunk (was the single m->aj_defer_*)
   unsigned         defer_count;
   bool             defer_pending;
   void           **bindtab;         // per-run address table (the .so's AJB -> here)
   uint8_t          in_sel, out_sel; // decoupled bank-select registers (later)
   int              order;           // topological eval order (later)
   // NVC_ACCEL_HANDOFF (filled by aj_link_handoff after all chunks install)
   aj_bpin_t       *b_in;            // bridged inputs, bridge ordinal order
   aj_bpin_t       *b_out;           // bridged outputs, output ordinal order
   int              n_bin, n_bout;
   uint8_t         *hoff_flags;      // per-output ord: 1 = handoff (skip deposit)
   uint8_t         *hin_flags;       // per-input ord: 1 = handoff-fed (never
                                     // translate from logic3d — the deposit is
                                     // bypassed so those bytes are stale; the
                                     // producer's poke is the only writer)
   aj_hoff_edge_t  *hoff_edges;
   int              hoff_nedges;
   // post-link respecialization inputs: everything needed to RE-EMIT this
   // chunk's bridge with the link results (hoff/hin flags, VERIFY, clocking
   // mode) baked in as constants, compile it, and swap eval — the vtable
   // regenerated to reflect the object's current state.
   char            *rs_bridge;       // original bridge .c path
   char            *rs_dutc;         // model .c the bridge #includes
   char            *rs_top;          // subtree name
   aj_pin_t        *rs_pins;         // full pin table copy
   int              rs_npins;
   aj_pin_t         rs_clk, rs_rst;
   bool             rs_have_rst;
   unsigned long    rs_state_size;   // sanity check across the swap
   // per-delta eval dedup (see aj_proc_eval)
   uint64_t         last_eval_now;
   int              last_eval_iter;
   uint64_t         live_out_mask[4]; // dead-output pruning (re-applied on respec)
   // Serializes eval of THIS chunk under NVC_PARALLEL_PROCS: the chunk's .so
   // state is mutable, so two workers must never run the same chunk's eval at
   // once. Held across the dedup check + eval; different chunks have distinct
   // locks so they still run concurrently. (No-op cost in the serial path.)
   nvc_lock_t       eval_lock;
};

static rt_model_t *g_aj_model     = NULL;  // the model (for deposit_signal)

// Comb-at-edge staging: an accel chunk computes the comb consequence of a
// clock edge IN the edge delta, but the interpreter only exposes it two
// deltas later (flop NBA at end-of-edge-delta -> comb process runs next
// delta -> its assignment is visible the delta after). Deposit immediately
// and a gated-clock late-commit in the gap samples it a cycle early (the
// dec<->exu race, via exu's comb flush outputs). Stage such changes and
// apply them at END_OF_PROCESSES of the FOLLOWING delta: bytes land at
// end of delta N+1, readers wake and same-delta scanners see them from
// delta N+2 -- interp-exact.
typedef struct {
   rt_signal_t   *sig;
   unsigned char *buf;
   size_t         bufsz;
   int            width;
   bool           armed;   // false = staged this delta; true = apply next
} aj_stage2_t;
static aj_stage2_t *g_aj_stage2 = NULL;
static int g_aj_stage2_n = 0, g_aj_stage2_cap = 0;
// The stage2 list persists across deltas (the two-phase armed logic), so it
// can't be per-thread. Under NVC_PARALLEL_PROCS the bridge calls put/cancel
// from concurrent worker evals; serialize them. aj_apply_stage2() runs only at
// the post-eval barrier (thread 0, workers idle), so it needs no lock.
static nvc_lock_t g_aj_stage2_lock = 0;

static void aj_stage2_put(rt_signal_t *sig, const void *buf, size_t bufsz,
                          int width)
{
   nvc_lock(&g_aj_stage2_lock);
   for (int i = 0; i < g_aj_stage2_n; i++)
      if (g_aj_stage2[i].sig == sig) {      // latest value wins, keep phase
         assert(g_aj_stage2[i].bufsz == bufsz);
         memcpy(g_aj_stage2[i].buf, buf, bufsz);
         nvc_unlock(&g_aj_stage2_lock);
         return;
      }
   if (g_aj_stage2_n == g_aj_stage2_cap) {
      g_aj_stage2_cap = g_aj_stage2_cap ? g_aj_stage2_cap * 2 : 32;
      g_aj_stage2 = xrealloc_array(g_aj_stage2, g_aj_stage2_cap,
                                   sizeof(aj_stage2_t));
   }
   aj_stage2_t *e = &g_aj_stage2[g_aj_stage2_n++];
   e->sig = sig; e->bufsz = bufsz; e->width = width; e->armed = false;
   e->buf = xmalloc(bufsz);
   memcpy(e->buf, buf, bufsz);
   nvc_unlock(&g_aj_stage2_lock);
}

static void aj_stage2_cancel(rt_signal_t *sig)
{
   nvc_lock(&g_aj_stage2_lock);
   for (int i = 0; i < g_aj_stage2_n; i++)
      if (g_aj_stage2[i].sig == sig) {
         free(g_aj_stage2[i].buf);
         g_aj_stage2[i] = g_aj_stage2[--g_aj_stage2_n];
         nvc_unlock(&g_aj_stage2_lock);
         return;
      }
   nvc_unlock(&g_aj_stage2_lock);
}

static void aj_apply_stage2(rt_model_t *m)
{
   for (int i = 0; i < g_aj_stage2_n; ) {
      aj_stage2_t *e = &g_aj_stage2[i];
      if (e->armed) {
         deposit_signal(m, e->sig, e->buf, 0, e->width);
         free(e->buf);
         *e = g_aj_stage2[--g_aj_stage2_n];
      }
      else {
         e->armed = true;
         i++;
      }
   }
}
// Per-thread: the chunk whose eval is running on THIS worker. Under
// NVC_PARALLEL_PROCS each worker runs a different chunk, and the .so's bridge
// callbacks (aj_out/aj_poke/aj_stage2) recover their chunk from here with no
// model pointer, so it must be indexed by thread_id(), not a single global.
static aj_chunk_t *g_aj_cur_chunk[MAX_THREADS];  // chunk running per worker

// NVC_ACCEL_VERIFY: run the accel .so as a PASSIVE companion of the interpreter
// (do NOT reroute — the interpreter drives the real sim), and at end of each
// time step compare every bridge output against the settled interpreted value.
// Turns the accel path into a per-net differential oracle: it reports the exact
// net + time where the compiled model first diverges from the reference.
static int         g_aj_verify   = 0;       // int (not bool): the bridge reads it via AJB
static bool        g_aj_verify_skipx = false;  // NVC_ACCEL_VERIFY_X: skip interp-X elems
static bool        g_aj_vcompare = false;   // this pass compares (else just advances state)
static aj_chunk_t *g_aj_vchunks[64];
static int         g_aj_nvchunks = 0;
static int         g_aj_vreports = 0;

// NVC_FORK_AT: fork-and-test checkpointing. On reaching the target simulation
// time (at a settled END_TIME_STEP), fork() snapshots the ENTIRE simulation —
// heap, signals, the dlopen'd accel .so's and JIT code all come along copy-on-
// write. Each of NVC_FORK_TESTS children runs FORWARD from that state (to
// --stop-time); the parent WAITS between them, so it never advances and can
// spawn every test from the same good state. Reach an expensive state once, then
// probe forward from it cheaply and repeatably. Needs a single-threaded sim
// (NVC_PARALLEL_PROCS unset — the default) so fork() at the barrier is clean.
static int64_t     g_fork_at     = -2;      // fs; -2 = not parsed, -1 = disabled
static int         g_fork_tests  = 1;       // children spawned per checkpoint
static int         g_fork_child  = 0;       // 0 = root/parent; else 1+iter in a child
static bool        g_forked      = false;   // checkpoint already taken

static int64_t parse_fork_time(const char *s)
{
   unsigned base; char unit[4];
   if (s == NULL || sscanf(s, "%u%3s", &base, unit) != 2) return -1;
   uint64_t mult;
   if      (!strcmp(unit, "fs")) mult = 1;
   else if (!strcmp(unit, "ps")) mult = 1000;
   else if (!strcmp(unit, "ns")) mult = 1000000;
   else if (!strcmp(unit, "us")) mult = 1000000000;
   else if (!strcmp(unit, "ms")) mult = 1000000000000ULL;
   else return -1;
   return (int64_t)(base * mult);
}

// Any element where the interpreter value is DEFINED (logic3d 2/3 = '0'/'1')
// disagrees with the accel value (which is always 2/3). Interpreter metavalues
// (U/X/Z/W/...) are don't-care — the .so only models 2-state, so skip them.
static bool aj_verify_diff(const unsigned char *ip, const unsigned char *ap,
                           size_t valuesz, int width)
{
   const int es = (width > 0) ? (int)(valuesz / width) : 1, e = es > 0 ? es : 1;
   const int n  = (width > 0) ? width : (int)valuesz;
   // Compare the VALUE bit (bit0) of every element. logic3d carries the 2-state
   // value in bit0 regardless of the unknown/strength bits, and that value bit is
   // exactly what a 2-state sim (Verilator) computes — so this is a Verilator-
   // equivalent comparison. (An earlier version skipped interp metavalues, which
   // HID real divergences on nets that read X in the 4-state interpreter but whose
   // value bit is still the true 2-state result.) NVC_ACCEL_VERIFY_X restores the
   // old metavalue-skipping behaviour for designs that genuinely traffic in X.
   for (int i = 0; i < n; i++) {
      const unsigned char iv = ip[(size_t)i * e];
      if (g_aj_verify_skipx && iv != 2 && iv != 3) continue;   // opt-out: skip X
      if ((iv & 1) != (ap[(size_t)i * e] & 1)) return true;
   }
   return false;
}

static void aj_proc_eval(rt_model_t *m, rt_proc_t *proc)
{
   // Recover this proc's chunk from its vtable (first field), establish the
   // active-process context (run_process does not — only the default JIT eval
   // does, and the bridge needs it for deposit_signal()/AJ_OUT), run the chunk.
   aj_chunk_t *chunk = (aj_chunk_t *)proc->vtable;
   // Per-delta dedup: EVERY rerouted proc of the chunk wakes on a boundary
   // event and would re-eval the whole chunk — hundreds of identical evals
   // per delta for a whole-core chunk, each paying the full input scan
   // (aj_scan_inputs was 43% of dhrystone sim time). A repeat eval in the
   // SAME (time, iteration) can never observe different inputs: interpreter
   // driver updates are never same-delta-visible, and accel deposits/pokes/
   // stage2 wake their consumers in a LATER delta. VERIFY doesn't reroute,
   // so this path only runs in active mode.
   const int tid = thread_id();
   // Atomic dedup CLAIM. A whole-core chunk is rerouted to hundreds of procs
   // (one per boundary input) that all wake the same delta; the dedup collapses
   // them to ONE eval. Under NVC_PARALLEL_PROCS those duplicates scatter across
   // workers, so the claim must be atomic — but the lock is held ONLY for the
   // tiny (now,iter) test-and-set, NOT the eval. The sole claim winner then
   // evals lock-free (different chunks run concurrently); the hundreds of losers
   // take the nanosecond critical section and return, so there is no spinning on
   // the expensive eval. (Serial path: one uncontended lock, negligible.)
   nvc_lock(&chunk->eval_lock);
   const bool mine = !(chunk->last_eval_now == (uint64_t)m->now
                       && chunk->last_eval_iter == m->iteration);
   if (mine) {
      chunk->last_eval_now  = (uint64_t)m->now;
      chunk->last_eval_iter = m->iteration;
   }
   nvc_unlock(&chunk->eval_lock);
   if (!mine)
      return;   // another worker owns this chunk's eval this delta
   model_thread_t *thread = model_thread(m);
   rt_wakeable_t *save_obj   = thread->active_obj;
   rt_scope_t    *save_scope = thread->active_scope;
   aj_chunk_t    *save_chunk = g_aj_cur_chunk[tid];
   thread->active_obj   = &proc->wakeable;
   thread->active_scope = proc->scope;
   g_aj_cur_chunk[tid]  = chunk;
   if (chunk->eval) chunk->eval(chunk->state, chunk->bindtab);
   g_aj_cur_chunk[tid]  = save_chunk;
   thread->active_obj   = save_obj;
   thread->active_scope = save_scope;
}


// NVC_ACCEL_BANK 2-bank deferred output. One per bridged output; `defer`
// distinguishes the bank-switched outputs (staged into `shadow`, published by
// the post-dispatch swap) from the fall-back outputs (deposited immediately).
struct _aj_defer_out {
   rt_nexus_t    *nexus;      // consumer-visible target nexus (after port hop)
   unsigned char *eff;        // nexus_effective (cached at install)
   unsigned char *last;       // nexus_last_value (cached at install)
   size_t         valuesz;    // width * elem bytes
   unsigned char *shadow;     // staged value (malloc valuesz) — NULL if !defer
   bool           cache_event;
   bool           defer;      // bank-switch this output (vs deposit_signal)
   bool           dirty;      // shadow staged this cycle, awaiting swap
   bool           verify_flagged;  // NVC_ACCEL_VERIFY: already reported diverged
   bool           off_edge;   // seen changing on a non-posedge delta => Mealy/
                              // combinational => never route through NBA region
};

// NVC_ACCEL_VERIFY report: compact logic3d-bytes -> value hex (bit0 of each
// element) + the net name and sim time, once per diverging output.
static void aj_verify_report(void *sigp, const unsigned char *interp,
                             const unsigned char *accel, size_t valuesz, int width)
{
   if (g_aj_vreports++ >= 100) {
      if (g_aj_vreports == 101)
         notef("accel-verify: further divergences suppressed (cap 100)");
      return;
   }
   const int es = (width > 0) ? (int)(valuesz / width) : 1, e = es > 0 ? es : 1;
   uint64_t iv = 0, av = 0;
   const int nb = width <= 64 ? width : 64;
   // logic3d bytes are stored MSB-first: byte (width-1-b) carries bit b.
   for (int b = 0; b < nb; b++) {
      if (interp[(size_t)(width - 1 - b) * e] & 1) iv |= (uint64_t)1 << b;
      if (accel [(size_t)(width - 1 - b) * e] & 1) av |= (uint64_t)1 << b;
   }
   char tm[32]; fmt_time_r(tm, sizeof tm, g_aj_model->now, "");
   notef("accel-verify: %s+%d  %s  interp=0x%"PRIx64" accel=0x%"PRIx64
         "%s  (accel diverges from interp)", tm, g_aj_model->iteration,
         istr(tree_ident(((rt_signal_t *)sigp)->where)), iv, av,
         width > 64 ? " [low 64b]" : "");
}

// Baked into the bridge as AJ_OUT: called once per output with the logic3d
// bytes the chunk computed. Routed to the CURRENTLY-running chunk's deferred-
// output table (g_aj_cur_chunk, set by aj_proc_eval). A deferred output is
// copied into its shadow (the swap publishes it later); a non-deferred output
// falls back to the immediate deposit, exactly as before.
static void aj_out(int ord, void *sigp, const void *buf, int width, int posedge)
{
   rt_model_t *m = g_aj_model;
   aj_chunk_t *c = g_aj_cur_chunk[thread_id()];

   // NVC_ACCEL_VERIFY: passive check — the interpreter drives the net; compare the
   // accel bytes against its settled value and report the first divergence per
   // output. Never deposit (the reference sim must stay golden). Advance passes
   // (g_aj_vcompare == false) just step the .so state per delta so its multi-clock
   // register order matches the rerouted model; only the settle pass compares.
   if (g_aj_verify) {
      if (g_aj_vcompare && c != NULL && ord >= 0
          && (unsigned)ord < c->defer_count) {
         aj_defer_out_t *d = &c->defer_outs[ord];
         // d->eff is BANK-only; read the signal's live value directly (offset 0,
         // matching deposit_signal(sigp, buf, 0, width)).
         const unsigned char *interp = ((rt_signal_t *)sigp)->shared.data;
         if (!d->verify_flagged && aj_verify_diff(interp, buf, d->valuesz, width)) {
            d->verify_flagged = true;
            aj_verify_report(sigp, interp, buf, d->valuesz, width);
            if (g_aj_vreports == 1 && getenv("NVC_ACCEL_SMDUMP") != NULL) {
               // settled-state dump at the FIRST divergence (needs -DSM_DUMP)
               void (*dumpfn)(void *) = dlsym(c->dl, "accel_dump");
               if (dumpfn != NULL) dumpfn(c->state);
            }
         }
      }
      return;
   }
   if (c != NULL && ord >= 0 && (unsigned)ord < c->defer_count
       && c->defer_outs[ord].defer) {
      aj_defer_out_t *d = &c->defer_outs[ord];
      // A deferred output that CHANGES on a non-posedge (combinational settle)
      // delta is Mealy: the swap only publishes at the posedge, so the post-edge
      // re-settle would be lost. Detect it and permanently fall back to the
      // immediate deposit for that output (registered outputs never change off
      // the edge, so this never fires for them — e.g. churn's y).
      if (!posedge && !cmp_bytes(d->eff, buf, d->valuesz)) {
         d->defer = false;
         d->dirty = false;
         deposit_signal(m, (rt_signal_t *)sigp, buf, 0, width);
         return;
      }
      memcpy(d->shadow, buf, d->valuesz);
      d->dirty = true;
      c->defer_pending = true;
   }
   else {
      // Route genuine flop-Q outputs through nvc's non-blocking (NBA) region —
      // the SAME region native Verilog `<=` schedules into (--std=2040). Every
      // flop in the design, interpreted or accel-chunk, then commits in one
      // consistent region, so a consumer's flop reads this producer's PRE-edge
      // value this delta and the new value only after the NBA region. That is
      // exact flop-to-flop timing across a chunk boundary and fixes the
      // cross-chunk gated-clock race (a consumer no longer captures a producer's
      // post-edge Q one delta early when both sides are accel). The blocking
      // deposit only approximated this via a next-delta wake, which drifted once
      // the consumer sampled at its own later gated-clock delta.
      //
      // ONLY registered outputs may move to NBA: a Mealy/combinational output
      // must stay visible in the active region. `posedge` alone is per-EVAL, not
      // per-output (a comb output can change in a posedge delta), so classify at
      // runtime — any output ever seen changing on a NON-posedge delta is Mealy
      // and pinned to the immediate deposit (mirrors the deferred-bank fallback
      // at the top of this function).
      // The bridge bakes the per-output cone class (gen_statemachine's
      // sm_comb_outputs table) into bit2 of the posedge argument; bit0 is the
      // eval's edge flag. Under NVC_ACCEL_NBA:
      //   reg-only output @edge  -> NBA region (interp flop `<=`: visible d1)
      //   comb output     @edge  -> 2-delta stage (interp comb-of-edge: d2)
      //   anything off-edge      -> immediate (interp active-region comb)
      static int _nba = -1, _st2 = -1;
      if (_nba < 0) {
         _nba = getenv("NVC_ACCEL_NBA") ? 1 : 0;
         const char *s2 = getenv("NVC_ACCEL_STAGE2");
         _st2 = s2 ? atoi(s2) : _nba;   // default: follow NBA; 0 forces off
      }
      const int pe = posedge & 1, combcls = posedge & 4;
      if (_nba && pe && !combcls)
         sched_deposit(m, (rt_signal_t *)sigp, buf, 0, width, 0, true /*nonblock*/);
      else if (_st2 && pe && combcls && c != NULL && ord >= 0
               && (unsigned)ord < c->defer_count)
         aj_stage2_put((rt_signal_t *)sigp, buf,
                       c->defer_outs[ord].valuesz, width);
      else {
         if (_st2) aj_stage2_cancel((rt_signal_t *)sigp);  // newer value wins
         deposit_signal(m, (rt_signal_t *)sigp, buf, 0, width);
      }
   }
}

// NVC_ACCEL_HANDOFF: producer-side poke. Called from the generated bridge for
// an output whose hoff flag is set, ONLY when the packed value changed (the
// bridge gates on o_prev — required for fixpoint termination on cross-chunk
// combinational cycles, mirroring deposit's wake-on-change). The value is
// STAGED, not applied: writing the consumer's in_live immediately would make
// it visible within the CURRENT delta — a consumer evaluating later in the
// same posedge queue would capture the producer's post-edge Q one cycle early
// (deposit_signal's update lands at the next delta boundary; ours must too).
// aj_apply_pokes() publishes stage->dst after the delta's procq drains.
static bool g_aj_hoff_pending = false;

static void aj_poke(int ord, const void *packed, unsigned nbytes)
{
   aj_chunk_t *c = g_aj_cur_chunk[thread_id()];
   if (c == NULL) return;
   // c's eval is serialized by eval_lock, so this chunk's e->stage/e->dirty
   // writes are single-threaded; only the shared pending flag needs atomicity
   // (workers running OTHER chunks may set it concurrently).
   for (int i = 0; i < c->hoff_nedges; i++) {
      aj_hoff_edge_t *e = &c->hoff_edges[i];
      if (e->ord != ord) continue;
      memcpy(e->stage, packed, nbytes < e->nbytes ? nbytes : e->nbytes);
      e->dirty = true;
      atomic_store(&g_aj_hoff_pending, true);
   }
}

// Between-deltas application point (called from model_cycle after the procq
// drain): publish staged pokes into consumer in_live fields, flag ext_chg and
// schedule one proc per consumer for the NEXT delta — exactly the visibility
// and wake semantics the bypassed deposit_signal would have given.
static void aj_apply_pokes(rt_model_t *m)
{
   if (!atomic_load(&g_aj_hoff_pending)) return;
   atomic_store(&g_aj_hoff_pending, false);
   for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
      aj_chunk_t *c = m->aj_chunks[ci];
      for (int i = 0; i < c->hoff_nedges; i++) {
         aj_hoff_edge_t *e = &c->hoff_edges[i];
         if (!e->dirty) continue;
         e->dirty = false;
         memcpy(e->dst, e->stage, e->nbytes);
         *e->ext_chg = 1;
         if (!e->wake->wakeable.pending)
            deltaq_insert_proc(m, 0, e->wake);
      }
   }
}

// NVC_ACCEL_VERIFY: run every passive companion chunk against the interpreted
// inputs/state. Called per delta with compare=false (advance the .so state so its
// multi-clock register order tracks the rerouted model) and once per settled time
// step with compare=true (aj_out then diffs each output vs the interpreter and
// flags divergences). Mirrors aj_proc_eval's context; nothing is ever deposited.
static void aj_verify_step(rt_model_t *m, bool compare)
{
   model_thread_t *thread = model_thread(m);
   const int tid = thread_id();   // VERIFY runs on thread 0, but stay per-thread
   rt_scope_t *save_scope = thread->active_scope;
   aj_chunk_t *save_chunk = g_aj_cur_chunk[tid];
   g_aj_vcompare = compare;
   for (int i = 0; i < g_aj_nvchunks; i++) {
      aj_chunk_t *c = g_aj_vchunks[i];
      thread->active_scope = c->scope;
      g_aj_cur_chunk[tid] = c;
      if (c->eval) c->eval(c->state, c->bindtab);
   }
   g_aj_vcompare = false;
   g_aj_cur_chunk[tid] = save_chunk;
   thread->active_scope = save_scope;
}

// NVC_FORK_AT: at a settled time step, fork NVC_FORK_TESTS children that each run
// forward from this exact state; the parent waits between them (holding the
// checkpoint) and never advances. See the g_fork_* comment above.
static void fork_checkpoint(rt_model_t *m)
{
   g_forked = true;
   char tb[32];
   fmt_time_r(tb, sizeof tb, m->now, "");
   // Return to a single thread first: nvc keeps parked pool workers (from
   // elaboration/JIT) that fork() would orphan — a child inheriting the ghosts
   // spins forever. They are lazily recreated in whichever process needs them.
   thread_quiesce_workers();
   notef("fork-checkpoint: reached %s — spawning %d test(s); parent holds this state",
         tb, g_fork_tests);
   for (int i = 0; i < g_fork_tests; i++) {
      fflush(NULL);                          // drain stdio so COW doesn't duplicate it
      const pid_t pid = fork();
      if (pid < 0) {
         warnf("fork-checkpoint: fork failed: %s", strerror(errno));
         return;                             // give up; parent resumes normally
      }
      else if (pid == 0) {
         g_fork_child = i + 1;               // child never re-forks; runs forward
         char ib[16];
         snprintf(ib, sizeof ib, "%d", i);
         setenv("NVC_FORK_ITER", ib, 1);     // per-test probes can key off the iteration
         notef("fork-checkpoint: [test %d/%d pid=%d] advancing from %s",
               i + 1, g_fork_tests, (int)getpid(), tb);
         return;                             // -> run loop advances this child to stop-time
      }
      int status = 0;
      waitpid(pid, &status, 0);              // parent blocks here — does NOT advance
      notef("fork-checkpoint: [test %d/%d] exited (%d); checkpoint at %s intact",
            i + 1, g_fork_tests,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1, tb);
   }
   notef("fork-checkpoint: %d test(s) complete; parent held at %s, not advancing",
         g_fork_tests, tb);
   fflush(NULL);
   _exit(0);                                 // parent never advances past the checkpoint
}

static tree_t aj_scope_ref(rt_scope_t *scope)
{
   if (scope->where == NULL || tree_decls(scope->where) == 0)
      return NULL;
   tree_t hier = tree_decl(scope->where, 0);
   if (tree_kind(hier) != T_HIER)
      return NULL;
   return tree_ref(hier);
}

static void aj_lower(char *dst, const char *src, size_t n)
{
   const char *dot = strrchr(src, '.');     // strip WORK. prefix
   if (dot) src = dot + 1;
   size_t i = 0;
   for (; src[i] && i + 1 < n; i++)
      dst[i] = tolower((unsigned char)src[i]);
   dst[i] = '\0';
}

// Find a port signal by (lowercased) name in this scope or its parent.
static rt_signal_t *aj_find_signal(rt_scope_t *scope, const char *lname)
{
   for (rt_scope_t *s = scope; s != NULL; s = s->parent) {
      for (int i = 0; i < s->signals.count; i++) {
         rt_signal_t *sig = s->signals.items[i];
         char sn[64];
         aj_lower(sn, istr(tree_ident(sig->where)), sizeof sn);
         if (strcmp(sn, lname) == 0)
            return sig;
      }
      // A direct-mapped (collapsed) port — e.g. a clk wired straight through the
      // hierarchy — has NO rt_signal_t in this scope; it is an ALIAS to the
      // parent's signal. find_signal() walks aliases; this clone must too, or a
      // collapsed clk/port is invisible ("no clk port found" on dec).
      for (int i = 0; i < s->aliases.count; i++) {
         rt_alias_t *a = s->aliases.items[i];
         char an[64];
         aj_lower(an, istr(tree_ident(a->where)), sizeof an);
         if (strcmp(an, lname) == 0)
            return a->signal;
      }
      if (s == scope->parent) break;        // only this scope + immediate parent
   }
   return NULL;
}

// Does `t` (walking the subtype chain) have leaf base name `want`?
static bool aj_type_named(type_t t, const char *want)
{
   for (int i = 0; i < 8 && t != NULL; i++) {
      if (type_has_ident(t)) {
         const char *nm = istr(type_ident(t));
         const char *dot = strrchr(nm, '.');
         if (strcasecmp(dot ? dot + 1 : nm, want) == 0) return true;
      }
      if (type_kind(t) != T_SUBTYPE) break;
      t = type_base(t);
   }
   return false;
}

// Can the value-bit bridge marshal this port losslessly? The bridge packs ONE
// value bit (bit0 of each element) per element into a uint64_t, so only logic3d
// (a `natural 0..7` subtype, value in bit0) and the std_logic/bit family (1-byte
// enum, code 2/3) work. A record / integer / real boundary port would be
// silently corrupted (the dec_tlu_ic_diag_pkt class), so a chunk with one must
// stay interpreted — sound, just not accelerated.
static bool aj_marshallable_type(type_t t)
{
   type_t et = type_is_array(t) ? type_elem(t) : t;
   return aj_type_named(et, "LOGIC3D") || aj_type_named(et, "STD_LOGIC")
       || aj_type_named(et, "STD_ULOGIC") || aj_type_named(et, "BIT")
       || aj_type_named(et, "BOOLEAN");
}

// Collect distinct recovered-Verilog source files across the subtree.
static void aj_collect_sources(rt_scope_t *scope, char srcs[][512],
                               int *nsrc, int max)
{
   tree_t r = aj_scope_ref(scope);
   char buf[512];
   if (r != NULL && accel_verilog_src(r, buf, sizeof buf)) {
      bool dup = false;
      for (int i = 0; i < *nsrc; i++)
         if (strcmp(srcs[i], buf) == 0) { dup = true; break; }
      if (!dup && *nsrc < max)
         snprintf(srcs[(*nsrc)++], 512, "%s", buf);
   }
   for (int ci = 0; ci < scope->children.count; ci++)
      aj_collect_sources(scope->children.items[ci], srcs, nsrc, max);
}

// Reroute every process in the subtree to the accel eval (gate makes
// non-posedge calls harmless).
static aj_chunk_t *aj_chunk_new(rt_model_t *m)
{
   if (m->aj_chunk_count == m->aj_chunk_max) {
      m->aj_chunk_max = m->aj_chunk_max ? m->aj_chunk_max * 2 : 4;
      m->aj_chunks = xrealloc_array(m->aj_chunks, m->aj_chunk_max,
                                    sizeof(aj_chunk_t *));
   }
   aj_chunk_t *c = xcalloc(sizeof(aj_chunk_t));
   c->vtable.eval = aj_proc_eval;
   m->aj_chunks[m->aj_chunk_count++] = c;
   return c;
}

// Route every process in a subtree to ITS chunk's vtable (recovered later by
// aj_proc_eval via the vtable-first-field trick).
static void aj_reroute(rt_scope_t *scope, aj_chunk_t *chunk)
{
   for (int pi = 0; pi < scope->procs.count; pi++)
      proc_set_vtable(scope->procs.items[pi], &chunk->vtable);
   for (int ci = 0; ci < scope->children.count; ci++)
      aj_reroute(scope->children.items[ci], chunk);
}

// ---- NVC_FAST_CLK posedge-table dispatch -----------------------------------
// Apply CB to every non-postponed W_PROC wakeable on a nexus' pending list.
// pending is a tagged pointer (util.h): tag==1 -> a single waiter; otherwise an
// rt_pending_t* with a NULL-skipping wake[] array (see wakeup_all).
static void aj_pending_foreach(void *pending,
                               void (*cb)(rt_wakeable_t *, void *), void *ctx)
{
   if (pointer_tag(pending) == 1) {
      rt_wakeable_t *w = untag_pointer(pending, rt_wakeable_t);
      if (w->kind == W_PROC && !w->postponed) cb(w, ctx);
   }
   else if (pending != NULL) {
      rt_pending_t *p = untag_pointer(pending, rt_pending_t);
      for (int i = 0; i < p->count; i++) {
         rt_wakeable_t *w = p->wake[i];
         if (w != NULL && w->kind == W_PROC && !w->postponed) cb(w, ctx);
      }
   }
}

static void aj_guard_notify(rt_model_t *m, rt_nexus_t *n);
static void aj_dissolve_fastclk(rt_model_t *m);

static bool aj_blacklisted(rt_model_t *m, rt_nexus_t *n)
{
   for (unsigned i = 0; i < m->fastclk_nbl; i++)
      if (m->fastclk_bl[i] == n)
         return true;
   return false;
}

static void aj_flag_cb(rt_wakeable_t *w, void *ctx)   { w->fastclk = 1; }
static void aj_unflag_cb(rt_wakeable_t *w, void *ctx) { w->fastclk = 0; }

static void aj_collect_cb(rt_wakeable_t *w, void *ctx)
{
   if (!w->fastclk) return;
   rt_model_t *m = ctx;
   m->fastclk_table[m->fastclk_count++] = container_of(w, rt_proc_t, wakeable);
}

// Upper bound on the number of procs on a nexus' pending list.
static unsigned aj_pending_count(void *pending)
{
   if (pointer_tag(pending) == 1)
      return 1;
   else if (pending != NULL) {
      rt_pending_t *p = untag_pointer(pending, rt_pending_t);
      return p->count;
   }
   return 0;
}

// Build the dispatch table of clk-ONLY processes (statically sensitive to the
// clock nexus and nothing else). A proc also sensitive to rst / another clock /
// driving clk itself (the `clk <= not clk` generator) is filtered out and stays
// on the normal procq — only an optimisation, never a correctness lever.
static void aj_build_fastclk(rt_model_t *m, rt_signal_t *clksig, uint8_t *clkdata)
{
   aj_dissolve_fastclk(m);    // rebuilt per install/candidate — full cleanup
   if (!getenv("NVC_FAST_CLK") && !getenv("NVC_FAST_CLK_AUTO")) return;
   if (clksig->n_nexus != 1) return;        // single-bit clock only
   rt_nexus_t *clkn = &clksig->nexus;

   // Pass 0: provisionally flag every clk-pending proc.
   aj_pending_foreach(clkn->pending, aj_flag_cb, NULL);

   // Pass 1: un-flag any that also appear on an ACTIVE other nexus. A
   // QUIET other nexus (no event within QUIET_FS of now — e.g. released
   // rst) does not disqualify its clk procs: mark it as a GUARD instead;
   // any later event on a guard nexus dissolves the table and falls back
   // to normal dispatch before the wakeup propagates (user directive:
   // settle first, block-dispatch, fall back as needed).
   const uint64_t QUIET_FS = UINT64_C(2000000000);  // 2us: only long-stable
   // nexuses qualify as guards (at the first build only never-toggled ones)
   const bool strict = getenv("NVC_FAST_CLK_STRICT") != NULL;
   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
      if (n == clkn) continue;
      if (aj_pending_count(n->pending) == 0) continue;
      const bool quiet = !strict && !aj_blacklisted(m, n)
         && (n->last_event > m->now || m->now - n->last_event > QUIET_FS);
      if (quiet)
         m->fastclk_nguards++;    // counted now, vtables patched below
      else
         aj_pending_foreach(n->pending, aj_unflag_cb, NULL);
   }

   // Patch each guard nexus's vtable (post-elab vtable hack): a copy whose
   // notify dissolves the table (restoring every vtable) before the event
   // propagates, so member procs fall back to normal queued wakeup.
   if (m->fastclk_nguards > 0) {
      m->fastclk_guard_nx   = xmalloc_array(m->fastclk_nguards, sizeof(rt_nexus_t *));
      m->fastclk_guard_orig = xmalloc_array(m->fastclk_nguards, sizeof(void *));
      m->fastclk_guard_vt   = xmalloc_array(m->fastclk_nguards, sizeof(rt_nexus_vtable_t));
      unsigned gi = 0;
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
         if (n == clkn || aj_pending_count(n->pending) == 0) continue;
         if (strict || aj_blacklisted(m, n)
             || !(n->last_event > m->now || m->now - n->last_event > QUIET_FS))
            continue;
         m->fastclk_guard_nx[gi]   = n;
         m->fastclk_guard_orig[gi] = n->vtable;
         m->fastclk_guard_vt[gi]   = *n->vtable;
         m->fastclk_guard_vt[gi].notify = aj_guard_notify;
         n->vtable = &m->fastclk_guard_vt[gi];
         gi++;
      }
      m->fastclk_nguards = gi;
   }

   // Pass 2: collect the survivors into the table.
   const unsigned maxn = aj_pending_count(clkn->pending);
   if (maxn == 0) return;
   m->fastclk_table = xmalloc_array(maxn, sizeof(rt_proc_t *));
   aj_pending_foreach(clkn->pending, aj_collect_cb, m);

   if (m->fastclk_count == 0) {
      aj_dissolve_fastclk(m);   // unpatch guards, unflag strays
      return;
   }
   m->fastclk_nexus = clkn;
   m->fastclk_data  = clkdata;
   m->fastclk_on    = true;
   notef("accel-jit: NVC_FAST_CLK — %u clk-only proc(s) in posedge table",
         m->fastclk_count);
}

// Tear the posedge table down (guard nexus fired, or install replaced it):
// unflag members so wakeup_one queues them normally again, clear guards.
static void aj_dissolve_fastclk(rt_model_t *m)
{
   if (m->fastclk_table != NULL) {
      for (unsigned i = 0; i < m->fastclk_count; i++)
         m->fastclk_table[i]->wakeable.fastclk = 0;
      free(m->fastclk_table);
      m->fastclk_table = NULL;
   }
   const bool was_on = m->fastclk_on;
   m->fastclk_on = false;
   m->fastclk_count = 0;
   m->fastclk_hit = false;
   // Unflag EVERY stray (failed candidates leave pass-0 flags on procs not
   // in any table; with fastclk_on they would skip wakeups forever).
   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain)
      aj_pending_foreach(n->pending, aj_unflag_cb, NULL);
   for (unsigned i = 0; i < m->fastclk_nguards; i++)
      m->fastclk_guard_nx[i]->vtable = m->fastclk_guard_orig[i];
   free(m->fastclk_guard_nx);   m->fastclk_guard_nx = NULL;
   free(m->fastclk_guard_orig); m->fastclk_guard_orig = NULL;
   free(m->fastclk_guard_vt);   m->fastclk_guard_vt = NULL;
   m->fastclk_nguards = 0;
   if (was_on) {
      if (getenv("NVC_FAST_CLK_AUTO") != NULL)   // redo-as-we-go: re-arm;
         m->fastclk_auto_at = m->now + UINT64_C(500000000);  // +500ns, each
      // rebuild excludes recently-active nexuses so membership converges.
      static unsigned dcount = 0;
      if (dcount++ < 10)
         notef("accel-jit: fast-clk table dissolved (guard event)");
   }
}

// Guard nexus fired: restore all vtables + fall back, then deliver the
// event through the nexus's ORIGINAL notify (vtable now restored).
static void aj_guard_notify(rt_model_t *m, rt_nexus_t *n)
{
   if (m->fastclk_nbl == m->fastclk_blmax) {
      m->fastclk_blmax = m->fastclk_blmax ? m->fastclk_blmax * 2 : 64;
      m->fastclk_bl = xrealloc_array(m->fastclk_bl, m->fastclk_blmax,
                                     sizeof(rt_nexus_t *));
   }
   m->fastclk_bl[m->fastclk_nbl++] = n;
   aj_dissolve_fastclk(m);
   n->vtable->notify(m, n);
}

// True iff every static waiter on this output nexus is a clk-only fast-clk
// proc (so it is dispatched off the clock edge and reads the OLD effective
// value before the swap). Any non-W_PROC waiter, or a W_PROC that did not
// survive the clk-only filter (fastclk==0), means a same-delta reader could
// observe the new value -> not deferrable.
static bool aj_out_pending_ok(void *pending)
{
   if (pointer_tag(pending) == 1) {
      rt_wakeable_t *w = untag_pointer(pending, rt_wakeable_t);
      return w->kind == W_PROC && w->fastclk;
   }
   else if (pending != NULL) {
      rt_pending_t *p = untag_pointer(pending, rt_pending_t);
      for (int i = 0; i < p->count; i++) {
         rt_wakeable_t *w = p->wake[i];
         if (w == NULL) continue;
         if (w->kind != W_PROC || !w->fastclk) return false;
      }
   }
   return true;
}

// No static waiter at all on this nexus (used for the chunk-side output nexus,
// which must only feed its output port — anything reading it directly would see
// a stale value since the swap writes the consumer-side nexus).
static bool aj_pending_empty(void *pending)
{
   if (pointer_tag(pending) == 1) return false;
   if (pending == NULL) return true;
   rt_pending_t *p = untag_pointer(pending, rt_pending_t);
   for (int i = 0; i < p->count; i++)
      if (p->wake[i] != NULL) return false;
   return true;
}

// Decide whether a chunk output may be bank-switched (deferred). Conservative
// smallest-correct gate: NVC_FAST_CLK active, NVC_ACCEL_BANK not disabled,
// single full-width nexus, no downstream port fan-out, and every static
// reader a clk-only fast-clk proc. Anything else falls back to deposit_signal.
// Returns the consumer-visible nexus to bank-switch into, or NULL to decline.
// An accel chunk output always feeds its parent via an output port, so we follow
// at most ONE direct (no-conversion) port hop to the nexus the readers actually
// read, and bank-switch into THAT. Conservative: single full-width nexus on both
// sides, no further fan-out, all readers clk-only fast-clk procs.
static rt_nexus_t *aj_classify_output(rt_model_t *m, rt_signal_t *sig)
{
   const bool dbg = getenv("NVC_ACCEL_BANK_DBG") != NULL;
   #define BANK_DECLINE(why) do { \
      if (dbg) notef("accel-jit: bank decline %s — " why, \
                     istr(tree_ident(sig->where))); return NULL; } while (0)

   if (!m->fastclk_on) return NULL;
   // Opt-in: on the single-chunk / interpreted-consumer path the deferred bank
   // is bit-identical but net-neutral (the consumer reads a fixed effective
   // location, so one copy is unavoidable and deposit_signal already does it).
   // Its payoff is chunk-to-chunk handoff (a future, compiled consumer that can
   // read a bank-select bit with zero copy). Off unless NVC_ACCEL_BANK=1.
   const char *bank = getenv("NVC_ACCEL_BANK");
   if (bank == NULL || atoi(bank) == 0) return NULL;
   if (sig->n_nexus != 1) BANK_DECLINE("multi-nexus chunk output");
   rt_nexus_t *n = &sig->nexus;

   if (n->outputs != NULL) {
      // follow one direct output-port hop to the consumer-visible nexus
      rt_source_t *o = n->outputs;
      if (o->chain_output != NULL)       BANK_DECLINE("multiple output fan-out");
      if (o->tag != SOURCE_PORT)         BANK_DECLINE("non-port output source");
      if (o->u.port.conv_func != NULL)   BANK_DECLINE("output port has conversion");
      if (!aj_pending_empty(n->pending)) BANK_DECLINE("chunk-side nexus has readers");
      n = o->u.port.output;              // the parent / consumer-visible nexus
      if (n->signal->n_nexus != 1)       BANK_DECLINE("multi-nexus consumer signal");
      if (n->outputs != NULL)            BANK_DECLINE("consumer nexus fans out further");
   }
   if (!aj_out_pending_ok(n->pending))   BANK_DECLINE("non-clk-only reader on consumer");
   return n;
   #undef BANK_DECLINE
}

// ---- NVC_ACCEL_HANDOFF link pass -------------------------------------------
// After all chunks are installed, wire packed chunk-to-chunk edges: for each
// producer output whose consumer-visible nexus is read ONLY by other chunks'
// rerouted procs, resolve each consumer's packed input field (via the .so's
// accel_in_addr) and record an edge; set the producer's per-output hoff flag
// so the bridge pokes packed instead of translating + depositing. Any non-
// chunk reader (interp glue, VCD watch, implicit signal) declines the output
// and it keeps the deposit path. Never runs under VERIFY.

typedef void *(*aj_in_addr_fn)(void *, int, unsigned long *);

static aj_chunk_t *aj_chunk_of_proc(rt_model_t *m, rt_proc_t *p)
{
   for (unsigned i = 0; i < m->aj_chunk_count; i++)
      if ((void *)p->vtable == (void *)m->aj_chunks[i])
         return m->aj_chunks[i];
   return NULL;
}

// Collect the distinct consumer chunks reading `pending` (with one proc each).
// Returns false if ANY reader is not a rerouted chunk proc.
#define AJ_HOFF_MAX_CONS 8
static bool aj_hoff_readers(rt_model_t *m, void *pending,
                            aj_chunk_t **cons, rt_proc_t **wake, int *ncons)
{
   *ncons = 0;
   rt_wakeable_t *one = NULL;
   rt_pending_t  *many = NULL;
   int count = 0;
   if (pointer_tag(pending) == 1) {
      one = untag_pointer(pending, rt_wakeable_t);
      count = 1;
   }
   else if (pending != NULL) {
      many = untag_pointer(pending, rt_pending_t);
      count = many->count;
   }
   for (int i = 0; i < count; i++) {
      rt_wakeable_t *w = one != NULL ? one : many->wake[i];
      if (w == NULL) continue;
      if (w->kind != W_PROC) return false;
      rt_proc_t *p = container_of(w, rt_proc_t, wakeable);
      aj_chunk_t *c = aj_chunk_of_proc(m, p);
      if (c == NULL) return false;
      bool seen = false;
      for (int j = 0; j < *ncons; j++)
         if (cons[j] == c) { seen = true; break; }
      if (!seen) {
         if (*ncons == AJ_HOFF_MAX_CONS) return false;
         cons[*ncons] = c;
         wake[*ncons] = p;
         (*ncons)++;
      }
   }
   return *ncons > 0;
}

static bool aj_emit_bridge(const char *path, const char *dutc,
                           aj_pin_t *pins, int npins, aj_pin_t *clk,
                           aj_pin_t *rst, rt_model_t *m, aj_chunk_t *chunk,
                           bool spec);

// Post-link respecialization: the vtable-hacking principle applied to the
// bridge — the eval pointer is mutable state, so once the link pass fixes the
// handoff wiring, RE-EMIT the bridge with those results (and VERIFY=0) baked
// in as constants, compile it (cached by a hash of the flag vectors), and
// swap chunk->eval. The steady-state code is then straight-line for the
// linked topology: no AJ_HOFF/AJ_HIN tests, no VERIFY deref, handoff-fed
// input pins gone entirely (not even a memcmp). The chunk STATE (aj_cs_t) is
// emitted layout-identically, so the live state carries across the swap.
static void aj_respecialize(rt_model_t *m)
{
   for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
      aj_chunk_t *c = m->aj_chunks[ci];
      if (c->rs_bridge == NULL || c->rs_pins == NULL) continue;
      bool any = false;
      for (int i = 0; i < c->rs_npins && !any; i++)
         if (c->hoff_flags[i] || c->hin_flags[i]) any = true;
      if (!any) continue;   // link changed nothing for this chunk

      uint64_t h = 1469598103934665603ull;   // FNV-1a over both flag vectors
      for (int i = 0; i < c->rs_npins; i++) {
         h = (h ^ c->hoff_flags[i]) * 1099511628211ull;
         h = (h ^ (unsigned)(c->hin_flags[i] << 1)) * 1099511628211ull;
      }
      // spec bakes _coinc as a constant — a mode flip must miss the cache
      h = (h ^ (getenv("NVC_ACCEL_CK_COINCIDENT") != NULL ? 5u : 9u))
         * 1099511628211ull;

      char dir[512];
      snprintf(dir, sizeof dir, "%s", c->rs_bridge);
      char *slash = strrchr(dir, '/');
      if (slash != NULL) *slash = '\0';
      else snprintf(dir, sizeof dir, ".");
      char specc[600], specso[600];
      snprintf(specc,  sizeof specc,  "%s/aj_%s_bridge_spec.c", dir, c->rs_top);
      snprintf(specso, sizeof specso, "%s/aj_%s_spec_%016llx.so", dir, c->rs_top,
               (unsigned long long)h);

      if (!aj_emit_bridge(specc, c->rs_dutc, c->rs_pins, c->rs_npins,
                          &c->rs_clk, c->rs_have_rst ? &c->rs_rst : NULL,
                          m, c, true))
         continue;

      if (getenv("NVC_ACCEL_NO_CACHE") != NULL || access(specso, F_OK) != 0) {
         const char *cc = getenv("NVC_ACCEL_CC");
         if (cc == NULL) cc = "gcc -g -O3";
         const char *smd = getenv("NVC_ACCEL_SMDUMP") ? "-DSM_DUMP" : "";
         char cmd[2048];
         snprintf(cmd, sizeof cmd, "%s %s -shared -fPIC -o '%s' '%s'",
                  cc, smd, specso, specc);
         if (system(cmd) != 0 || access(specso, F_OK) != 0) {
            warnf("accel-jit: respec compile failed for '%s' — keeping generic",
                  c->rs_top);
            continue;
         }
      }

      void *dl = dlopen(specso, RTLD_NOW);
      if (dl == NULL) {
         warnf("accel-jit: respec dlopen: %s", dlerror());
         continue;
      }
      void (*eval)(void *, void **) = dlsym(dl, "accel_eval");
      unsigned long (*ssize)(void)  = dlsym(dl, "accel_state_size");
      if (eval == NULL || ssize == NULL || ssize() != c->rs_state_size) {
         warnf("accel-jit: respec '%s' state-layout mismatch — keeping generic",
               c->rs_top);
         dlclose(dl);
         continue;
      }
      c->eval = eval;   // the swap: dispatch now points at specialized code
      // the specialized .so has its own sm_live_outputs — re-apply the mask
      uint64_t *lom = dlsym(dl, "sm_live_outputs");
      if (lom != NULL) memcpy(lom, c->live_out_mask, sizeof c->live_out_mask);
      notef("accel-jit: respecialized '%s' (link results compiled in)",
            c->rs_top);
   }
}

static void aj_link_handoff(rt_model_t *m)
{
   static bool done = false;
   if (done || getenv("NVC_ACCEL_HANDOFF") == NULL || g_aj_verify)
      return;
   done = true;

   int nlink = 0;
   for (unsigned pi = 0; pi < m->aj_chunk_count; pi++) {
      aj_chunk_t *P = m->aj_chunks[pi];
      if (P->b_out == NULL || P->hoff_flags == NULL) continue;
      for (int ord = 0; ord < P->n_bout; ord++) {
         rt_signal_t *sig = P->b_out[ord].sig;
         if (sig == NULL || sig->n_nexus != 1) continue;
         rt_nexus_t *n = &sig->nexus;
         if (n->outputs != NULL) {
            // one direct output-port hop to the consumer-visible nexus
            // (mirrors aj_classify_output)
            rt_source_t *o = n->outputs;
            if (o->chain_output != NULL || o->tag != SOURCE_PORT
                || o->u.port.conv_func != NULL) continue;
            if (!aj_pending_empty(n->pending)) continue;
            n = o->u.port.output;
            if (n->signal->n_nexus != 1 || n->outputs != NULL) continue;
         }
         const int width = P->b_out[ord].width;
         if ((size_t)n->size * n->width
             != (size_t)width * P->b_out[ord].elem) continue;

         aj_chunk_t *cons[AJ_HOFF_MAX_CONS];
         rt_proc_t  *wake[AJ_HOFF_MAX_CONS];
         int ncons = 0;
         if (!aj_hoff_readers(m, n->pending, cons, wake, &ncons)) continue;

         const unsigned pbytes = width <= 64 ? 8 : 4u * ((width + 31) / 32);
         void *sigdata = n->signal->shared.data;
         aj_hoff_edge_t add[AJ_HOFF_MAX_CONS];
         int nadd = 0;
         bool ok = true;
         for (int ci = 0; ci < ncons && ok; ci++) {
            aj_chunk_t *C = cons[ci];
            if (C == P || C->b_in == NULL) { ok = false; break; }
            aj_in_addr_fn ia =
               (aj_in_addr_fn)dlsym(C->dl, "accel_in_addr");
            if (ia == NULL) { ok = false; break; }
            int j = -1;
            for (int k = 0; k < C->n_bin; k++)
               if (C->b_in[k].data == sigdata && C->b_in[k].width == width) {
                  j = k; break;
               }
            if (j < 0) { ok = false; break; }
            unsigned long nb = 0;
            void *dst = ia(C->state, j, &nb);
            void *ext = ia(C->state, -1, NULL);
            if (dst == NULL || ext == NULL || nb != pbytes) { ok = false; break; }
            add[nadd++] = (aj_hoff_edge_t){
               .ord = ord, .dst = dst, .ext_chg = (unsigned char *)ext,
               .wake = wake[ci], .nbytes = pbytes,
               .stage = xmalloc(pbytes), .dirty = false };
         }
         if (!ok || nadd == 0) continue;

         P->hoff_edges = xrealloc_array(P->hoff_edges,
                                        P->hoff_nedges + nadd,
                                        sizeof(aj_hoff_edge_t));
         for (int k = 0; k < nadd; k++)
            P->hoff_edges[P->hoff_nedges++] = add[k];
         P->hoff_flags[ord] = 1;
         // consumers: stop translating these inputs from logic3d — the deposit
         // is bypassed so those bytes are frozen; the poke is the only writer
         for (int ci = 0; ci < ncons; ci++) {
            aj_chunk_t *C = cons[ci];
            for (int k = 0; k < C->n_bin; k++)
               if (C->b_in[k].data == sigdata && C->b_in[k].width == width)
                  C->hin_flags[k] = 1;
         }
         nlink++;
      }
   }
   if (nlink > 0)
      notef("accel-jit: NVC_ACCEL_HANDOFF — %d output(s) wired packed"
            " chunk-to-chunk (deposit bypassed)", nlink);

   if (nlink > 0 && getenv("NVC_ACCEL_NO_RESPEC") == NULL)
      aj_respecialize(m);
}

// Undo aj_build_fastclk + the partially-built chunk when an install fails after
// they were created, so a non-installed chunk leaves no active state.
static void aj_accel_teardown(rt_model_t *m)
{
   free(m->fastclk_table);
   m->fastclk_table = NULL;
   m->fastclk_count = 0;
   m->fastclk_on    = false;
   if (m->aj_chunk_count > 0) {
      aj_chunk_t *c = m->aj_chunks[--m->aj_chunk_count];
      if (c->defer_outs != NULL) {
         for (unsigned i = 0; i < c->defer_count; i++)
            free(c->defer_outs[i].shadow);
         free(c->defer_outs);
      }
      free(c->bindtab);
      free(c->state);
      if (c->dl != NULL) dlclose(c->dl);
      free(c);
   }
}

static char *aj_read_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f) return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = xmalloc(n + 1);
   if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
   buf[n] = '\0';
   fclose(f);
   return buf;
}

// gen_statemachine declares each scalar/vector port as "uint64_t _<name>;" for
// <=64 bits, or as a limb array "uint32_t _<name>[N];" for a >64-bit port (the
// scalable wide path). Match either spelling — a wide port that fails this check
// would pass the gate but never get bridged. (The older "unsigned __int128"
// spelling is matched too for forward/backward compatibility.)
#define AJ_MAX_PINS 4096   // per-subtree bridged-pin cap (dec has hundreds)

static bool aj_model_has_field(const char *dutc_text, const char *name)
{
   char n1[80], n2[96], n3[96];
   snprintf(n1, sizeof n1, "uint64_t _%s;", name);
   snprintf(n2, sizeof n2, "unsigned __int128 _%s;", name);
   snprintf(n3, sizeof n3, "uint32_t _%s[", name);   // wide limb array
   return strstr(dutc_text, n1) != NULL || strstr(dutc_text, n2) != NULL
       || strstr(dutc_text, n3) != NULL;
}

// Emit the address-baked bridge .c around the generated model.
// spec: post-link RE-emission — the link results (hoff/hin flags) and run mode
// (VERIFY=0) are baked in as constants and every resolved branch is emitted
// one-sided; all install side effects (bindtab, defer_outs, flag allocs,
// b_in/b_out tables) are SKIPPED — they exist and must not be disturbed. The
// state layout (aj_cs_t) is emitted identically so the live chunk state
// carries across the eval-pointer swap.
static bool aj_emit_bridge(const char *path, const char *dutc,
                           aj_pin_t *pins, int npins, aj_pin_t *clk,
                           aj_pin_t *rst, rt_model_t *m, aj_chunk_t *chunk,
                           bool spec)
{
   char *dut_text = aj_read_file(dutc);
   if (!dut_text) return false;
   // sm_clock_late present? (extra-group commits from a pre-edge snapshot +
   // current inputs, at each gated clock's own value edge)
   const bool has_late = strstr(dut_text, "void sm_clock_late") != NULL;
   // Fused commit+outputs (one full pass + output-cone recompute instead of
   // sm_clock's pass followed by a full sm_comb pass) — models emitted by
   // newer gen_statemachine provide it; older cached models fall back.
   const bool has_clock_out = strstr(dut_text, "void sm_clock_out") != NULL;
   const bool has_late_out  = strstr(dut_text, "void sm_clock_late_out") != NULL;

   // Multi-clock: gen_statemachine emits `const char *sm_extra_clocks[] = {...};`
   // listing the non-main clock INPUT field base-names. The bridge edge-detects
   // each across deltas to advance the right flop group (sm_clock_masked). Text-
   // scrape it (same convention as aj_model_has_field). Single-clock -> {0} -> 0.
   char extra_clk[16][64];
   int nck = 0;
   {
      const char *start = strstr(dut_text, "const char *sm_extra_clocks[] = {");
      if (start != NULL) {
         start += strlen("const char *sm_extra_clocks[] = {");
         const char *end = strchr(start, '}');
         const char *p = start;
         while (end != NULL && nck < 16) {
            const char *q = strchr(p, '"');
            if (q == NULL || q >= end) break;
            const char *e = strchr(q + 1, '"');
            if (e == NULL || e >= end) break;
            int len = (int)(e - q - 1);
            if (len > 0 && len < 64) {
               memcpy(extra_clk[nck], q + 1, len);
               extra_clk[nck][len] = '\0';
               nck++;
            }
            p = e + 1;
         }
      }
      // Each extra clock must be a marshalled input field, else the bridge cannot
      // edge-detect it from the boundary — decline (leave the chunk interpreted).
      for (int k = 0; k < nck; k++)
         if (!aj_model_has_field(dut_text, extra_clk[k])) {
            notef("accel-jit: extra clock '%s' not a marshalled input — declining",
                  extra_clk[k]);
            free(dut_text);
            return false;
         }
   }

   FILE *f = fopen(path, "w");
   if (!f) { free(dut_text); return false; }

   fprintf(f, "#define SM_NO_MAIN 1\n");
   fprintf(f, "#include <stdint.h>\n#include <string.h>\n");
   fprintf(f, "#include \"%s\"\n\n", dutc);
   // DE-BAKED BINDING. The .so contains NO run-specific addresses: the install
   // points the exported AJB table at a per-run array of {deposit_signal,
   // model_now, model, aj_out, clk-data, rst-data, input-data..., output-sig...}.
   // So an unchanged-logic .so is content-only and is cached/reused across runs
   // and edits (in-place update — skip both synth AND compile). Everything below
   // reads its addresses through AJB via these macros, so the call sites are the
   // same as the previously-baked versions.
   int ni = 0;
   for (int i = 0; i < npins; i++)
      if (!pins[i].is_output && aj_model_has_field(dut_text, pins[i].name)) ni++;

   fprintf(f, "typedef long long (*now_fn)(void*,unsigned*);\n");
   fprintf(f, "typedef void (*force_fn)(void*,void*,const void*,int,unsigned long);\n");
   fprintf(f, "typedef void (*ajout_fn)(int,void*,const void*,int,int);\n");
   // AJB is a PARAMETER of accel_eval (not a global) so an identical-logic .so
   // shared by N chunks stays reentrant — each call uses its own address table.
   fprintf(f, "#define FORCE  ((force_fn)AJB[0])\n");
   fprintf(f, "#define NOW    ((now_fn)AJB[1])\n");
   fprintf(f, "#define MDL    (AJB[2])\n");
   fprintf(f, "#define AJ_OUT ((ajout_fn)AJB[3])\n");
   fprintf(f, "#define CLK    ((uint8_t*)AJB[4])\n");
   fprintf(f, "#define RST    ((uint8_t*)AJB[5])\n");
   fprintf(f, "#define IN_ADDR(i) ((uint8_t*)AJB[6+(i)])\n");
   fprintf(f, "#define OUT_SIG(j) (AJB[%d+(j)])\n", 6 + ni);
   // Per-chunk state lives OUTSIDE the .so (passed in), so an identical-logic .so
   // can be shared by many instances (one synth, N chunks) each with its OWN
   // state. accel_eval/accel_reset take the chunk's state pointer; S and last_t
   // are macros over it so the generated body below is unchanged.
   // clk_last0: last main-clk value, for VERIFY-mode value-edge detection (the
   // verify harness runs the bridge every delta so it sees clk-low; the rerouted
   // path is NOT guaranteed to, hence keeps the time-edge below).
   // DIRECT IN-PLACE INPUTS (no per-eval marshalling). in_live is the packed
   // input state the model reads — it PERSISTS in per-chunk state (NOT a .so
   // static: one .so can serve many instances) and is the data's home in the
   // model's preferred format. raw_shadow keeps the last-seen nvc logic3d
   // bytes per pin; on eval each pin does a raw memcmp and is re-translated
   // ONLY if its bytes changed. Per-eval data movement is therefore
   // proportional to actual signal events, not to total boundary width —
   // an unchanged pin costs one SIMD memcmp, a quiet subtree costs nothing.
   size_t raw_total = 0;
   for (int i = 0; i < npins; i++) {
      if (pins[i].is_output) continue;
      if (!aj_model_has_field(dut_text, pins[i].name)) continue;
      raw_total += (size_t)pins[i].width * pins[i].elem;
   }
   if (raw_total == 0) raw_total = 1;
   // ext_chg: set by a PRODUCER chunk's packed poke (NVC_ACCEL_HANDOFF) when it
   // writes this chunk's in_live directly — the logic3d bytes never change so
   // the raw-shadow memcmp can't see it. o_prev: previous packed outputs, gates
   // the poke (poke-on-change = fixpoint termination on cross-chunk comb loops).
   if (nck == 0)
      fprintf(f, "typedef struct { state_t S; long long last_t;"
                 " unsigned char clk_last0;"
                 " inputs_t in_live; unsigned char shadow_valid, rst_prev, ext_chg;"
                 " outputs_t o_prev;"
                 " unsigned char raw_shadow[%zu]; } aj_cs_t;\n", raw_total);
   else
      // per-extra-clock last value, for value-edge detection across deltas.
      // snapS/late_pend (late-capable models): pre-edge state snapshot taken
      // at the main posedge + the extra-group bits still awaiting their
      // clock's value edge (sm_clock_late commits them from snapS + current
      // inputs — the interp-faithful gated-clock semantics).
      fprintf(f, "typedef struct { state_t S; long long last_t;"
                 " unsigned char clk_last0; unsigned char ck_last[%d];"
                 "%s"
                 " inputs_t in_live; unsigned char shadow_valid, rst_prev, ext_chg;"
                 " outputs_t o_prev;"
                 " unsigned char raw_shadow[%zu]; } aj_cs_t;\n", nck,
              has_late ? " state_t snapS; inputs_t snapIn;"
                         " unsigned char late_pend;" : "",
              raw_total);
   fprintf(f, "unsigned long accel_state_size(void){ return sizeof(aj_cs_t); }\n");
   fprintf(f, "#define S (aj_cs->S)\n#define last_t (aj_cs->last_t)\n");
   if (nck == 0)
      fprintf(f, "void accel_reset(void *p){ aj_cs_t *aj_cs = p;"
                 " sm_reset(&S); last_t = -1; aj_cs->clk_last0 = 0;"
                 " aj_cs->shadow_valid = 0; aj_cs->ext_chg = 0;"
                 " memset(&aj_cs->o_prev, 0, sizeof aj_cs->o_prev);"
                 " memset(&aj_cs->in_live, 0, sizeof aj_cs->in_live); }\n\n");
   else
      fprintf(f, "void accel_reset(void *p){ aj_cs_t *aj_cs = p;"
                 " sm_reset(&S); last_t = -1; aj_cs->clk_last0 = 0;"
                 " aj_cs->shadow_valid = 0; aj_cs->ext_chg = 0;"
                 " memset(&aj_cs->o_prev, 0, sizeof aj_cs->o_prev);"
                 " memset(&aj_cs->in_live, 0, sizeof aj_cs->in_live);"
                 "%s"
                 " for(int _k=0;_k<%d;_k++) aj_cs->ck_last[_k]=0; }\n\n",
              has_late ? " aj_cs->late_pend = 0;" : "", nck);

   // Fill the scalar table slots (the per-pin slots are filled in the loops).
   chunk->bindtab[0] = (void *)&deposit_signal;
   chunk->bindtab[1] = (void *)&model_now;
   chunk->bindtab[2] = m;
   chunk->bindtab[3] = (void *)&aj_out;
   chunk->bindtab[4] = clk->data;
   chunk->bindtab[5] = rst ? rst->data : NULL;
   // Slots after the per-pin table (array is sized 6+npins+4):
   //   [6+npins]   live pointer to g_aj_verify (value-edge clocking under VERIFY)
   //   [6+npins+1] per-output handoff flags (set by aj_link_handoff post-install)
   //   [6+npins+2] aj_poke — packed chunk-to-chunk handoff (NVC_ACCEL_HANDOFF)
   if (!spec) {
      chunk->bindtab[6 + npins] = &g_aj_verify;
      chunk->hoff_flags = xcalloc(npins > 0 ? npins : 1);
      chunk->hin_flags  = xcalloc(npins > 0 ? npins : 1);
      chunk->bindtab[6 + npins + 1] = chunk->hoff_flags;
      chunk->bindtab[6 + npins + 2] = (void *)&aj_poke;
      chunk->bindtab[6 + npins + 3] = chunk->hin_flags;
   }
   // spec: the link pass only runs rerouted (never under VERIFY), so VERIFY is
   // a compile-time 0 — the value-edge clocking, gate bypasses and per-delta
   // compare branches all fold away.
   if (spec)
      fprintf(f, "#define VERIFY 0\n");
   else
      fprintf(f, "#define VERIFY (*(int*)AJB[%d])\n", 6 + npins);
   fprintf(f, "typedef void (*ajpoke_fn)(int,const void*,unsigned);\n");
   fprintf(f, "#define AJ_HOFF ((unsigned char*)AJB[%d])\n", 6 + npins + 1);
   fprintf(f, "#define AJ_POKE ((ajpoke_fn)AJB[%d])\n", 6 + npins + 2);
   fprintf(f, "#define AJ_HIN ((unsigned char*)AJB[%d])\n", 6 + npins + 3);

   fprintf(f, "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n");
   fprintf(f, "static int g_dbg = -1;\n");
   // `in` resolves to the PERSISTENT packed inputs in chunk state. Emitted
   // after the model #include so the model's own `in` parameters are untouched.
   fprintf(f, "#define in (aj_cs->in_live)\n");
   // aj_scan_inputs: apply the CURRENT delta's boundary values to the
   // persistent packed inputs (translate-on-change against the raw shadow).
   // A FUNCTION so the caller controls WHEN this delta's inputs become
   // visible: non-coincident scans before the register advance (legacy);
   // coincident advances FIRST — every flop's D then samples pre-edge state
   // and PREVIOUS-delta-settled inputs (true NBA across the boundary; a
   // mid-settle delta-0 glitch on an input can no longer poison a gated-clock
   // register, which deadlocked ifu_mem_ctl's DMA arbitration FSM).
   fprintf(f, "static int aj_scan_inputs(aj_cs_t *aj_cs, void **AJB){\n");
   // _chg: any NON-CLOCK input pin's raw bytes changed since the last eval
   // (clock-family pins still re-translate for sm_clock/mask use, but their
   // effect is an EDGE, captured by posedge/posedge_mask — so they don't force
   // a re-settle by themselves). First eval (shadow_valid==0) runs everything.
   // ext_chg: a producer chunk poked our packed inputs directly (handoff) —
   // the logic3d bytes didn't change, so only this flag knows.
   fprintf(f, "  int _chg = !aj_cs->shadow_valid;\n");
   fprintf(f, "  if(aj_cs->ext_chg){ _chg = 1; aj_cs->ext_chg = 0; }\n");

   int bridged_in = 0, bridged_out = 0;
   size_t raw_off = 0;
   static int bin_pin[AJ_MAX_PINS];    // pin index per bridged-input ordinal
   static int bout_pin[AJ_MAX_PINS];   // pin index per output ordinal
   // inputs: DIRECT IN-PLACE — the packed `in` (chunk-state in_live) persists;
   // each pin memcmp's its nvc logic3d bytes against raw_shadow and is only
   // re-translated on change. Translation detail: each sub-element is `elem`
   // bytes (logic3d = natural); the value bit is bit 0 of the element's low
   // byte (little-endian). nvc stores a `(width-1 downto 0)` vector MSB-first:
   // element offset e maps to bit (width-1-e) — without this reversal every
   // multi-bit input arrives bit-reversed (din=1 read as 0x80).
   for (int i = 0; i < npins; i++) {
      if (pins[i].is_output) continue;
      if (!aj_model_has_field(dut_text, pins[i].name)) continue;
      if (!spec)
         chunk->bindtab[6 + bridged_in] = pins[i].data;
      const size_t nb = (size_t)pins[i].width * pins[i].elem;
      bool is_ck = strcmp(pins[i].name, "clk") == 0;
      for (int k = 0; k < nck && !is_ck; k++)
         if (strcmp(pins[i].name, extra_clk[k]) == 0) is_ck = true;
      // spec + handoff-fed: the pin's logic3d bytes are frozen (deposit
      // bypassed) and the poke is the only writer — emit NOTHING, not even
      // the memcmp. raw_off still advances so aj_cs_t stays layout-identical.
      const bool suppress = spec && chunk->hin_flags != NULL
         && chunk->hin_flags[bridged_in];
      if (!suppress) {
         if (spec)
            fprintf(f, "  { uint8_t*p=IN_ADDR(%d);"
                       " if(!aj_cs->shadow_valid ||"
                       " memcmp(aj_cs->raw_shadow+%zu,p,%zu)){"
                       " memcpy(aj_cs->raw_shadow+%zu,p,%zu);%s\n",
                    bridged_in, raw_off, nb, raw_off, nb,
                    is_ck ? "" : " _chg=1;");
         else
            fprintf(f, "  { uint8_t*p=IN_ADDR(%d);"
                       " if(!AJ_HIN[%d] && (!aj_cs->shadow_valid ||"
                       " memcmp(aj_cs->raw_shadow+%zu,p,%zu))){"
                       " memcpy(aj_cs->raw_shadow+%zu,p,%zu);%s\n",
                    bridged_in, bridged_in, raw_off, nb, raw_off, nb,
                    is_ck ? "" : " _chg=1;");
         if (pins[i].width > 64) {
            // Wide port: gen_statemachine declares in._<name> as a uint32_t[N]
            // limb array; place each bit in its limb.
            const int nl = (pins[i].width + 31) / 32;
            fprintf(f, "    for(int _l=0;_l<%d;_l++) in._%s[_l]=0;"
                       " for(int b=0;b<%d;b++){ int _bp=%d-1-b;"
                       " in._%s[_bp>>5]|=(uint32_t)(p[b*%d]&1)<<(_bp&31); } } }\n",
                    nl, pins[i].name, pins[i].width, pins[i].width,
                    pins[i].name, pins[i].elem);
         } else {
            fprintf(f, "    uint64_t v=0;"
                       " for(int b=0;b<%d;b++) v|=(uint64_t)(p[b*%d]&1)<<(%d-1-b);"
                       " in._%s=v; } }\n",
                    pins[i].width, pins[i].elem, pins[i].width, pins[i].name);
         }
      }
      bin_pin[bridged_in] = i;
      raw_off += nb;
      bridged_in++;
   }
   fprintf(f, "  aj_cs->shadow_valid = 1;\n");
   if (!spec) {
      // bridged-input summary for the handoff link pass (bridge ordinal order)
      chunk->b_in = xcalloc_array(bridged_in > 0 ? bridged_in : 1,
                                  sizeof(aj_bpin_t));
      chunk->n_bin = bridged_in;
      for (int bi = 0; bi < bridged_in; bi++) {
         aj_pin_t *pp = &pins[bin_pin[bi]];
         chunk->b_in[bi] = (aj_bpin_t){ .data = pp->data, .sig = pp->sig,
                                        .width = pp->width, .elem = pp->elem };
      }
   }
   fprintf(f, "  return _chg;\n}\n\n");
   fprintf(f, "void accel_eval(void *p, void **AJB){\n  aj_cs_t *aj_cs = p;\n");
   fprintf(f, "  if(g_dbg<0) g_dbg = getenv(\"NVC_ACCEL_JIT_DEBUG\")?20000:0;\n");
   fprintf(f, "  unsigned d; long long t = NOW(MDL,&d);\n");
   // clk is one element; its 0/1 value is bit 0 of the low byte of the element.
   fprintf(f, "  int _clk = CLK[0]&1;\n");
   fprintf(f, "  if(g_dbg>0 && _clk){ fprintf(stderr,\"AJ clk=%%d t=%%lld last=%%lld\\n\",_clk,t,last_t); g_dbg--; }\n");
   // The bridge now runs on EVERY boundary-input-change delta (the rerouted
   // combinational processes wake it), not just the clock edge. ADVANCE the
   // registers once per clock cycle — at the first call with clk high at a NEW
   // simulation time (the posedge); last_t holds the time we last advanced (a
   // level edge-detect would need the bridge to also run during clk-low, which a
   // posedge-optimized clocked process does NOT). COMB outputs re-settle on every
   // call regardless (below).
   // Reroute path: time-edge (bridge only runs when a rerouted process wakes it —
   // reliably at the posedge, not during clk-low). VERIFY path: the harness runs
   // the bridge EVERY delta (it sees clk-low too), so use a robust value-edge that
   // never double-fires on a stray clk-high sample (e.g. a clock being stopped).
   fprintf(f, "  int posedge;\n");
   fprintf(f, "  if(VERIFY) posedge = (_clk && !aj_cs->clk_last0);\n");
   fprintf(f, "  else { posedge = (_clk && t != last_t); if(posedge) last_t = t; }\n");
   fprintf(f, "  aj_cs->clk_last0 = _clk;\n");
   // Escape hatch / A-B proof: NVC_ACCEL_NO_SETTLE restores the OLD once-per-edge
   // behaviour (no combinational re-settle on input-change deltas) — wrong for a
   // Mealy boundary, used to demonstrate the settling fix.
   if (getenv("NVC_ACCEL_NO_SETTLE"))
      fprintf(f, "  if(!posedge) return;\n");
   fprintf(f, "  outputs_t o;\n");
   fprintf(f, "  memset(&o,0,sizeof o);\n");
   if (nck == 0)
      fprintf(f, "  int _chg = aj_scan_inputs(aj_cs, AJB);\n");
   // NVC_ACCEL_INDUMP: dump this cycle's SETTLED input vector (named, in pin
   // order) at each posedge, so a fork-checkpoint child can capture the exact
   // real stimulus around a divergence for offline replay (net_diff / xcheck).
   fprintf(f, "  static int _indump=-1; if(_indump<0) _indump=getenv(\"NVC_ACCEL_INDUMP\")?1:0;\n");
   fprintf(f, "  if(_indump && posedge){ fprintf(stderr,\"#AJIN t=%%lld\", t);\n");
   for (int i = 0; i < npins; i++) {
      if (pins[i].is_output) continue;
      if (!aj_model_has_field(dut_text, pins[i].name)) continue;
      if (pins[i].width > 64) {
         const int nl = (pins[i].width + 31) / 32;
         fprintf(f, "    fprintf(stderr,\" %s=\"); for(int _l=%d;_l>=0;_l--)"
                    " fprintf(stderr,\"%%08x\", in._%s[_l]);\n",
                 pins[i].name, nl - 1, pins[i].name);
      }
      else
         fprintf(f, "    fprintf(stderr,\" %s=%%llx\", (unsigned long long)in._%s);\n",
                 pins[i].name, pins[i].name);
   }
   fprintf(f, "    fprintf(stderr,\"\\n\"); }\n");
   // Early-out: no clock edge, no reset-level change, and no non-clock input
   // changed (per the raw-shadow memcmp above) — no output can change, so skip
   // sm_clock/sm_comb and the whole deposit pass. Supersedes the struct-copy
   // skip: detection now rides on the same per-pin change tracking that keeps
   // in_live fresh, with zero extra copies. Disabled under VERIFY (per-delta
   // evaluation is the point there); NVC_ACCEL_NO_SKIP for A/B measurement.
   {
      const char *rstv = (rst != NULL) ? "(RST[0]&1)" : "0";
      if (nck == 0) {
         fprintf(f, "  { static int _noskip=-1; if(_noskip<0) _noskip=getenv(\"NVC_ACCEL_NO_SKIP\")?1:0;\n");
         fprintf(f, "    int _rn = %s;\n", rstv);
         fprintf(f, "    if(!_noskip && !VERIFY && !posedge && !_chg && _rn==aj_cs->rst_prev) return;\n");
         fprintf(f, "    aj_cs->rst_prev = _rn; }\n");
      }
      // multi-clock: the skip is emitted inline after the post-advance scan
      // (see the nck>0 dispatch below); nothing stashed.
   }
   // Advance the registers to the next state ONLY on the clock posedge
   // (sm_clock). An async reset port (rst) resets immediately whenever asserted
   // (any delta), matching real async-reset hardware; otherwise advance on the
   // edge. gen_statemachine folds a SYNC reset into the model's logic (it reads
   // the reset as a normal input), so sm_clock handles that itself.
   fprintf(f, "  int _fused = 0;\n");
   if (nck == 0) {
      const char *adv = has_clock_out
         ? "{ if(!VERIFY){ sm_clock_out(&S,&in,&o,1u); _fused=1; }"
           " else sm_clock(&S,&in); }"
         : "sm_clock(&S,&in);";
      if (rst != NULL) {
         fprintf(f, "  if(RST[0]&1) sm_reset(&S);\n");
         fprintf(f, "  else if(posedge) %s\n", adv);
      }
      else
         fprintf(f, "  if(posedge) %s\n", adv);
   }
   else {
      // Multi-clock: build a per-group posedge mask. Bit0 = main clk (time-edge,
      // posedge). Bit 1+k = extra clock k, VALUE-edge (now && !last) across the
      // bridge's per-delta re-runs — because the rvclkhdr gater drives free_clk/
      // active_clk in a LATER delta than clk (same sim time), they read stale-low
      // at the clk delta, so we advance each extra group in whatever delta its
      // clock actually rises. Each advance reads the LIVE state S at its own delta
      // (an extra clock lagging clk sees the post-clk-advance state, as nvc's
      // interpreted delta loop does); the top-of-sm_clock snapshot gives NBA.
      //
      // NVC_ACCEL_CK_COINCIDENT: VeeR's active_clk/free_clk are the SAME edge as
      // clk merely gated by an enable (rvclkhdr ICG = clk & en). Their flops must
      // sample the PRE-edge snapshot coincident with clk, not the post-clk-advance
      // state. In coincident mode we fold every extra group's advance into the main
      // posedge (one sm_clock_masked call reads ONE pre-edge S), giving correct NBA
      // for coincident gated clocks. (A genuinely-lagging derived clock would need
      // the value-edge path; VeeR has none.)
      const int env_late = has_late
         && getenv("NVC_ACCEL_CK_LATE") != NULL ? 1 : 0;
      if (spec) {
         fprintf(f, "  enum { _coinc = %d, _late = %d, _lsnap = %d };\n",
                 getenv("NVC_ACCEL_CK_COINCIDENT") != NULL ? 1 : 0, env_late,
                 getenv("NVC_ACCEL_LATE_SNAPIN") != NULL ? 1 : 0);
      }
      else {
         fprintf(f, "  static int _coinc=-1; if(_coinc<0) _coinc=getenv(\"NVC_ACCEL_CK_COINCIDENT\")?1:0;\n");
         if (has_late) {
            fprintf(f, "  static int _late=-1; if(_late<0) _late=getenv(\"NVC_ACCEL_CK_LATE\")?1:0;\n");
            fprintf(f, "  static int _lsnap=-1; if(_lsnap<0) _lsnap=getenv(\"NVC_ACCEL_LATE_SNAPIN\")?1:0;\n");
         }
         else
            fprintf(f, "  enum { _late = 0 };\n");
      }
      fprintf(f, "  unsigned posedge_mask = 0;\n");
      fprintf(f, "  if(posedge) posedge_mask |= 1u;\n");
      fprintf(f, "  int _chg = 0;\n");
      // LATE mode: snapshot the pre-edge state at the main posedge; the extra
      // groups commit later, at each gated clock's own VALUE edge, from
      // (snapshot registers + at-that-delta inputs) — the interp-faithful
      // semantics for both internal-cone flops (dec ibvalff: pre-edge state)
      // and input-fed flops (mem_ctl ok_prev, lsu bus enables: the input as
      // of the gater's rise). Group 0 still advances at the posedge below.
      if (has_late)
         // NOTE: `S` is a macro for aj_cs->S — writing aj_cs->S here would
         // expand into aj_cs->(aj_cs->S). Use the macro.
         fprintf(f, "  if(_late && posedge){ aj_cs->snapS = S;"
                    " aj_cs->snapIn = in;"
                    " aj_cs->late_pend = %uu; }\n", ((1u << (nck + 1)) - 2u));
      // non-coincident (legacy): scan FIRST, then value-edge-detect each extra
      // clock from the freshly-scanned values — original behaviour, unchanged.
      fprintf(f, "  if(!_late && !_coinc){\n");
      fprintf(f, "    _chg = aj_scan_inputs(aj_cs, AJB);\n");
      for (int k = 0; k < nck; k++)
         fprintf(f, "    { int _n=(in._%s&1);"
                    " if(_n && !aj_cs->ck_last[%d]) posedge_mask|=(1u<<(1+%d));"
                    " aj_cs->ck_last[%d]=_n; }\n", extra_clk[k], k, k, k);
      // coincident: every gated clock is the main edge; DEFER the scan until
      // after the advance so flops sample previous-delta-settled inputs.
      fprintf(f, "  } else if(!_late) {\n");
      fprintf(f, "    if(posedge) posedge_mask |= %uu;\n",
              ((1u << (nck + 1)) - 2u));
      fprintf(f, "  }\n");
      fprintf(f, "  int aj_pe = (posedge_mask != 0);\n");
      {
         const char *adv = has_clock_out
            ? "{ if(!VERIFY){ sm_clock_out(&S,&in,&o,posedge_mask); _fused=1; }"
              " else sm_clock_masked(&S,&in,posedge_mask); }"
            : "sm_clock_masked(&S,&in,posedge_mask);";
         if (rst != NULL) {
            fprintf(f, "  if(RST[0]&1) sm_reset(&S);\n");
            fprintf(f, "  else if(posedge_mask) %s\n", adv);
         }
         else
            fprintf(f, "  if(posedge_mask) %s\n", adv);
      }
      fprintf(f, "  if(_late || _coinc) _chg = aj_scan_inputs(aj_cs, AJB);\n");
      if (has_late) {
         // gated-clock value edges, from the freshly-scanned inputs; each
         // pending group commits ONCE per main-clock cycle.
         // A fired late commit IS a clock edge: set aj_pe so the outputs it
         // changes reach AJ_OUT edge-classified. Without this they arrived
         // with posedge=0, were permanently pinned off_edge (Mealy) and thus
         // excluded from the NBA-region deposit — leaving cross-chunk capture
         // at a shared gated-clock delta dependent on procq eval order (the
         // immediate deposit is same-delta-visible to a later-evaluated
         // chunk's input scan; interp is immune because native `<=` commits
         // in the NBA region).
         //
         // Snapshot discipline: collect ALL groups whose clock rises in THIS
         // delta, snapshot the LIVE state once, and commit them together from
         // that snapshot. The per-eval snapshot is the interp-exact register
         // view at this delta: group 0 (committed at the posedge delta) and
         // any gated group that fired in an EARLIER delta read POST-edge
         // (already in S); groups firing in THIS delta read each other
         // PRE-edge (classic simultaneous <=). The previous design
         // snapshotted once at the POSEDGE, so a late cone reading a group-0
         // register saw a stale PRE-edge value — unified lsu's AXI arvalid
         // cone (bus_clk_en sync on free_clk gating handshake flops on clk)
         // issued every bus command one cycle late (cycles 1033 -> 955).
         // NVC_ACCEL_LATE_CYCSNAP restores the per-cycle snapshot for A/B.
         fprintf(f, "  if(_late){\n");
         fprintf(f, "    unsigned _fired = 0;\n");
         for (int k = 0; k < nck; k++)
            fprintf(f, "    { int _n=(in._%s&1);"
                       " if(_n && !aj_cs->ck_last[%d] && (aj_cs->late_pend & (1u<<(1+%d))))"
                       " _fired |= (1u<<(1+%d));"
                       " aj_cs->ck_last[%d]=_n; }\n",
                    extra_clk[k], k, k, k, k);
         fprintf(f, "    if(_fired){\n");
         // Default = per-CYCLE snapshot (dec/ifu/mem_ctl-validated); the
         // per-EVAL (live-S) variant is opt-in for A/B — it regressed dec and
         // mem_ctl and did not change lsu, so the posedge snapshot stands.
         fprintf(f, "      static int _evalsnap=-1; if(_evalsnap<0)"
                    " _evalsnap=getenv(\"NVC_ACCEL_LATE_EVALSNAP\")?1:0;\n");
         fprintf(f, "      if(_evalsnap) aj_cs->snapS = S;\n");
         if (has_late_out)
            // fused: late-D-cone commit + output-cone recompute in one call;
            // the tail full sm_comb is skipped for this eval
            fprintf(f, "      if(!VERIFY){"
                       " sm_clock_late_out(&S, &aj_cs->snapS,"
                       " _lsnap ? &aj_cs->snapIn : &in, &o, _fired); _fused=1; }"
                       " else sm_clock_late(&S, &aj_cs->snapS,"
                       " _lsnap ? &aj_cs->snapIn : &in, _fired);\n");
         else
            fprintf(f, "      sm_clock_late(&S, &aj_cs->snapS,"
                       " _lsnap ? &aj_cs->snapIn : &in, _fired);\n");
         fprintf(f, "      aj_cs->late_pend &= ~_fired; _chg = 1; aj_pe = 1;\n");
         fprintf(f, "    }\n");
         fprintf(f, "  }\n");
      }
      {
         const char *rstv2 = (rst != NULL) ? "(RST[0]&1)" : "0";
         fprintf(f, "  { static int _noskip=-1; if(_noskip<0) _noskip=getenv(\"NVC_ACCEL_NO_SKIP\")?1:0;\n");
         fprintf(f, "    int _rn = %s;\n", rstv2);
         fprintf(f, "    if(!_noskip && !VERIFY && !posedge_mask && !_chg && _rn==aj_cs->rst_prev) return;\n");
         fprintf(f, "    aj_cs->rst_prev = _rn; }\n");
      }
   }
   // ALWAYS re-settle the combinational outputs against the CURRENT state +
   // inputs. This runs on the posedge (after the register advance, so outputs
   // reflect the new state) AND on every boundary-input-change delta in the
   // cycle — intra-cycle combinational settling that converges to the same
   // fixpoint nvc's interpreted delta loop reaches. No lookahead needed: outputs
   // are deposited THIS delta (below) and propagate immediately via wakeup.
   fprintf(f, "  if(!_fused) sm_comb(&S,&in,&o);\n");
   // NVC_ACCEL_SMDUMP: dump the accel model's ALL internal nets (registers +
   // combinational) once per cycle at the posedge, in the REAL sim — accurate
   // multi-clock + real stimulus. Trace where a divergence enters a cone that
   // random xcheck / offline replay can't reach. Needs -DSM_DUMP (compile).
   fprintf(f, "#ifdef SM_DUMP\n");
   fprintf(f, "  { static int _smd=-1; if(_smd<0) _smd=getenv(\"NVC_ACCEL_SMDUMP\")?1:0;\n");
   fprintf(f, "    if(_smd && posedge){ fprintf(stderr,\"#AJSM t=%%lld\\n\",t);"
              " sm_dump_comb(&S,&in,stderr); } }\n");
   fprintf(f, "#endif\n");
   // Generic per-pin trace (only fields the synth model actually declares, so it
   // compiles for any DUT — not just the a_plus_b ports it was first written for).
   fprintf(f, "  if(g_dbg>0){ fprintf(stderr,\"AJ   in:");
   for (int i = 0; i < npins; i++)
      if (!pins[i].is_output && aj_model_has_field(dut_text, pins[i].name))
         fprintf(f, " %s=%%lu", pins[i].name);
   fprintf(f, " | out:");
   for (int i = 0; i < npins; i++)
      if (pins[i].is_output && aj_model_has_field(dut_text, pins[i].name))
         fprintf(f, " %s=%%lu", pins[i].name);
   fprintf(f, "\\n\"");
   for (int i = 0; i < npins; i++)
      if (!pins[i].is_output && aj_model_has_field(dut_text, pins[i].name))
         fprintf(f, ",(unsigned long)in._%s", pins[i].name);
   for (int i = 0; i < npins; i++)
      if (pins[i].is_output && aj_model_has_field(dut_text, pins[i].name))
         fprintf(f, ",(unsigned long)o._%s", pins[i].name);
   fprintf(f, "); }\n");
   // outputs: driven-certain logic3d (L3D_0=2 / L3D_1=3) in the low byte of each
   // `elem`-byte sub-element; upper bytes stay 0 (value fits in 0..7).
   // NVC_ACCEL_NO_FORCE: compute outputs but skip the deposit (measurement only;
   // produces wrong results — used to isolate the boundary-deposit per-cycle cost).
   const bool no_force = getenv("NVC_ACCEL_NO_FORCE") != NULL;
   // The posedge flag handed to AJ_OUT classifies a registered-output change as an
   // edge change (kept in the deferred bank) vs a Mealy off-edge change. With
   // multi-clock, an output registered on free_clk changes in a delta where bit0
   // (main posedge) is 0; pass `aj_pe = (posedge_mask!=0)` so any-group advance
   // still counts as an edge. Single-clock keeps the literal `posedge`.
   const char *pe_arg = (nck > 0) ? "aj_pe" : "posedge";
   // Per-output cone class from gen_statemachine: `sm_comb_outputs[] = {...}`
   // lists outputs whose combinational cone reaches a boundary input (Mealy).
   // Those must stay in the ACTIVE region (posedge literal 0 -> immediate
   // deposit); register-only outputs keep the real edge flag and may commit
   // in the NBA region under NVC_ACCEL_NBA. Absent table (older model .c)
   // -> every output is treated Mealy (today's behaviour).
   char comb_out[256][64];
   int n_comb_out = 0;
   {
      const char *start = strstr(dut_text, "const char *sm_comb_outputs[] = {");
      if (start != NULL) {
         start += strlen("const char *sm_comb_outputs[] = {");
         const char *end = strchr(start, '}');
         const char *p = start;
         while (end != NULL && n_comb_out < 256) {
            const char *q = strchr(p, '"');
            if (q == NULL || q >= end) break;
            const char *e = strchr(q + 1, '"');
            if (e == NULL || e >= end) break;
            int len = (int)(e - q - 1);
            if (len > 0 && len < 64) {
               memcpy(comb_out[n_comb_out], q + 1, len);
               comb_out[n_comb_out][len] = '\0';
               n_comb_out++;
            }
            p = e + 1;
         }
      }
      else
         n_comb_out = -1;   // no table: treat ALL outputs as Mealy
   }
   // Build the deferred-output table in emit order so the bridge's per-output
   // ordinal matches m->aj_defer_outs[ord]. Each output is classified now (the
   // fast-clk table already exists — aj_build_fastclk runs before emit); a
   // qualifying output is bank-switched, the rest fall back to deposit_signal.
   if (!spec)
      chunk->defer_outs = xcalloc_array(npins > 0 ? npins : 1,
                                        sizeof(aj_defer_out_t));
   int ord = 0, deferred = 0;
   for (int i = 0; i < npins; i++) {
      if (!pins[i].is_output) continue;
      if (!aj_model_has_field(dut_text, pins[i].name)) continue;
      bool is_comb = (n_comb_out < 0);   // no table -> conservative Mealy
      for (int ci = 0; ci < n_comb_out && !is_comb; ci++)
         if (strcmp(comb_out[ci], pins[i].name) == 0) is_comb = true;
      char pe_buf[32];
      snprintf(pe_buf, sizeof pe_buf, "(4|%s)", pe_arg);
      const char *pe_i = is_comb ? pe_buf : pe_arg;
      const int bufsz = pins[i].width * pins[i].elem > 0
                        ? pins[i].width * pins[i].elem : 1;   // width*elem bytes
      if (!spec) {
         aj_defer_out_t *d = &chunk->defer_outs[ord];
         rt_nexus_t *tgt = aj_classify_output(m, pins[i].sig);
         if (tgt != NULL && (size_t)tgt->size * tgt->width != (size_t)bufsz)
            tgt = NULL;   // layout mismatch across the port hop — fall back
         d->valuesz = (size_t)bufsz;
         d->defer   = (tgt != NULL);
         if (d->defer) {
            d->nexus       = tgt;
            d->eff         = (unsigned char *)tgt->signal->shared.data + tgt->offset;
            d->last        = d->eff + tgt->signal->shared.size;
            d->cache_event = (tgt->flags & NET_F_CACHE_EVENT) != 0;
            d->shadow      = xmalloc(bufsz);
            deferred++;
         }
         chunk->bindtab[6 + ni + ord] = pins[i].sig;
      }
      if (!no_force) {
         // NVC_ACCEL_HANDOFF: an output the link pass wired chunk-to-chunk
         // pokes its PACKED value straight into the consumers (change-gated on
         // o_prev — poke-on-change is what terminates cross-chunk comb loops)
         // and skips the logic3d translation + deposit entirely. Flags are all
         // zero until aj_link_handoff runs, so the else branch is the default.
         // spec: the flag is a known constant — emit only the taken branch.
         const int hoff = spec && chunk->hoff_flags != NULL
            && chunk->hoff_flags[ord];
         if (pins[i].width > 64) {
            const int nl = (pins[i].width + 31) / 32;
            if (!spec || hoff) {
               if (!spec) fprintf(f, "  if(AJ_HOFF[%d])", ord);
               else       fprintf(f, "  ");
               fprintf(f, "{"
                          " if(memcmp(o._%s,aj_cs->o_prev._%s,%d)){"
                          " memcpy(aj_cs->o_prev._%s,o._%s,%d);"
                          " AJ_POKE(%d,aj_cs->o_prev._%s,%d); } }\n",
                       pins[i].name, pins[i].name, 4 * nl,
                       pins[i].name, pins[i].name, 4 * nl,
                       ord, pins[i].name, 4 * nl);
            }
            if (!spec || !hoff) {
               // Wide output: o._<name> is a uint32_t[N] limb array; read bit b
               // from its limb, drive nexus element (width-1-b) MSB-first.
               // UNCONDITIONAL: deposits carry transaction semantics interp
               // readers can wake on — change-gating them altered VeeR's
               // cycle count (1033 -> 887). deposit_signal already handles
               // value-change detection for events.
               fprintf(f, "  %s{ uint8_t buf[%d]; memset(buf,0,sizeof buf);"
                          " for(int b=0;b<%d;b++) buf[(%d-1-b)*%d]="
                          "2|(unsigned)((o._%s[b>>5]>>(b&31))&1);"
                          " AJ_OUT(%d,OUT_SIG(%d),buf,%d,%s); }\n",
                       spec ? "" : "else ",
                       bufsz, pins[i].width, pins[i].width, pins[i].elem,
                       pins[i].name, ord, ord, pins[i].width, pe_i);
            }
         }
         else {
            if (!spec || hoff) {
               if (!spec) fprintf(f, "  if(AJ_HOFF[%d])", ord);
               else       fprintf(f, "  ");
               fprintf(f, "{"
                          " if(o._%s!=aj_cs->o_prev._%s){"
                          " aj_cs->o_prev._%s=o._%s;"
                          " AJ_POKE(%d,&aj_cs->o_prev._%s,8); } }\n",
                       pins[i].name, pins[i].name,
                       pins[i].name, pins[i].name, ord, pins[i].name);
            }
            if (!spec || !hoff) {
               fprintf(f, "  %s{ uint8_t buf[%d]; memset(buf,0,sizeof buf); uint64_t v=o._%s;"
                          " for(int b=0;b<%d;b++) buf[(%d-1-b)*%d]=2|(unsigned)((v>>b)&1);"
                          " AJ_OUT(%d,OUT_SIG(%d),buf,%d,%s); }\n",
                       spec ? "" : "else ",
                       bufsz, pins[i].name, pins[i].width, pins[i].width, pins[i].elem,
                       ord, ord, pins[i].width, pe_i);
            }
         }
      }
      bout_pin[ord] = i;
      ord++;
      bridged_out++;
   }
   if (!spec) {
      chunk->defer_count = ord;
      // bridged-output summary for the handoff link pass (output ordinal order)
      chunk->b_out = xcalloc_array(ord > 0 ? ord : 1, sizeof(aj_bpin_t));
      chunk->n_bout = ord;
      for (int bo = 0; bo < ord; bo++) {
         aj_pin_t *pp = &pins[bout_pin[bo]];
         chunk->b_out[bo] = (aj_bpin_t){ .data = pp->data, .sig = pp->sig,
                                         .width = pp->width, .elem = pp->elem };
      }
   }
   if (deferred > 0 && !spec)
      notef("accel-jit: NVC_ACCEL_BANK — %d/%d output(s) bank-switched (deferred)",
            deferred, ord);
   fprintf(f, "}\n");
   // accel_in_addr: resolve a bridged-input ordinal to its packed in_live
   // field (ord == -1 -> the ext_chg flag). The handoff link pass uses this to
   // wire a producer's poke straight into this chunk's packed inputs.
   // Settle-time dump: the VERIFY harness calls this at the exact report of
   // the first diverging output — full internal nets from the CURRENT state
   // + in_live, i.e. the companion's settled view (the posedge SMDUMP can't
   // see divergence born in later deltas).
   fprintf(f, "#ifdef SM_DUMP\n");
   fprintf(f, "void accel_dump(void *p){ aj_cs_t *aj_cs = p;\n");
   fprintf(f, "  fprintf(stderr, \"#AJVD\\n\");\n");
   fprintf(f, "  sm_dump_comb(&S, &(aj_cs->in_live), stderr); }\n");
   fprintf(f, "#endif\n");
   fprintf(f, "void *accel_in_addr(void *p, int _iord, unsigned long *nb){\n");
   fprintf(f, "  aj_cs_t *aj_cs = p;\n  switch(_iord){\n");
   fprintf(f, "  case -1: return &aj_cs->ext_chg;\n");
   for (int bi = 0; bi < bridged_in; bi++) {
      aj_pin_t *pp = &pins[bin_pin[bi]];
      if (pp->width > 64) {
         const int nl = (pp->width + 31) / 32;
         fprintf(f, "  case %d: if(nb)*nb=%d; return aj_cs->in_live._%s;\n",
                 bi, 4 * nl, pp->name);
      }
      else
         fprintf(f, "  case %d: if(nb)*nb=8; return &aj_cs->in_live._%s;\n",
                 bi, pp->name);
   }
   fprintf(f, "  }\n  return 0;\n}\n");
   fclose(f);
   free(dut_text);

   if (!spec)
      notef("accel-jit: bridge %d inputs, %d outputs", bridged_in, bridged_out);
   return bridged_in > 0 && bridged_out > 0;
}

// Resolve the gen_statemachine binary without relying on $PATH: honour
// $GEN_STATEMACHINE, then the in-tree install location, then fall back to PATH.
// (A standalone board may not have sv2ghdl/yosys on its login PATH.)
static const char *aj_gen_sm(void)
{
   const char *g = getenv("GEN_STATEMACHINE");
   if (g != NULL && access(g, X_OK) == 0)
      return g;
   static const char *const cand[] = {
      "/usr/local/src/sv2ghdl/yosys/gen_statemachine", NULL };
   for (const char *const *p = cand; *p != NULL; p++)
      if (access(*p, X_OK) == 0)
         return *p;
   return "gen_statemachine";   // last resort: hope it's on PATH
}

// mkdir -p: create each component of path (best effort; ignore EEXIST).
static void aj_mkdir_p(const char *path)
{
   char tmp[512];
   snprintf(tmp, sizeof tmp, "%s", path);
   for (char *p = tmp + 1; *p != '\0'; p++) {
      if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
   }
   mkdir(tmp, 0755);
}

// Emit a scope's whole subtree to one open .v file via vhdl2vlog (logic3d->
// 2-state), each unique module once. Returns true iff every module fully
// translates. Module names + the dedup key use vhdl2vlog_variant_name (per
// (entity,generics)) so generic width-variants emit separately and instance refs
// resolve when gen_statemachine flattens the top.
static bool emit_subtree_v(rt_scope_t *scope, FILE *f,
                           ident_t *seen, int *nseen, int maxseen)
{
   if (scope->kind == SCOPE_INSTANCE && scope->where != NULL) {
      tree_t r = aj_scope_ref(scope);
      if (r != NULL) {
         tree_t ent = (tree_kind(r) == T_ARCH) ? tree_primary(r) : r;
         // Dedup by the per-(entity,generics) VARIANT name, not the entity ident,
         // so a generic module instantiated at multiple widths emits one module
         // PER width (matching what emit_stmt instantiates from the same block).
         char mod[320];
         snprintf(mod, sizeof mod, "%s",
                  vhdl2vlog_variant_name(tree_ident(ent), scope->where));
         ident_t key = ident_new(mod);
         bool dup = false;
         for (int i = 0; i < *nseen; i++)
            if (seen[i] == key) { dup = true; break; }
         if (!dup) {
            if (*nseen < maxseen) seen[(*nseen)++] = key;
            if (!vhdl2vlog_module(f, scope->where, mod))
               return false;
         }
      }
   }
   for (int ci = 0; ci < scope->children.count; ci++)
      if (!emit_subtree_v(scope->children.items[ci], f, seen, nseen, maxseen))
         return false;
   return true;
}

// Cheap recursive instance count for a subtree (no translation). Used as a
// PRE-gate before the expensive emit: instance count >= unique-module count
// (nseen), so instances < min_mod implies the subtree is "too small" anyway.
static int aj_count_instances(rt_scope_t *scope)
{
   int n = (scope->kind == SCOPE_INSTANCE) ? 1 : 0;
   for (int i = 0; i < scope->children.count; i++)
      n += aj_count_instances(scope->children.items[i]);
   return n;
}

static bool accel_install_subtree(rt_model_t *m, rt_scope_t *scope,
                                  tree_t ref, const char *accel_dir)
{
   tree_t ent0 = (tree_kind(ref) == T_ARCH) ? tree_primary(ref) : ref;
   char top0[128];
   aj_lower(top0, istr(tree_ident(ent0)), sizeof top0);

   // Pre-gate: skip the expensive emit/translate entirely for subtrees that
   // cannot reach min_mod modules. This skips the ~21k tiny primitive-cell
   // (sv_and / rvdff) emit attempts that dominated the scan on real designs.
   {
      const char *minenv = getenv("NVC_ACCEL_MIN_MODULES");
      const int min_mod = minenv ? atoi(minenv) : 8;
      if (aj_count_instances(scope) < min_mod)
         return false;
   }

   // Bisection knobs (localize a miscompiling subtree by name):
   //   NVC_ACCEL_ONLY=n1,n2  install ONLY subtrees whose lowered name contains a token
   //   NVC_ACCEL_SKIP=n1,n2  leave matching subtrees in the interpreter
   // Both are comma lists matched as substrings of the lowered subtree name.
   {
      const char *only = getenv("NVC_ACCEL_ONLY");
      if (only != NULL && only[0] != '\0') {
         char buf[512];
         snprintf(buf, sizeof buf, "%s", only);
         bool match = false;
         for (char *t = strtok(buf, ","); t != NULL; t = strtok(NULL, ","))
            if (t[0] != '\0' && strstr(top0, t) != NULL) { match = true; break; }
         if (!match) return false;
      }
      const char *skip = getenv("NVC_ACCEL_SKIP");
      if (skip != NULL && skip[0] != '\0') {
         char buf[512];
         snprintf(buf, sizeof buf, "%s", skip);
         for (char *t = strtok(buf, ","); t != NULL; t = strtok(NULL, ","))
            if (t[0] != '\0' && strstr(top0, t) != NULL) {
               notef("accel-jit: subtree '%s' skipped (NVC_ACCEL_SKIP)", top0);
               return false;
            }
      }
   }

   // 1. gather the subtree's Verilog sources
   static char srcs[64][512];
   int nsrc = 0;
   // VHDL->Verilog path: emit the whole subtree via vhdl2vlog (logic3d->2-state)
   // into one .v. Used when the original SV won't parse in yosys (sv2ghdl).
   if (getenv("NVC_ACCEL_FROM_VHDL")) {
      char vpath[512];
      snprintf(vpath, sizeof vpath, "%s/aj_%s_subtree.v", accel_dir, top0);
      FILE *vf = fopen(vpath, "w");
      if (vf == NULL) return false;
      ident_t seen[512];
      int nseen = 0;
      const bool ok = emit_subtree_v(scope, vf, seen, &nseen, 512);
      fclose(vf);
      if (!ok) {
         notef("accel-jit: subtree '%s' not fully translatable (%d modules)",
               top0, nseen);
         return false;
      }
      // Skip trivial subtrees (a lone flop, a clock gate, ...): accelerating one
      // costs a synth + compile + a per-cycle bridge crossing for ~no compute,
      // and the clk-alias fix made thousands of them installable -> a synth
      // explosion (10k+). Only chunks with real datapath are worth a .so. Gate
      // on module count (env NVC_ACCEL_MIN_MODULES, default 8) BEFORE synth.
      const char *minenv = getenv("NVC_ACCEL_MIN_MODULES");
      const int min_mod = minenv ? atoi(minenv) : 8;
      if (nseen < min_mod) {
         notef("accel-jit: subtree '%s' too small (%d modules) — leaving in nvc",
               top0, nseen);
         return false;
      }
      notef("accel-jit: emitted subtree '%s' -> %d modules -> %s",
            top0, nseen, vpath);
      snprintf(srcs[0], sizeof srcs[0], "%s", vpath);
      nsrc = 1;
   }
   else
      aj_collect_sources(scope, srcs, &nsrc, 64);
   if (nsrc == 0)
      return false;

   // Verilog top module name = entity (without -ARCH suffix), lowercased.
   // `top` is used only for FILE PATHS below. The gen_statemachine top-module
   // ARG must instead be the variant name emit_subtree_v gives the top module
   // (same helper, same block) — else flatten can't find the top.
   tree_t ent = (tree_kind(ref) == T_ARCH) ? tree_primary(ref) : ref;
   char top[128];
   aj_lower(top, istr(tree_ident(ent)), sizeof top);
   char top_mod[320];
   snprintf(top_mod, sizeof top_mod, "%s",
            vhdl2vlog_variant_name(tree_ident(ent), scope->where));

   // Content-hash the synthesis inputs (the emitted/collected Verilog + the top
   // module name) so the synth output is keyed by the LOGIC: a cached synth is
   // reused when the logic is unchanged (in-place update) and gen_statemachine
   // only re-runs when it actually changes. Override with NVC_ACCEL_NO_CACHE.
   uint64_t vhash = 1469598103934665603ULL;   // FNV-1a
   vhash = (vhash ^ 3u) * 1099511628211ULL;    // cache version — bump on codegen change
   // Mix gen_statemachine's mtime so a synth-tool change invalidates the cache
   // (the cached dutc/.so are the synth output; a stale one would miscompile).
   { struct stat gst;
     if (stat(aj_gen_sm(), &gst) == 0)
        vhash = (vhash ^ (uint64_t)gst.st_mtime) * 1099511628211ULL; }
   for (const char *p = top_mod; *p; p++) { vhash ^= (uint8_t)*p; vhash *= 1099511628211ULL; }
   for (int i = 0; i < nsrc; i++) {
      char *vtext = aj_read_file(srcs[i]);
      if (vtext != NULL) {
         for (const char *p = vtext; *p; p++) { vhash ^= (uint8_t)*p; vhash *= 1099511628211ULL; }
         free(vtext);
      }
   }

   char dutc[600], bridge[600], so[600];
   snprintf(dutc,   sizeof dutc,   "%s/aj_%s_%016llx.c", accel_dir, top,
            (unsigned long long)vhash);
   snprintf(bridge, sizeof bridge, "%s/aj_%s_bridge.c", accel_dir, top);
   snprintf(so,     sizeof so,     "%s/aj_%s_%016llx.so", accel_dir, top,
            (unsigned long long)vhash);

   // 2. synthesize the flattened model (gen_statemachine) — but only if this
   //    exact logic has not been synthesized before. The cached synth (keyed by
   //    the content hash above) is reused as-is for an in-place update; a logic
   //    change yields a new hash -> a fresh synth, recompiling ONLY this chunk.
   char cmd[8192];
   if (getenv("NVC_ACCEL_NO_CACHE") == NULL && access(dutc, F_OK) == 0) {
      notef("accel-jit: reusing cached synth for '%s' (logic unchanged)", top);
   }
   else {
      char dir[512]; snprintf(dir, sizeof dir, "%s", srcs[0]);
      char *slash = strrchr(dir, '/');
      if (slash) *slash = '\0';
      else snprintf(dir, sizeof dir, ".");   // bare filename: sources rel to cwd
      int off = snprintf(cmd, sizeof cmd, "cd '%s' && '%s'", dir, aj_gen_sm());
      for (int i = 0; i < nsrc; i++)
         off += snprintf(cmd + off, sizeof cmd - off, " '%s'", srcs[i]);
      // Re-synthesize with the elaboration's actual generics (width/depth/...).
      // The vhdl2vlog path emits already-elaborated modules (generics baked in),
      // so passing them would chparam a non-existent defparam and error.
      char params[256];
      if (!getenv("NVC_ACCEL_FROM_VHDL")
          && accel_verilog_params(ref, params, sizeof params) && params[0]) {
         off += snprintf(cmd + off, sizeof cmd - off, " %s", params);
         notef("accel-jit: params %s", params);
      }
      off += snprintf(cmd + off, sizeof cmd - off, " %s '%s'", top_mod, dutc);
      notef("accel-jit: synth '%s' (top module '%s') from %d source(s)",
            top, top_mod, nsrc);
      if (system(cmd) != 0 || access(dutc, F_OK) != 0) {
         notef("accel-jit: synth failed for '%s' — leaving in nvc", top);
         return false;
      }
   }

   // 3. capture the port boundary (post-elab addresses). A big datapath chunk
   // (dec) has hundreds of ports; a 64-pin cap silently dropped most of them ->
   // the chunk ran on stale inputs / undriven outputs. Size by the real port
   // count and decline (don't silently truncate) if a module is absurdly wide.
   static aj_pin_t pins[AJ_MAX_PINS];
   int npins = 0;
   aj_pin_t clk = {0}, rst = {0};
   bool have_clk = false, have_rst = false;
   const int nports = tree_ports(ent);
   for (int i = 0; i < nports; i++) {
      tree_t p = tree_port(ent, i);
      char lname[64];
      aj_lower(lname, istr(tree_ident(p)), sizeof lname);
      rt_signal_t *sig = aj_find_signal(scope, lname);
      if (sig == NULL)
         continue;
      aj_pin_t pin = {0};
      snprintf(pin.name, sizeof pin.name, "%s", lname);
      // Element size is the per-nexus byte stride; the element COUNT is
      // shared.size/elem. Using sig->nexus.width here was WRONG for a sliced
      // (multi-nexus) signal — nexus.width is only the FIRST slice's width, so
      // elem=size/width inflated and the FORCE buffer stride mismatched what
      // deposit_signal reads (a 2 landing at byte 2 -> 0x20000 logic3d fatal).
      pin.elem     = (int)sig->nexus.size;
      pin.width    = pin.elem ? (int)(sig->shared.size / pin.elem) : 1;
      pin.data     = (uint8_t *)sig->shared.data;
      pin.sig      = sig;
      // PORT_BUFFER is an output (a readable-back registered output); classify it
      // with PORT_OUT so the bridge DEPOSITS it rather than trying to drive it in,
      // matching vhdl2vlog emitting `buffer` ports as Verilog `output`.
      pin.is_output = (tree_subkind(p) == PORT_OUT || tree_subkind(p) == PORT_BUFFER);
      notef("accel-jit:   port %-10s %-3s width=%d elem=%d", lname,
            pin.is_output ? "out" : "in", pin.width, pin.elem);
      // Soundness gate: the bit0-per-element value-bit bridge handles logic3d
      // (elem=4) / std_logic (elem=1) vectors. Width is now marshalled in 32-bit
      // limbs both here and in gen_statemachine, so any width is sound; the 4096
      // cap is just the wide-int runtime's wmul scratch (uint32_t t[128]). A
      // record / integer port still declines (stays interpreted).
      if (pin.width < 1 || pin.width > 4096 || (pin.elem != 1 && pin.elem != 4)
          || !aj_marshallable_type(tree_type(p))) {
         notef("accel-jit: subtree '%s' port '%s' not value-bit marshallable "
               "(width=%d elem=%d) — leaving in nvc", top, lname, pin.width, pin.elem);
         return false;
      }
      if (strcmp(lname, "clk") == 0) { clk = pin; have_clk = true; }
      else if (strcmp(lname, "rst") == 0) { rst = pin; have_rst = true; }
      else if (npins < AJ_MAX_PINS) pins[npins++] = pin;
      else {
         notef("accel-jit: subtree '%s' exceeds %d boundary pins — leaving in nvc",
               top, AJ_MAX_PINS);
         return false;
      }
   }
   if (!have_clk) {
      notef("accel-jit: no clk port found for '%s' — leaving in nvc", top);
      return false;
   }

   // 4. register the chunk, build the fast-clk posedge table FIRST (the bank-
   //    switch classifier in aj_emit_bridge consults it), then emit the address-
   //    baked bridge (builds the chunk's deferred-output table) and compile.
   aj_chunk_t *chunk = aj_chunk_new(m);
   chunk->scope   = scope;
   chunk->bindtab = xcalloc_array(6 + (npins > 0 ? npins : 1) + 4, sizeof(void *));
   aj_build_fastclk(m, clk.sig, clk.data);
   // aj_emit_bridge writes the (now address-free) bridge .c and fills the per-run
   // address table in chunk->bindtab — both cheap; the gcc below is what we cache.
   if (!aj_emit_bridge(bridge, dutc, pins, npins, &clk,
                       have_rst ? &rst : NULL, m, chunk, false)) {
      aj_accel_teardown(m);
      return false;
   }
   // Compile only if this exact (de-baked, content-only) .so is not already
   // cached. A logic change re-hashes -> a fresh .so; unchanged logic skips BOTH
   // synth and compile, leaving just the per-run table bind below.
   if (getenv("NVC_ACCEL_NO_CACHE") == NULL && access(so, F_OK) == 0) {
      notef("accel-jit: reusing cached .so for '%s' (logic unchanged)", top);
   }
   else {
      const char *cc = getenv("NVC_ACCEL_CC");
      if (!cc) cc = "gcc -g -O3";
      // NVC_ACCEL_SMDUMP: compile in sm_dump_comb (all internal nets) for the
      // real-sim internal-net probe (see aj_emit_bridge). Clear the cache to toggle.
      const char *smd = getenv("NVC_ACCEL_SMDUMP") ? "-DSM_DUMP" : "";
      snprintf(cmd, sizeof cmd, "%s %s -shared -fPIC -o '%s' '%s'", cc, smd, so, bridge);
      if (system(cmd) != 0 || access(so, F_OK) != 0) {
         notef("accel-jit: compile failed for '%s'", top);
         aj_accel_teardown(m);
         return false;
      }
   }

   // 5. load, point the .so's AJB table at our per-run addresses, reroute
   void *dl = dlopen(so, RTLD_NOW);
   if (!dl) { warnf("accel-jit: dlopen %s: %s", so, dlerror());
              aj_accel_teardown(m); return false; }
   void (*eval)(void *, void **) = dlsym(dl, "accel_eval");
   void (*reset)(void *)         = dlsym(dl, "accel_reset");
   unsigned long (*ssize)(void)  = dlsym(dl, "accel_state_size");
   if (!eval || !reset || !ssize) {
      warnf("accel-jit: missing accel_eval/reset/state_size in %s", so);
      dlclose(dl);
      aj_accel_teardown(m);
      return false;
   }
   chunk->eval  = eval;
   chunk->reset = reset;
   chunk->dl    = dl;
   chunk->state = xcalloc(ssize());   // per-chunk state (identical .so's don't share)
   // keep the emission inputs for post-link respecialization (re-emit the
   // bridge with the link results baked in, compile, swap eval)
   chunk->rs_bridge = xstrdup(bridge);
   chunk->rs_dutc   = xstrdup(dutc);
   chunk->rs_top    = xstrdup(top);
   chunk->rs_pins   = xmalloc_array(npins > 0 ? npins : 1, sizeof(aj_pin_t));
   memcpy(chunk->rs_pins, pins, (npins > 0 ? npins : 1) * sizeof(aj_pin_t));
   chunk->rs_npins  = npins;
   chunk->rs_clk    = clk;
   chunk->rs_rst    = rst;
   chunk->rs_have_rst = have_rst;
   chunk->rs_state_size = ssize();
   g_aj_model   = m;
   chunk->reset(chunk->state);
   if (g_aj_verify) {
      // Passive companion: DON'T reroute — the interpreter keeps driving the real
      // sim; aj_verify_step runs this chunk at end of each time step and compares.
      if (g_aj_nvchunks < 64) g_aj_vchunks[g_aj_nvchunks++] = chunk;
      notef("accel-jit: VERIFY — '%s' companion installed (interp drives; accel "
            "checked per-net each step)", top);
   }
   else {
      aj_reroute(scope, chunk);
      notef("accel-jit: ACTIVE — '%s' subtree rerouted to native model", top);
      // Dead-output pruning: an output whose consumer-visible nexus has NO
      // readers (empty pending list — wave watchers and processes both live
      // there — and no downstream port) is never observed; clear its bit in
      // the model's exported sm_live_outputs so the cone cells exclusive to
      // it are skipped at run time (e.g. the retire-trace buses when the tb
      // variant doesn't sample them). VERIFY never prunes (compares all).
      for (int mi = 0; mi < 4; mi++) chunk->live_out_mask[mi] = ~0ull;
      // Opt-in only: automatic liveness detection via pending-lists is
      // UNSOUND for time-waiting readers (`wait for`/`wait until` processes
      // are not in any signal's pending list while blocked on time — the toy
      // checkers read Y that way and auto-pruning zeroed them). The tb author
      // declares dead outputs explicitly: NVC_ACCEL_PRUNE=name1,name2 — or
      // NVC_ACCEL_PRUNE=auto accepts the heuristic (sensitivity-list readers
      // and wave watchers are visible; time-waiting readers are NOT).
      const char *prune_env = getenv("NVC_ACCEL_PRUNE");
      if (prune_env != NULL && prune_env[0] != '\0') {
         const bool prune_auto = strcmp(prune_env, "auto") == 0;
         uint64_t *lom = dlsym(dl, "sm_live_outputs");
         char *dtx = aj_read_file(dutc);
         const char *ord_tab = (dtx != NULL)
            ? strstr(dtx, "const char *sm_output_order[] = {") : NULL;
         if (lom != NULL && ord_tab != NULL) {
            int pruned = 0;
            for (int i = 0; i < npins; i++) {
               if (!pins[i].is_output || pins[i].sig == NULL) continue;
               bool dead = false;
               if (prune_auto) {
                  // heuristic: live iff ANY nexus (following single-port
                  // hops) has pending readers/watchers
                  bool live = false;
                  rt_signal_t *sg = pins[i].sig;
                  rt_nexus_t *n = &sg->nexus;
                  for (unsigned nx = 0; nx < sg->n_nexus && !live;
                       nx++, n = n->chain) {
                     rt_nexus_t *t = n;
                     for (int hop = 0; hop < 4 && t != NULL; hop++) {
                        if (!aj_pending_empty(t->pending)) { live = true; break; }
                        rt_source_t *o = t->outputs;
                        if (o == NULL) break;
                        if (o->chain_output != NULL || o->tag != SOURCE_PORT
                            || o->u.port.conv_func != NULL) { live = true; break; }
                        t = o->u.port.output;
                     }
                  }
                  dead = !live;
               }
               else {
                  // explicit comma list of output names to prune
                  const char *p = prune_env;
                  size_t nl = strlen(pins[i].name);
                  while (*p != '\0') {
                     const char *e = strchr(p, ',');
                     size_t len = (e != NULL) ? (size_t)(e - p) : strlen(p);
                     if (len == nl && strncmp(p, pins[i].name, nl) == 0) {
                        dead = true; break;
                     }
                     if (e == NULL) break;
                     p = e + 1;
                  }
               }
               if (!dead) continue;
               // find this output's bit in sm_output_order
               const char *p = ord_tab;
               int idx = 0; bool found = false;
               while ((p = strchr(p, '"')) != NULL) {
                  const char *e = strchr(p + 1, '"');
                  if (e == NULL) break;
                  if ((size_t)(e - p - 1) == strlen(pins[i].name)
                      && strncmp(p + 1, pins[i].name, e - p - 1) == 0) {
                     found = true; break;
                  }
                  idx++;
                  p = e + 1;
                  if (*p == '}' || idx >= 256) break;
               }
               if (found && idx < 256) {
                  chunk->live_out_mask[idx >> 6] &= ~(1ull << (idx & 63));
                  pruned++;
               }
            }
            memcpy(lom, chunk->live_out_mask, sizeof chunk->live_out_mask);
            if (pruned > 0)
               notef("accel-jit: pruned %d unread output cone(s)", pruned);
         }
         free(dtx);
      }
   }
   return true;
}

// Recursively scan scopes for acceleration .so files.
static void accel_scan_scope(rt_model_t *m, rt_scope_t *scope,
                             const char *accel_dir)
{
   // JIT subtree path: work down from the top, accelerate the first
   // synthesizable subtree that compiles, and don't recurse into it.
   if (getenv("NVC_ACCEL_JIT") != NULL) {
      // NVC_ACCEL_PER_INSTANCE: install each LEAF instance (one with no instance
      // children) as its own chunk, so siblings become separate cacheable .so's
      // (finest-grained in-place rebuild) and the inter-instance signals become
      // chunk-to-chunk boundaries — instead of flattening the whole subtree into
      // one chunk. Default (whole-subtree) installs the largest synth subtree.
      const bool per_inst = getenv("NVC_ACCEL_PER_INSTANCE") != NULL;
      if (scope->kind == SCOPE_INSTANCE) {
         bool leaf = true;
         if (per_inst)
            for (int ci = 0; ci < scope->children.count; ci++)
               if (scope->children.items[ci]->kind == SCOPE_INSTANCE) {
                  leaf = false; break;
               }
         tree_t r = aj_scope_ref(scope);
         char tmp[512];
         // NVC_ACCEL_FROM_VHDL emits the subtree via vhdl2vlog (not the original
         // SV), so the SV-source gate doesn't apply -- plain VHDL has no
         // nvc_verilog_src attr either.
         if ((!per_inst || leaf) && r != NULL
             && (getenv("NVC_ACCEL_FROM_VHDL")
                 || accel_verilog_src(r, tmp, sizeof tmp))
             && accel_install_subtree(m, scope, r, accel_dir)
             && !per_inst)
            return;   // whole subtree accelerated; don't recurse into it
      }
      for (int ci = 0; ci < scope->children.count; ci++)
         accel_scan_scope(m, scope->children.items[ci], accel_dir);
      return;
   }

   if (scope->kind == SCOPE_INSTANCE && scope->where != NULL
       && tree_decls(scope->where) > 0) {
      tree_t hier = tree_decl(scope->where, 0);
      if (tree_kind(hier) == T_HIER) {
         // hier ref points to the original entity/block
         tree_t ref = tree_ref(hier);
         const char *entity = istr(tree_ident(ref));

         // Strip library prefix (e.g. "WORK.COUNTER8" -> "COUNTER8")
         const char *dot = strrchr(entity, '.');
         const char *modname = dot ? dot + 1 : entity;

         char mod_lower[256];
         snprintf(mod_lower, sizeof(mod_lower), "%s", modname);
         for (char *p = mod_lower; *p; p++) {
            char c = tolower((unsigned char)*p);
            *p = (isalnum((unsigned char)c) || c == '_') ? c : '_';   // valid Verilog id
         }

         char so_path[512];
         snprintf(so_path, sizeof(so_path),
                  "%s/accel-mod_%s-arch_from_verilog.so",
                  accel_dir, mod_lower);

         if (access(so_path, F_OK) == 0)
            accel_load(m, so_path);
         else {
            // No cached .so — try to compile in background.
            // Prefer the original Verilog source recovered from the
            // nvc_verilog_src attribute (Mode 1); fall back to the elaborated
            // unit's location otherwise.
            char vsrc_buf[600];
            const char *src_file;
            // synth_top is the module name handed to gen_statemachine. For the
            // VHDL-emitted path it is mod_lower (vhdl2vlog names the module so).
            // For recovered Verilog it must be the ORIGINAL Verilog module name
            // (the entity, without the "-FROM_VERILOG" arch suffix), not the
            // entity-arch combined name that mod_lower encodes.
            const char *synth_top = mod_lower;
            char vlog_top[256];
            // NVC_ACCEL_FROM_VHDL forces the vhdl2vlog path (emit clean
            // Verilog from the elaborated tree) instead of recovering the
            // original source, which for sv2ghdl designs is heavy SV that
            // yosys's read_verilog can't parse (structs/typedefs/imports).
            if (!getenv("NVC_ACCEL_FROM_VHDL")
                && accel_verilog_src(ref, vsrc_buf, sizeof(vsrc_buf))) {
               src_file = vsrc_buf;
               tree_t ent = (tree_kind(ref) == T_ARCH) ? tree_primary(ref) : ref;
               const char *ename = istr(tree_ident(ent));
               const char *edot = strrchr(ename, '.');
               snprintf(vlog_top, sizeof(vlog_top), "%s", edot ? edot + 1 : ename);
               for (char *p = vlog_top; *p; p++) {
                  char c = tolower((unsigned char)*p);
                  *p = (isalnum((unsigned char)c) || c == '_') ? c : '_';
               }
               synth_top = vlog_top;
            }
            else
               src_file = loc_file_str(tree_loc(ref));
            // If the source is not Verilog (VHDL, or SV via sv2ghdl), emit
            // synthesizable Verilog from the elaborated tree so gen_statemachine
            // has something to read. This makes --accel work for VHDL too.
            const char *ext = src_file ? strrchr(src_file, '.') : NULL;
            bool is_vlog = ext != NULL
               && (strcmp(ext, ".v") == 0 || strcmp(ext, ".sv") == 0
                   || strcmp(ext, ".vh") == 0 || strcmp(ext, ".svh") == 0);
            char emitted[600];
            if (!is_vlog) {
               // Only attempt LEAF instances: vhdl2vlog emits a single module,
               // so a hierarchy node's child instances wouldn't be defined.
               // Children are accelerated on their own via the recursion below.
               int child_insts = 0;
               for (int ci = 0; ci < scope->children.count; ci++)
                  if (scope->children.items[ci]->kind == SCOPE_INSTANCE)
                     child_insts++;

               if (child_insts > 0) {
                  src_file = NULL;   // not a leaf — leave to its children / nvc
               }
               else {
                  snprintf(emitted, sizeof(emitted), "%s/%s_from_vhdl.v",
                           accel_dir, mod_lower);
                  // Best-effort: only accelerate if FULLY translatable. A wrong
                  // but parseable model would silently corrupt results, so on any
                  // unhandled construct we decline and the leaf stays in nvc.
                  if (vhdl2vlog(scope->where, mod_lower, emitted)) {
                     notef("accel: emitted Verilog for leaf '%s' -> %s",
                           mod_lower, emitted);
                     src_file = emitted;
                  }
                  else {
                     notef("accel: '%s' not fully translatable — staying in nvc sim",
                           mod_lower);
                     src_file = NULL;
                  }
               }
            }
            if (src_file != NULL) {
               accel_bg_compile(m, synth_top, src_file, so_path);
            }
            else {
               notef("accel: no .so for module '%s' and no source",
                     mod_lower);
            }
         }
      }
   }

   for (int ci = 0; ci < scope->children.count; ci++)
      accel_scan_scope(m, scope->children.items[ci], accel_dir);
}

// Auto-discover and load acceleration .so files.
// Naming: ~/.cache/nvc/accel/accel-mod_<entity>-arch_<arch>.so
void accel_auto(rt_model_t *m)
{
   g_aj_verify = getenv("NVC_ACCEL_VERIFY") != NULL;
   g_aj_verify_skipx = getenv("NVC_ACCEL_VERIFY_X") != NULL;

   const char *home = getenv("HOME");
   if (!home) home = "/tmp";

   char accel_dir[512];
   snprintf(accel_dir, sizeof(accel_dir), "%s/.cache/nvc/accel", home);
   aj_mkdir_p(accel_dir);   // JIT writes aj_*.c/.so here; may not exist yet

   accel_scan_scope(m, root_scope(m), accel_dir);
}

void proc_reset_vtable(rt_proc_t *proc)
{
   proc->vtable = &proc_default_vtable;
}

// --- Lazy eval: deposit-driven process arming ---
//
// When a signal value changes (via deposit/put_effective), the
// deposit method arms all processes that read it. On clock edge,
// armed processes eval, unarmed processes are NOPs. After eval,
// the process disarms. The deposit re-arms when data changes again.

typedef struct {
   rt_proc_vtable_t vtable;
   proc_eval_fn     real_eval;
   uint64_t         dirty;      // bitmap: bit per input, non-zero = armed
} lazy_proc_wrap_t;

static void proc_eval_lazy(rt_model_t *m, rt_proc_t *proc)
{
   lazy_proc_wrap_t *wrap = (lazy_proc_wrap_t *)proc->vtable;
   if (wrap->dirty == 0)
      return;   // NOP — no inputs changed
   // Don't clear dirty here — the deposit from this eval's writes
   // happens in a later delta cycle. Clear it at end of cycle instead.
   // For now, mark as "ran this cycle" by saving dirty and restoring
   // after eval so the deposit can re-arm.
   uint64_t saved = wrap->dirty;
   wrap->dirty = 0;
   wrap->real_eval(m, proc);
   // If no deposits re-armed during eval, the process will be NOP
   // next cycle. If deposits DID re-arm, dirty is non-zero again.
   // For self-feeding processes (counter), the write goes through
   // put_driving which eventually calls put_effective_lazy in the
   // same or next delta — that's too late.
   // Workaround: if the process wrote anything (dirty was set before),
   // keep it armed for one more cycle.
   if (wrap->dirty == 0)
      wrap->dirty = saved;  // stay armed until deposit confirms no change
}

// Per-nexus reader entry: which process to arm, which bit to set
typedef struct {
   lazy_proc_wrap_t *wrap;
   uint64_t          bit;
} lazy_nexus_reader_t;

// Per-nexus reader array (compact, no hash, no list)
typedef struct {
   int                   count;
   lazy_nexus_reader_t  *readers;
} lazy_nexus_readers_t;

// Global: nexus -> readers mapping
static ihash_t *g_lazy_nmap = NULL;

// put_effective_lazy and nexus_lazy_vtable defined after inline helpers
static void put_effective_lazy(rt_model_t *m, rt_nexus_t *n, const void *value);
static const rt_nexus_vtable_t nexus_lazy_vtable;

void lazy_eval_install(rt_model_t *m)
{
   rt_scope_t *root = root_scope(m);
   g_lazy_nmap = ihash_new(256);
   int n_wrapped = 0;

   // Collect all W_PROC processes
   void wrap_procs(rt_scope_t *scope) {
      for (int pi = 0; pi < scope->procs.count; pi++) {
         rt_proc_t *proc = scope->procs.items[pi];
         if (proc->wakeable.kind != W_PROC)
            continue;

         // Only wrap event-sensitive always blocks (@posedge etc).
         // Skip initial blocks and delay-based always (#N).
         {
            bool wrap_this = false;
            if (proc->where != NULL && tree_kind(proc->where) == T_VERILOG) {
               vlog_node_t vn = tree_vlog(proc->where);
               if (vlog_kind(vn) == V_ALWAYS && vlog_stmts(vn) > 0) {
                  vlog_node_t first = vlog_stmt(vn, 0);
                  // V_TIMING with V_EVENT_CONTROL = event-sensitive
                  // V_TIMING with V_DELAY_CONTROL = delay-based, don't wrap
                  if (vlog_kind(first) == V_TIMING
                      && vlog_kind(vlog_value(first)) == V_EVENT_CONTROL)
                     wrap_this = true;
               }
            }
            else if (proc->where != NULL && tree_kind(proc->where) == T_PROCESS)
               wrap_this = true;  // VHDL process

            if (!wrap_this) continue;
         }

         lazy_proc_wrap_t *wrap = xcalloc(sizeof(lazy_proc_wrap_t));
         wrap->vtable.eval = proc_eval_lazy;
         wrap->vtable.reset = proc_reset_default;
         wrap->real_eval = proc->vtable->eval;
         wrap->dirty = ~(uint64_t)0;  // all bits set = armed initially
         proc->vtable = &wrap->vtable;
         n_wrapped++;

         // Register this process on signals in its scope and parent.
         // Each signal gets one bit in the dirty mask.
         // Bit 0 = first signal, bit 1 = second, etc. (up to 64)
         int bit_idx = 0;
         for (int pass = 0; pass < 2; pass++) {
            rt_scope_t *s = (pass == 0) ? scope : scope->parent;
            if (s == NULL) continue;
            for (int si = 0; si < s->signals.count && bit_idx < 64; si++) {
               rt_signal_t *sig = s->signals.items[si];
               rt_nexus_t *nx = &sig->nexus;

               // Skip 1-bit signals (likely clock/reset — not data)
               if (nx->width == 1) continue;
               uint64_t bit = UINT64_C(1) << bit_idx;
               bit_idx++;

               // Add to nexus reader array
               lazy_nexus_readers_t *nr = ihash_get(g_lazy_nmap, (uintptr_t)nx);
               if (nr == NULL) {
                  nr = xcalloc(sizeof(lazy_nexus_readers_t));
                  ihash_put(g_lazy_nmap, (uintptr_t)nx, nr);
               }
               nr->readers = xrealloc(nr->readers,
                  (nr->count + 1) * sizeof(lazy_nexus_reader_t));
               nr->readers[nr->count].wrap = wrap;
               nr->readers[nr->count].bit = bit;
               nr->count++;
            }
         }
      }
      for (int ci = 0; ci < scope->children.count; ci++)
         wrap_procs(scope->children.items[ci]);
   }
   wrap_procs(root);

   // Install lazy deposit on all nexuses
   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain)
      n->vtable = &nexus_lazy_vtable;

   notef("lazy-eval: %d processes wrapped", n_wrapped);
}

static void run_process(rt_model_t *m, rt_proc_t *proc)
{
   TRACE("run %sprocess %s", *mptr_get(proc->privdata) ? "" :  "stateless ",
         istr(proc->name));

   rt_wakeable_t *obj = &(proc->wakeable);

   if (obj->trigger != NULL && !run_trigger(m, obj->trigger))
      return;   // Filtered

   proc->vtable->eval(m, proc);
}

static void reset_scope(rt_model_t *m, rt_scope_t *s)
{
   for (int i = 0; i < s->children.count; i++)
      reset_scope(m, s->children.items[i]);

   for (int i = 0; i < s->procs.count; i++)
      reset_process(m, s->procs.items[i]);

   for (int i = 0; i < s->properties.count; i++)
      reset_property(m, s->properties.items[i]);
}

static res_memo_t *memo_resolution_fn(rt_model_t *m, rt_signal_t *signal,
                                      ffi_closure_t closure, int32_t nlits,
                                      res_flags_t flags)
{
   // Optimise some common resolution functions by memoising them

   res_memo_t *memo = ihash_get(m->res_memo, closure.handle);
   if (memo != NULL)
      return memo;

   memo = static_alloc(m, sizeof(res_memo_t));
   memo->closure = closure;
   memo->flags   = flags;

   ihash_put(m->res_memo, memo->closure.handle, memo);

   if (nlits == 0 || nlits > 16)
      return memo;

   const vhdl_severity_t old_severity = set_exit_severity(SEVERITY_NOTE);

   jit_set_silent(m->jit, true);

   // Memoise the function for all two value cases

   for (int i = 0; i < nlits; i++) {
      for (int j = 0; j < nlits; j++) {
         int8_t args[2] = { i, j };
         jit_scalar_t result;
         if (jit_try_call(m->jit, memo->closure.handle, &result,
                          memo->closure.context, args, 2)) {
            assert(result.integer < nlits && result.integer >= 0);
            memo->tab2[i][j] = result.integer;
         }
      }
   }

   // Memoise the function for all single value cases and determine if the
   // function behaves like the identity function

   bool identity = true;
   for (int i = 0; i < nlits; i++) {
      int8_t args[1] = { i };
      jit_scalar_t result;
      if (jit_try_call(m->jit, memo->closure.handle, &result,
                       memo->closure.context, args, 1)) {
         memo->tab1[i] = result.integer;
         identity = identity && (memo->tab1[i] == i);
      }
   }

   if (model_exit_status(m) == 0) {
      memo->flags |= R_MEMO;
      if (identity)
         memo->flags |= R_IDENT;
   }

   TRACE("memoised resolution function %pi for type %pT",
         jit_get_name(m->jit, closure.handle), tree_type(signal->where));

   jit_set_silent(m->jit, false);
   jit_reset_exit_status(m->jit);

   set_exit_severity(old_severity);

   return memo;
}

static inline void *nexus_effective(rt_nexus_t *n)
{
   return n->signal->shared.data + n->offset;
}

static inline void *nexus_last_value(rt_nexus_t *n)
{
   return n->signal->shared.data + n->offset + n->signal->shared.size;
}

static inline void *nexus_driving(rt_nexus_t *n)
{
   assert(n->flags & NET_F_EFFECTIVE);
   return n->signal->shared.data + n->offset + 2*n->signal->shared.size;
}

static inline void *nexus_initial(rt_nexus_t *n)
{
   assert(n->flags & NET_F_HAS_INITIAL);
   return n->signal->shared.data + n->offset + 2*n->signal->shared.size;
}

// Dispatch deposit through vtable
static void put_effective(rt_model_t *m, rt_nexus_t *n, const void *value)
{
   n->vtable->deposit(m, n, value);
}

// Deposit that arms reading processes via bitmap (lazy eval)
static void put_effective_lazy(rt_model_t *m, rt_nexus_t *n, const void *value)
{
   unsigned char *eff = nexus_effective(n);
   unsigned char *last = nexus_last_value(n);
   const size_t valuesz = n->size * n->width;

   if (!cmp_bytes(eff, value, valuesz)) {
      copy2(last, eff, value, valuesz);

      // Arm readers: one OR per process, no list walk, no hash
      lazy_nexus_readers_t *nr = ihash_get(g_lazy_nmap, (uintptr_t)n);
      if (nr != NULL) {
         for (int i = 0; i < nr->count; i++)
            nr->readers[i].wrap->dirty |= nr->readers[i].bit;
      }

      notify_event(m, n);
   }
}

static const rt_nexus_vtable_t nexus_lazy_vtable = {
   .update_driving = calculate_driving_value,
   .deposit        = put_effective_lazy,
   .read_source    = source_value,
   .notify         = notify_event_default,
};

static rt_value_t alloc_value(rt_model_t *m, rt_nexus_t *n)
{
   rt_value_t result = {};

   const size_t valuesz = n->size * n->width;
   if (valuesz > sizeof(rt_value_t)) {
      if (n->free_value != NULL) {
         result.ext = n->free_value;
         n->free_value = *(void **)result.ext;
      }
      else
         result.ext = static_alloc(m, valuesz);
   }

   return result;
}

static void free_value(rt_nexus_t *n, rt_value_t v)
{
   const size_t valuesz = n->width * n->size;
   if (valuesz > sizeof(rt_value_t)) {
      *(void **)v.ext = n->free_value;
      n->free_value = v.ext;
   }
}

static inline uint8_t *value_ptr(rt_nexus_t *n, rt_value_t *v)
{
   const size_t valuesz = n->width * n->size;
   if (valuesz <= sizeof(rt_value_t))
      return v->bytes;
   else
      return v->ext;
}

static void copy_value_ptr(rt_nexus_t *n, rt_value_t *v, const void *p)
{
   const size_t valuesz = n->width * n->size;
   if (valuesz <= sizeof(rt_value_t)) {
#if ASAN_ENABLED
      memcpy(v->bytes, p, valuesz);
#else
      v->qword = unaligned_load(p, uint64_t);
#endif
   }
   else
      memcpy(v->ext, p, valuesz);
}

static inline bool cmp_values(rt_nexus_t *n, rt_value_t a, rt_value_t b)
{
   const size_t valuesz = n->width * n->size;
   if (valuesz <= sizeof(rt_value_t))
      return a.qword == b.qword;
   else
      return cmp_bytes(a.ext, b.ext, valuesz);
}

static inline bool is_pseudo_source(source_kind_t kind)
{
   return kind == SOURCE_FORCING || kind == SOURCE_DEPOSIT
      || kind == SOURCE_IMPLICIT;
}

static void check_multiple_sources(rt_nexus_t *n, source_kind_t kind)
{
   if (n->signal->resolution != NULL || is_pseudo_source(kind))
      return;

   if (n->signal->shared.flags & SIG_F_PIPE)
      return;

   // In STD_MX mode, allow multiple sources on unresolved types.
   // Verilog regs share a single driver across all processes; the
   // translated VHDL may create multiple sources that need
   // last-writer-wins semantics.
   if (standard() == STD_MX)
      return;

   diag_t *d;
   if (is_signal_scope(n->signal->parent)) {
      rt_scope_t *root = n->signal->parent;
      for (; is_signal_scope(root->parent); root = root->parent);

      d = diag_new(DIAG_FATAL, tree_loc(root->where));
      diag_printf(d, "element %s of signal %s has multiple sources",
                  istr(tree_ident(n->signal->where)),
                  istr(tree_ident(root->where)));
      diag_hint(d, tree_loc(n->signal->where), "element %s declared here",
                istr(tree_ident(n->signal->where)));
      diag_hint(d, tree_loc(root->where), "composite signal %s declared with "
                "unresolved type %s", istr(tree_ident(root->where)),
                type_pp(tree_type(root->where)));
   }
   else {
      d = diag_new(DIAG_FATAL, tree_loc(n->signal->where));
      diag_printf(d, "unresolved signal %s has multiple sources",
                  istr(tree_ident(n->signal->where)));
      diag_hint(d, tree_loc(n->signal->where), "signal %s declared with "
                "unresolved type %s", istr(tree_ident(n->signal->where)),
                type_pp(tree_type(n->signal->where)));
   }

   if (n->sources.tag == SOURCE_DRIVER) {
      const rt_proc_t *p = n->sources.u.driver.proc;
      diag_hint(d, tree_loc(p->where), "driven by process %s", istr(p->name));
   }
   else if (n->sources.tag == SOURCE_PORT) {
      const rt_signal_t *s = n->sources.u.port.input->signal;
      tree_t where = s->where;
      if (is_signal_scope(s->parent)) {
         for (rt_scope_t *it = s->parent; is_signal_scope(it); it = it->parent)
            where = it->where;
      }

      if (tree_kind(where) == T_PORT_DECL)
         diag_hint(d, tree_loc(where), "connected to %s port %s",
                   port_mode_str(tree_subkind(where)), istr(tree_ident(where)));
      else
         diag_hint(d, tree_loc(where), "connected to signal %s",
                   istr(tree_ident(where)));
   }

   if (kind == SOURCE_DRIVER) {
      const rt_proc_t *p = get_active_proc();
      diag_hint(d, tree_loc(p->where), "driven by process %s", istr(p->name));
   }

   diag_emit(d);
   jit_abort_with_status(EXIT_FAILURE);
}

static rt_source_t *add_source(rt_model_t *m, rt_nexus_t *n, source_kind_t kind)
{
   rt_source_t *src = NULL;
   if (n->n_sources == 0)
      src = &(n->sources);
   else {
      check_multiple_sources(n, kind);

      rt_source_t **p;
      for (p = &(n->sources.chain_input); *p; p = &((*p)->chain_input))
         ;
      *p = src = static_alloc(m, sizeof(rt_source_t));
   }

   // The only interesting values of n_sources are 0, 1, and 2
   if (n->n_sources < UINT8_MAX)
      n->n_sources++;

   if (n->n_sources > 1) {
      n->flags &= ~NET_F_FAST_DRIVER;
      n->vtable = &nexus_default_vtable;   // Revert to full resolution
   }

   src->chain_input  = NULL;
   src->chain_output = NULL;
   src->tag          = kind;
   src->disconnected = 0;
   src->fastqueued   = 0;
   src->sigqueued    = 0;
   src->pseudoqueued = 0;

   switch (kind) {
   case SOURCE_DRIVER:
      {
         src->u.driver.proc  = NULL;
         src->u.driver.nexus = n;

         waveform_t *w0 = &(src->u.driver.waveforms);
         w0->when  = TIME_HIGH;
         w0->next  = NULL;
      }
      break;

   case SOURCE_PORT:
      src->u.port.conv_func = NULL;
      src->u.port.input     = NULL;
      src->u.port.output    = n;
      break;

   case SOURCE_DEPOSIT:
   case SOURCE_FORCING:
   case SOURCE_IMPLICIT:
      src->u.pseudo.nexus = n;
      src->u.pseudo.value = alloc_value(m, n);
      break;
   }

   return src;
}

static inline int map_index(rt_index_t *index, unsigned offset)
{
   if (likely(index->how >= 0))
      return offset >> index->how;
   else
      return offset / -(index->how);
}

static inline int unmap_index(rt_index_t *index, unsigned key)
{
   if (likely(index->how >= 0))
      return key << index->how;
   else
      return key * -(index->how);
}

static inline bool index_valid(rt_index_t *index, unsigned offset)
{
   if (likely(index->how >= 0))
      return (offset >> index->how) << index->how == offset;
   else
      return offset % -(index->how) == 0;
}

static void build_index(rt_signal_t *signal)
{
   const unsigned signal_w = signal->shared.size / signal->nexus.size;

   int shift = INT_MAX, gcd = 0;
   rt_nexus_t *n = &(signal->nexus);
   for (int i = 0, offset = 0; i < signal->n_nexus;
        i++, offset += n->width, n = n->chain) {
      if (offset > 0) {
         const int tzc = __builtin_ctz(offset);
         shift = MIN(shift, tzc);
      }

      // Compute greatest common divisor
      for (int b = offset; b > 0;) {
         int temp = b;
         b = gcd % b;
         gcd = temp;
      }
   }

   const int how = gcd > 1 && gcd > (1 << shift) && gcd > 1 ? -gcd : shift;
   const int count =
      how < 0 ? (signal_w - how - 1) / -how : (signal_w >> shift) + 1;

   TRACE("create index for signal %pi how=%d count=%d",
         tree_ident(signal->where), how, count);

   rt_index_t *index = xcalloc_flex(sizeof(rt_index_t), count,
                                    sizeof(rt_nexus_t *));
   index->how = how;

   n = &(signal->nexus);
   for (int i = 0, offset = 0; i < signal->n_nexus;
        i++, offset += n->width, n = n->chain) {
      index->nexus[map_index(index, offset)] = n;
   }

   free(signal->index);
   signal->index = index;
}

static void update_index(rt_signal_t *s, rt_nexus_t *n)
{
   const unsigned offset = n->offset / n->size;

   if (!index_valid(s->index, offset)) {
      TRACE("rebuild index for %pi offset=%d how=%d",
            tree_ident(s->where), offset, s->index->how);
      build_index(s);
      assert(s->index->nexus[map_index(s->index, offset)] == n);
   }
   else {
      const int elt = map_index(s->index, offset);
      assert(s->index->nexus[elt] == NULL);
      s->index->nexus[elt] = n;
   }
}

static rt_nexus_t *lookup_index(rt_signal_t *s, int *offset)
{
   if (likely(offset == 0 || s->index == NULL))
      return &(s->nexus);
   else if (!index_valid(s->index, *offset)) {
      TRACE("invalid index for %pi offset=%d how=%d", tree_ident(s->where),
            *offset, s->index->how);
      free(s->index);
      s->index = NULL;
      return &(s->nexus);
   }
   else {
      const int key = map_index(s->index, *offset);
      for (int k = key; k >= 0; k--) {
         rt_nexus_t *n = s->index->nexus[k];
         if (n != NULL) {
            *offset = unmap_index(s->index, key - k);
            return n;
         }
      }
      return &(s->nexus);
   }
}

static waveform_t *alloc_waveform(rt_model_t *m)
{
   model_thread_t *thread = model_thread(m);

   if (thread->free_waveforms == NULL) {
      // Ensure waveforms are always within one cache line
      STATIC_ASSERT(sizeof(waveform_t) <= 32);
      char *mem = static_alloc(m, WAVEFORM_CHUNK * 32);
      for (int i = 1; i < WAVEFORM_CHUNK; i++)
         free_waveform(m, (waveform_t *)(mem + i*32));

      return (waveform_t *)mem;
   }
   else {
      waveform_t *w = thread->free_waveforms;
      thread->free_waveforms = w->next;
      prefetch_write(w->next);
      w->next = NULL;
      return w;
   }
}

static void add_conversion_input(rt_model_t *m, rt_conv_func_t *cf,
                                 rt_nexus_t *in)
{
   if (cf->ninputs == cf->maxinputs) {
      const size_t per_block = MEMBLOCK_ALIGN / sizeof(conv_input_t);
      cf->maxinputs = ALIGN_UP(MAX(4, cf->maxinputs * 2), per_block);

      if (cf->inputs == cf->tail) {
         void *new = xmalloc_array(cf->maxinputs, sizeof(conv_input_t));
         memcpy(new, cf->inputs, cf->ninputs * sizeof(conv_input_t));
         cf->inputs = new;
      }
      else
         cf->inputs = xrealloc_array(cf->inputs, cf->maxinputs,
                                     sizeof(conv_input_t));
   }

   cf->inputs[cf->ninputs++] = (conv_input_t){
      .nexus  = in,
      .result = alloc_value(m, in),
   };
}

static rt_value_t *find_conversion_input(rt_conv_func_t *cf, rt_nexus_t *n)
{
   for (int i = 0; i < cf->ninputs; i++) {
      if (cf->inputs[i].nexus == n)
         return &(cf->inputs[i].result);
   }

   return NULL;
}

static void split_value(rt_nexus_t *nexus, rt_value_t *v_new,
                        rt_value_t *v_old, int offset)
{
   const int split = offset * nexus->size;
   const int oldsz = (offset + nexus->width) * nexus->size;
   const int newsz = nexus->width * nexus->size;

   if (split > sizeof(rt_value_t) && newsz > sizeof(rt_value_t)) {
      // Split the external memory with no copying
      v_new->ext = (char *)v_old->ext + split;
   }
   else if (newsz > sizeof(rt_value_t)) {
      // Wasting up to eight bytes at the start of the the old waveform
      char *ext = v_old->ext;
      v_old->qword = *(uint64_t *)ext;
      v_new->ext = ext + split;
   }
   else if (split > sizeof(rt_value_t)) {
      // Wasting up to eight bytes at the end of the the old waveform
      memcpy(v_new->bytes, v_old->ext + split, newsz);
   }
   else if (oldsz > sizeof(rt_value_t)) {
      // The memory backing this waveform is lost now but this can only
      // happen a bounded number of times as nexuses only ever shrink
      char *ext = v_old->ext;
      memcpy(v_new->bytes, ext + split, newsz);
      v_old->qword = *(uint64_t *)ext;
   }
   else {
      // This trick with shifting probably only works on little-endian
      // systems
      v_new->qword = v_old->qword >> (split * 8);
   }
}

static void clone_source(rt_model_t *m, rt_nexus_t *nexus,
                         rt_source_t *old, int offset)
{
   rt_source_t *new = add_source(m, nexus, old->tag);

   switch (old->tag) {
   case SOURCE_PORT:
      {
         new->u.port.input = old->u.port.input;

         if (old->u.port.conv_func != NULL) {
            new->u.port.conv_func = old->u.port.conv_func;
            new->u.port.conv_result = alloc_value(m, nexus);

            rt_source_t **p = &(old->u.port.conv_func->outputs);
            for (; *p != NULL; p = &((*p)->chain_output));
            *p = new;
         }
         else {
            if (old->u.port.input->width == offset)
               new->u.port.input = old->u.port.input->chain;  // Cycle breaking
            else {
               RT_LOCK(old->u.port.input->signal->lock);
               rt_nexus_t *n = clone_nexus(m, old->u.port.input, offset);
               new->u.port.input = n;
            }
            assert(new->u.port.input->width == nexus->width);
         }
      }
      break;

   case SOURCE_DRIVER:
      {
         new->u.driver.proc = old->u.driver.proc;

         // Current transaction
         waveform_t *w_new = &(new->u.driver.waveforms);
         waveform_t *w_old = &(old->u.driver.waveforms);
         w_new->when = w_old->when;
         w_new->next = NULL;

         split_value(nexus, &w_new->value, &w_old->value, offset);

         // Pending fast driver update
         if ((nexus->flags & NET_F_FAST_DRIVER) && old->fastqueued) {
            rt_nexus_t *n0 = &(nexus->signal->nexus);
            if (!n0->sources.sigqueued)
               deferq_do(&m->driverq, async_fast_driver, new);
            new->fastqueued = 1;
         }

         new->was_active = old->was_active;

         // Future transactions
         for (w_old = w_old->next; w_old; w_old = w_old->next) {
            w_new = (w_new->next = alloc_waveform(m));
            w_new->when = w_old->when;
            w_new->next = NULL;

            split_value(nexus, &w_new->value, &w_old->value, offset);

            assert(w_old->when >= m->now);
            deltaq_insert_driver(m, w_new->when - m->now, new);
         }
      }
      break;

   case SOURCE_FORCING:
   case SOURCE_DEPOSIT:
      {
         split_value(nexus, &(new->u.pseudo.value), &(old->u.pseudo.value),
                     offset);

         if (old->pseudoqueued) {
            deltaq_insert_pseudo_source(m, new);
            new->pseudoqueued = 1;
         }
      }
      break;

   case SOURCE_IMPLICIT:
      {
         // STD_MX: receiver → parent implicit link needs splitting
         // when the parent nexus is cloned during port mapping
         if (old->u.port.input == NULL)
            break;

         // Must pre-set input to old receiver nexus before clone_nexus
         // so that output handling can find us via u.port.input match.
         // add_source sets u.pseudo.value (= u.port.input due to union
         // overlap) to alloc_value which would prevent the match.
         new->u.port.input = old->u.port.input;

         if (old->u.port.input->width == offset) {
            new->u.port.input = old->u.port.input->chain;  // Cycle breaking
            // Manually link into new input's output chain since we didn't
            // call clone_nexus (which would do this via output handling)
            new->chain_output = new->u.port.input->outputs;
            new->u.port.input->outputs = new;
         }
         else {
            RT_LOCK(old->u.port.input->signal->lock);
            new->u.port.input = clone_nexus(m, old->u.port.input, offset);
            // clone_nexus output handling links us into the output chain
         }
         assert(new->u.port.input->width == nexus->width);
         new->u.port.input->flags |= NET_F_EFFECTIVE;
      }
      break;
   }
}

static rt_nexus_t *clone_nexus(rt_model_t *m, rt_nexus_t *old, int offset)
{
   assert(offset < old->width);

   rt_signal_t *signal = old->signal;
   MULTITHREADED_ONLY(assert_lock_held(&signal->lock));

   relaxed_add(&g_split_count, 1);
   g_split_last = m->now;
   // Thread 0 is the sole propagator, so it legitimately splits during the
   // parallel region; a split on a WORKER means an eval-path split slipped
   // through (e.g. a sub-range 'event) and would race — flag it.
   if (unlikely(relaxed_load(&g_par_active)) && thread_id() != 0) {
      static bool warned = false;
      if (!warned) { warned = true;
         warnf("nexus split on a worker during eval — would race; "
               "results may be incorrect"); }
   }

   signal->n_nexus++;

   if (signal->n_nexus == 2 && (old->flags & NET_F_FAST_DRIVER))
      signal->shared.flags |= NET_F_FAST_DRIVER;

   rt_nexus_t *new = static_alloc(m, sizeof(rt_nexus_t));
   new->vtable       = &nexus_default_vtable;
   new->width        = old->width - offset;
   new->size         = old->size;
   new->signal       = signal;
   new->offset       = old->offset + offset * old->size;
   new->chain        = old->chain;
   new->flags        = old->flags;
   new->active_delta = old->active_delta;
   new->event_delta  = old->event_delta;
   new->last_event   = old->last_event;
   new->rank         = old->rank;
   new->pipe_fifo    = NULL;

   old->chain = new;
   old->width = offset;

   if (old->pending == NULL)
      new->pending = NULL;
   else if (pointer_tag(old->pending) == 1)
      new->pending = old->pending;
   else {
      rt_pending_t *old_p = untag_pointer(old->pending, rt_pending_t);
      rt_pending_t *new_p = xmalloc_flex(sizeof(rt_pending_t), old_p->count,
                                         sizeof(rt_wakeable_t *));

      new_p->count = new_p->max = old_p->count;

      for (int i = 0; i < old_p->count; i++)
         new_p->wake[i] = old_p->wake[i];

      new->pending = tag_pointer(new_p, 0);
   }

   if (new->chain == NULL)
      m->nexus_tail = &(new->chain);

   if (old->n_sources > 0) {
      for (rt_source_t *it = &(old->sources); it; it = it->chain_input)
         clone_source(m, new, it, offset);
   }

   for (rt_source_t *old_o = old->outputs; old_o; old_o = old_o->chain_output) {
      assert(old_o->tag == SOURCE_PORT || old_o->tag == SOURCE_IMPLICIT);

      if (old_o->tag == SOURCE_PORT && old_o->u.port.conv_func != NULL) {
         new->outputs = old_o;
         add_conversion_input(m, old_o->u.port.conv_func, new);
      }
      else {
         rt_nexus_t *out_n;
         if (old_o->tag == SOURCE_IMPLICIT) {
            // Cycle break only when the output has a matching chain (paired
            // split, e.g. STD_MX RECEIVER).  For fan-in implicit signals
            // (e.g. 'stable where dst is scalar) the output has no chain
            // and every src nexus feeds the same dst nexus.
            if (old_o->u.port.output->width == offset
                && old_o->u.port.output->chain != NULL)
               out_n = old_o->u.port.output->chain;
            else
               out_n = old_o->u.port.output;
         }
         else if (old_o->u.port.output->width == offset)
            out_n = old_o->u.port.output->chain;   // Cycle breaking
         else {
            RT_LOCK(old_o->u.port.output->signal->lock);
            out_n = clone_nexus(m, old_o->u.port.output, offset);
         }

         for (rt_source_t *s = &(out_n->sources); s; s = s->chain_input) {
            if (s->tag != old_o->tag)
               continue;
            else if (s->u.port.input == new || s->u.port.input == old) {
               s->u.port.input = new;
               s->chain_output = new->outputs;
               new->outputs = s;
               break;
            }
         }
      }
   }

   if (signal->index == NULL && signal->n_nexus >= NEXUS_INDEX_MIN)
      build_index(signal);
   else if (signal->index != NULL)
      update_index(signal, new);

   return new;
}

static rt_nexus_t *split_nexus_slow(rt_model_t *m, rt_signal_t *s,
                                    int offset, int count)
{
   rt_nexus_t *result = NULL;
   for (rt_nexus_t *it = lookup_index(s, &offset); count > 0; it = it->chain) {
      if (offset >= it->width) {
         offset -= it->width;
         continue;
      }
      else if (offset > 0) {
         clone_nexus(m, it, offset);
         offset = 0;
         continue;
      }
      else {
         if (it->width > count)
            clone_nexus(m, it, count);

         count -= it->width;

         if (result == NULL)
            result = it;
      }
   }

   return result;
}

static inline rt_nexus_t *split_nexus(rt_model_t *m, rt_signal_t *s,
                                      int offset, int count)
{
   MULTITHREADED_ONLY(assert_lock_held(&s->lock));

   rt_nexus_t *n0 = &(s->nexus);
   if (likely(offset == 0 && n0->width == count))
      return n0;
   else if (offset == 0 && count == s->shared.size / n0->size)
      return n0;

   return split_nexus_slow(m, s, offset, count);
}

static void setup_signal(rt_model_t *m, rt_signal_t *s, tree_t where,
                         unsigned count, unsigned size, sig_flags_t flags,
                         unsigned offset)
{
   rt_scope_t *parent = model_thread(m)->active_scope;

   s->where   = where;
   s->n_nexus = 1;
   s->offset  = offset;
   s->parent  = parent;

   s->shared.flags = flags;
   s->shared.size  = count * size;

   APUSH(parent->signals, s);

   s->nexus.vtable       = &nexus_default_vtable;
   s->nexus.width        = count;
   s->nexus.size         = size;
   s->nexus.n_sources    = 0;
   s->nexus.offset       = 0;
   s->nexus.flags        = flags | NET_F_FAST_DRIVER | NET_F_HAS_INITIAL;
   s->nexus.signal       = s;
   s->nexus.pending      = NULL;
   s->nexus.pipe_fifo    = NULL;
   s->nexus.active_delta = DELTA_CYCLE_MAX;
   s->nexus.event_delta  = DELTA_CYCLE_MAX;
   s->nexus.last_event   = TIME_HIGH;

   *m->nexus_tail = &(s->nexus);
   m->nexus_tail = &(s->nexus.chain);

   m->n_signals++;
}

static void copy_sub_signal_sources(rt_scope_t *scope, void *buf, int stride)
{
   assert(is_signal_scope(scope));

   for (int i = 0; i < scope->signals.count; i++) {
      rt_signal_t *s = scope->signals.items[i];
      rt_nexus_t *n = &(s->nexus);
      for (unsigned i = 0; i < s->n_nexus; i++) {
         unsigned o = 0;
         for (rt_source_t *src = &(n->sources); src; src = src->chain_input) {
            const void *data = source_value(n, src);
            if (data == NULL)
               continue;

            memcpy(buf + s->offset + (o++ * stride), data, n->size * n->width);
         }
      }
   }

   for (int i = 0; i < scope->children.count; i++)
      copy_sub_signal_sources(scope->children.items[i], buf, stride);
}

static void convert_driving(rt_conv_func_t *cf)
{
   rt_model_t *m = get_model();

   if (cf->effective.handle == JIT_HANDLE_INVALID) {
      // Ensure effective value is only updated once per cycle
      if (cf->when == m->now && cf->iteration == m->iteration)
         return;

      cf->when = m->now;
      cf->iteration = m->iteration;
   }

   TRACE("call driving conversion function %pi",
         jit_get_name(m->jit, cf->driving.handle));

   model_thread_t *thread = model_thread(m);

   const uint32_t mark = tlab_mark(thread->tlab);

   jit_scalar_t context = { .pointer = cf->driving.context };
   jit_scalar_t arg = { .pointer = cf }, result;
   if (!jit_fastcall(m->jit, cf->driving.handle, &result, context, arg,
                     thread->tlab))
      m->force_stop = true;

   tlab_trim(thread->tlab, mark);
}

static void convert_effective(rt_conv_func_t *cf)
{
   rt_model_t *m = get_model();

   // Ensure effective value is only updated once per cycle
   if (cf->when == m->now && cf->iteration == m->iteration)
      return;

   cf->when = m->now;
   cf->iteration = m->iteration;

   TRACE("call effective conversion function %pi",
         jit_get_name(m->jit, cf->effective.handle));

   model_thread_t *thread = model_thread(m);

   const uint32_t mark = tlab_mark(thread->tlab);

   jit_scalar_t context = { .pointer = cf->effective.context };
   jit_scalar_t arg = { .pointer = cf }, result;
   if (!jit_fastcall(m->jit, cf->effective.handle, &result, context, arg,
                     thread->tlab))
      m->force_stop = true;

   tlab_trim(thread->tlab, mark);
}

static void *source_value(rt_nexus_t *nexus, rt_source_t *src)
{
   switch (src->tag) {
   case SOURCE_DRIVER:
      if (unlikely(src->disconnected))
         return NULL;
      else
         return value_ptr(nexus, &(src->u.driver.waveforms.value));

   case SOURCE_PORT:
      if (unlikely(standard() == STD_MX)) {
         // Mixed mode: a port with no real driver is not a source.
         // Walk SOURCE_PORT chain transitively until we find a
         // SOURCE_DRIVER (real driver) or detect a cycle (circular
         // back-ref from inout bidirectional mapping).
         rt_nexus_t *input = src->u.port.input;
         rt_nexus_t *seen[8];   // small bounded cycle-guard
         int n_seen = 0;
         seen[n_seen++] = nexus;
         bool has_driver = false;
         while (input != NULL && n_seen < 8) {
            bool already = false;
            for (int k = 0; k < n_seen; k++)
               if (seen[k] == input) { already = true; break; }
            if (already)
               break;   // cycle (e.g. inout back-ref); abandon this port
            seen[n_seen++] = input;
            rt_nexus_t *next = NULL;
            for (rt_source_t *s = &(input->sources); s; s = s->chain_input) {
               if (s->tag == SOURCE_DRIVER) {
                  has_driver = true;
                  break;
               }
               else if (s->tag == SOURCE_PORT && next == NULL)
                  next = s->u.port.input;
            }
            if (has_driver)
               break;
            input = next;
         }
         if (!has_driver)
            return NULL;
      }
      if (likely(src->u.port.conv_func == NULL)) {
         if (src->u.port.input->flags & NET_F_EFFECTIVE)
            return nexus_driving(src->u.port.input);
         else
            return nexus_effective(src->u.port.input);
      }
      else {
         convert_driving(src->u.port.conv_func);
         return value_ptr(nexus, &src->u.port.conv_result);
      }

   case SOURCE_FORCING:
   case SOURCE_DEPOSIT:
      assert(src->disconnected);
      return NULL;

   case SOURCE_IMPLICIT:
      if (standard() == STD_MX) {
         rt_nexus_t *input = src->u.port.input;
         if (input == NULL)
            return NULL;
         tree_t iwhere = input->signal->where;
         if (tree_kind(iwhere) == T_IMPLICIT_SIGNAL
             && tree_subkind(iwhere) == IMPLICIT_DRIVER) {
            // Forward implicit: auto-created 'driver → parent.
            // Only contribute when the driver has at least one
            // SOURCE_DRIVER (a process writes to it).  Auto-drivers
            // with no SOURCE_DRIVER are unused and should not
            // contribute stale initial values to resolution.
            if (input->n_sources == 0)
               return NULL;
            bool has_driver = false;
            for (rt_source_t *si = &(input->sources);
                 si; si = si->chain_input)
               if (si->tag == SOURCE_DRIVER) {
                  has_driver = true; break;
               }
            if (!has_driver)
               return NULL;
            if (input->flags & NET_F_EFFECTIVE)
               return nexus_driving(input);
            else
               return nexus_effective(input);
         }
         else {
            // Reverse implicit: receiver → parent signal.
            // Deposit (:=) writes directly to nexus_effective without
            // creating a SOURCE_DRIVER.  Only contribute when the receiver
            // has actually been deposited to (last_event < TIME_HIGH);
            // otherwise the stale initial value poisons resolution.
            if (input->last_event < TIME_HIGH)
               return nexus_effective(input);
         }
      }
      return NULL;
   }

   return NULL;
}

static void call_resolution(rt_model_t *m, rt_nexus_t *n, res_memo_t *r,
                            int nonnull, rt_source_t *s0)
{
   if ((n->flags & NET_F_R_IDENT) && nonnull == 1) {
      // Resolution function behaves like identity for a single driver
      put_driving(m, n, source_value(n, s0));
      // Rewrite: identity with single driver is just a direct deposit
      if (s0->tag == SOURCE_DRIVER)
         n->vtable = &nexus_single_driver_vtable;
   }
   else if ((r->flags & R_MEMO) && nonnull == 1) {
      // Resolution function has been memoised so do a table lookup

      model_thread_t *thread = model_thread(m);
      assert(thread->tlab != NULL);

      const uint32_t mark = tlab_mark(thread->tlab);

      void *resolved = tlab_alloc(thread->tlab, n->width * n->size);
      char *p0 = source_value(n, s0);

      for (int j = 0; j < n->width; j++) {
         const int index = ((uint8_t *)p0)[j];
         ((int8_t *)resolved)[j] = r->tab1[index];
      }

      put_driving(m, n, resolved);
      tlab_trim(thread->tlab, mark);
      // Rewrite: single driver with memo table, skip source walk next time
      if (s0->tag == SOURCE_DRIVER)
         n->vtable = &nexus_memo1_vtable;
   }
   else if ((r->flags & R_MEMO) && nonnull == 2) {
      // Resolution function has been memoised so do a table lookup

      model_thread_t *thread = model_thread(m);
      assert(thread->tlab != NULL);

      const uint32_t mark = tlab_mark(thread->tlab);

      void *resolved = tlab_alloc(thread->tlab, n->width * n->size);

      char *p0 = source_value(n, s0), *p1 = NULL;
      for (rt_source_t *s1 = s0->chain_input;
           s1 && (p1 = source_value(n, s1)) == NULL;
           s1 = s1->chain_input)
         ;

      for (int j = 0; j < n->width; j++)
         ((int8_t *)resolved)[j] = r->tab2[(int)p0[j]][(int)p1[j]];

      put_driving(m, n, resolved);
      tlab_trim(thread->tlab, mark);
   }
   else if (r->flags & R_COMPOSITE) {
      // Call resolution function of composite type

      rt_scope_t *scope = n->signal->parent, *rscope = scope;
      while (is_signal_scope(scope->parent)) {
         scope = scope->parent;
         if (scope->flags & SCOPE_F_RESOLVED)
            rscope = scope;
      }

      TRACE("resolved composite signal needs %d bytes", scope->size);

      model_thread_t *thread = model_thread(m);
      assert(thread->tlab != NULL);

      const uint32_t mark = tlab_mark(thread->tlab);

      uint8_t *inputs = tlab_alloc(thread->tlab, nonnull * scope->size);
      copy_sub_signal_sources(scope, inputs, scope->size);

      jit_scalar_t result;
      if (jit_try_call(m->jit, r->closure.handle, &result,
                       r->closure.context, inputs, nonnull))
         put_driving(m, n, result.pointer + n->signal->offset
                     + n->offset - rscope->offset);
      else
         m->force_stop = true;

      tlab_trim(thread->tlab, mark);
   }
   else {
      model_thread_t *thread = model_thread(m);
      assert(thread->tlab != NULL);

      const uint32_t mark = tlab_mark(thread->tlab);

      void *resolved = tlab_alloc(thread->tlab, n->width * n->size);

      for (int j = 0; j < n->width; j++) {
#define CALL_RESOLUTION_FN(type) do {                                   \
            type vals[nonnull];                                         \
            unsigned o = 0;                                             \
            for (rt_source_t *s = s0; s; s = s->chain_input) {          \
               const void *data = source_value(n, s);                   \
               if (data != NULL)                                        \
                  vals[o++] = ((const type *)data)[j];                  \
            }                                                           \
            assert(o == nonnull);                                       \
            type *p = (type *)resolved;                                 \
            jit_scalar_t result;                                        \
            if (!jit_try_call(m->jit, r->closure.handle, &result,       \
                              r->closure.context, vals, nonnull))       \
               m->force_stop = true;                                    \
            p[j] = result.integer;                                      \
         } while (0)

         FOR_ALL_SIZES(n->size, CALL_RESOLUTION_FN);
      }

      put_driving(m, n, resolved);
      tlab_trim(thread->tlab, mark);
   }
}

static rt_source_t *get_pseudo_source(rt_model_t *m, rt_nexus_t *n,
                                      source_kind_t kind)
{
   assert(is_pseudo_source(kind));

   if (n->n_sources > 0) {
      for (rt_source_t *s = &(n->sources); s; s = s->chain_input) {
         if (s->tag == kind)
            return s;
      }
   }

   return add_source(m, n, kind);
}

__attribute__((cold, noinline))
static void schedule_implicit_update(rt_model_t *m, rt_nexus_t *n)
{
   rt_implicit_t *imp = container_of(n->signal, rt_implicit_t, signal);

   if (!imp->wakeable.pending) {
      deferq_do(&m->implicitq, async_update_implicit_signal, imp);
      set_pending(&imp->wakeable);
   }
}

static void calculate_driving_value(rt_model_t *m, rt_nexus_t *n)
{

   // Algorithm for driving values is in LRM 08 section 14.7.3.2

   // If S has no source, then the driving value of S is given by the
   // default value associated with S
   if (n->n_sources == 0) {
      put_driving(m, n, nexus_initial(n));
      return;
   }

   res_memo_t *r = n->signal->resolution;

   int nonnull = 0;
   rt_source_t *s0 = NULL;
   for (rt_source_t *s = &(n->sources); s; s = s->chain_input) {
      if (s->disconnected)
         continue;
      else if (s->tag == SOURCE_FORCING) {
         // If S is driving-value forced, the driving value of S is
         // unchanged from its previous value; no further steps are
         // required.
         put_driving(m, n, value_ptr(n, &(s->u.pseudo.value)));
         return;
      }
      else if (s->tag == SOURCE_DEPOSIT) {
         // If a driving-value deposit is scheduled for S or for a
         // signal of which S is a subelement, the driving value of S is
         // the driving deposit value for S or the element of the
         // driving deposit value for the signal of which S is a
         // subelement, as appropriate.
         s->disconnected = 1;
         put_driving(m, n, value_ptr(n, &(s->u.pseudo.value)));
         return;
      }
      else if (unlikely(s->tag == SOURCE_IMPLICIT)) {
         if (standard() == STD_MX) {
            // Both forward (driver→receiver) and reverse (receiver→parent)
            // implicit sources contribute to driving value.
            const void *sv = source_value(n, s);
            if (sv != NULL) {
               nonnull++;
               if (s0 == NULL) s0 = s;
            }
            continue;
         }
         // At least one of the inputs is active so schedule an update
         // to the value of an implicit 'TRANSACTION or 'QUIET signal
         schedule_implicit_update(m, n);
         return;
      }
      else if (unlikely(standard() == STD_MX
                        && s->tag == SOURCE_PORT)) {
         // Mixed mode: skip port with no real driver.
         // Walk SOURCE_PORT chain transitively: a nested port-binding
         // (inner OUT → outer OUT → tb signal) has no SOURCE_DRIVER on
         // the immediate input, only further along the chain.  Cycle
         // guard handles the inout back-ref case.
         rt_nexus_t *input = s->u.port.input;
         rt_nexus_t *seen[8];
         int n_seen = 0;
         seen[n_seen++] = n;
         bool has_driver = false;
         while (input != NULL && n_seen < 8) {
            bool already = false;
            for (int k = 0; k < n_seen; k++)
               if (seen[k] == input) { already = true; break; }
            if (already)
               break;
            seen[n_seen++] = input;
            rt_nexus_t *next = NULL;
            for (rt_source_t *si = &(input->sources);
                 si; si = si->chain_input) {
               if (si->tag == SOURCE_DRIVER
                   || si->tag == SOURCE_IMPLICIT) {
                  has_driver = true;
                  break;
               }
               else if (si->tag == SOURCE_PORT && next == NULL)
                  next = si->u.port.input;
            }
            if (has_driver)
               break;
            input = next;
         }
         if (!has_driver)
            continue;
         if (s0 == NULL)
            s0 = s;
      }
      else if (s0 == NULL)
         s0 = s;
      nonnull++;
   }

   if (unlikely(s0 == NULL)) {
      // If S is of signal kind register and all the sources of S have
      // values determined by the null transaction, then the driving
      // value of S is unchanged from its previous value.
      if (n->signal->shared.flags & SIG_F_REGISTER)
         put_driving(m, n, nexus_effective(n));
      else if (r == NULL || is_pseudo_source(n->sources.tag)
               || (nonnull == 0 && standard() == STD_MX))
         put_driving(m, n, nexus_initial(n));
      else
         call_resolution(m, n, r, nonnull, s0);
   }
   else if (r == NULL) {
      switch (s0->tag) {
      case SOURCE_DRIVER:
         // If S has one source that is a driver and S is not a resolved
         // signal, then the driving value of S is the current value of
         // that driver.
         assert(!s0->disconnected);
         put_driving(m, n, value_ptr(n, &(s0->u.driver.waveforms.value)));
         // Rewrite vtable: next call skips source walk entirely
         if (nonnull == 1)
            n->vtable = &nexus_single_driver_vtable;
         break;

      case SOURCE_PORT:
         // If S has one source that is a port and S is not a resolved
         // signal, then the driving value of S is the driving value of
         // the formal part of the association element that associates S
         // with that port
         if (likely(s0->u.port.conv_func == NULL)) {
            if (s0->u.port.input->flags & NET_F_EFFECTIVE)
               put_driving(m, n, nexus_driving(s0->u.port.input));
            else
               put_driving(m, n, nexus_effective(s0->u.port.input));
         }
         else {
            convert_driving(s0->u.port.conv_func);
            put_driving(m, n, value_ptr(n, &(s0->u.port.conv_result)));
         }
         break;

      case SOURCE_IMPLICIT:
         // STD_MX: implicit source is the sole active source.
         // For reverse (receiver→parent): read receiver's effective value.
         // For forward (driver→receiver): read driver's driving value.
         if (s0->u.port.input != NULL) {
            if (s0->u.port.input->flags & NET_F_EFFECTIVE)
               put_driving(m, n, nexus_driving(s0->u.port.input));
            else
               put_driving(m, n, nexus_effective(s0->u.port.input));
         }
         break;

      default:
         break;
      }
   }
   else {
      // Otherwise, the driving value of S is obtained by executing the
      // resolution function associated with S

      call_resolution(m, n, r, nonnull, s0);
   }
}

static void calculate_effective_value(rt_model_t *m, rt_nexus_t *n)
{
   // Algorithm for effective values is in LRM 08 section 14.7.7.3

   // If S is a connected port of mode in or inout, then the effective
   // value of S is the same as the effective value of the actual part
   // of the association element that associates an actual with S
   if (n->flags & NET_F_INOUT) {
      for (rt_source_t *s = n->outputs; s; s = s->chain_output) {
         if (s->tag == SOURCE_PORT) {
            if (likely(s->u.port.conv_func == NULL))
               put_effective(m, n, nexus_effective(s->u.port.output));
            else {
               rt_value_t *v = find_conversion_input(s->u.port.conv_func, n);
               assert(v != NULL);

               convert_effective(s->u.port.conv_func);
               put_effective(m, n, value_ptr(n, v));
            }
            return;
         }
      }
   }

   // If S is a signal declared by a signal declaration, a port of mode
   // out or buffer, or an unconnected port of mode inout, then the
   // effective value of S is the same as the driving value of S.
   if (n->flags & NET_F_EFFECTIVE)
      put_effective(m, n, nexus_driving(n));

   // If S is an unconnected port of mode in, the effective value of S
   // is given by the default value associated with S.
}

static void calculate_initial_value(rt_model_t *m, rt_nexus_t *n)
{
   calculate_driving_value(m, n);

   if (n->flags & NET_F_EFFECTIVE) {
      // Driving and effective values must be calculated separately
      assert(n->flags & NET_F_PENDING);
   }
   else {
      // Effective value is always the same as the driving value
      memcpy(nexus_last_value(n), nexus_effective(n), n->size * n->width);
   }
}

static int nexus_rank(rt_nexus_t *n)
{
   if (n->rank > 0)
      return n->rank;   // Already calculated
   else if (n->n_sources > 0) {
      int rank = 0;
      for (rt_source_t *s = &(n->sources); s; s = s->chain_input) {
         if (s->tag != SOURCE_PORT)
            continue;
         else if (s->u.port.conv_func != NULL) {
            rt_conv_func_t *cf = s->u.port.conv_func;
            for (int i = 0; i < cf->ninputs; i++)
               rank = MAX(rank, nexus_rank(cf->inputs[i].nexus) + 1);
         }
         else
            rank = MAX(rank, nexus_rank(s->u.port.input) + 1);
      }
      return (n->rank = rank);
   }
   else
      return 0;
}

cover_data_t *get_coverage(rt_model_t *m)
{
   return m->cover;
}

#if TRACE_SIGNALS
static void dump_one_signal(rt_model_t *m, rt_scope_t *scope, rt_signal_t *s,
                            tree_t alias)
{
   rt_nexus_t *n = &(s->nexus);

   LOCAL_TEXT_BUF tb = tb_new();
   if (is_signal_scope(scope))
      tb_printf(tb, "%s.", istr(scope->name));
   tb_cat(tb, istr(tree_ident(alias ?: s->where)));
   if (alias != NULL)
      tb_append(tb, '*');

   for (int nth = 0; nth < s->n_nexus; nth++, n = n->chain) {
      int n_outputs = 0;
      for (rt_source_t *s = n->outputs; s != NULL; s = s->chain_output)
         n_outputs++;

      const void *driving = NULL;
      if (n->flags & NET_F_EFFECTIVE)
         driving = nexus_driving(n);

      fprintf(stderr, "%-20s %-5d %-4d %-7d %-7d %-4d ",
              nth == 0 ? tb_get(tb) : "+",
              n->width, n->size, n->n_sources, n_outputs, n->rank);

      if (n->event_delta == m->iteration && n->last_event == m->now)
         fprintf(stderr, "%s -> ", fmt_nexus(n, nexus_last_value(n)));

      fputs(fmt_nexus(n, nexus_effective(n)), stderr);

      if (driving != NULL)
         fprintf(stderr, " (%s)", fmt_nexus(n, driving));

      fputs("\n", stderr);
   }
}

static void dump_signals(rt_model_t *m, rt_scope_t *scope)
{
   if (scope->signals.count == 0 && scope->children.count == 0)
      return;

   if (!is_signal_scope(scope)) {
      const char *sname = istr(scope->name);
      fprintf(stderr, "== %s ", sname);
      for (int pad = 74 - strlen(sname); pad > 0; pad--)
         fputc('=', stderr);
      fputc('\n', stderr);

      fprintf(stderr, "%-20s %5s %4s %7s %7s %4s %s\n",
              "Signal", "Width", "Size", "Sources", "Outputs", "Rank", "Value");
   }

   for (int i = 0; i < scope->signals.count; i++)
      dump_one_signal(m, scope, scope->signals.items[i], NULL);

   for (int i = 0; i < scope->aliases.count; i++) {
      rt_alias_t *a = scope->aliases.items[i];
      dump_one_signal(m, scope, a->signal, a->where);
   }

   for (int i = 0; i < scope->children.count; i++) {
      rt_scope_t *c = scope->children.items[i];
      if (is_signal_scope(c))
         dump_signals(m, c);
   }

   for (int i = 0; i < scope->children.count; i++) {
      rt_scope_t *c = scope->children.items[i];
      if (!is_signal_scope(c))
         dump_signals(m, c);
   }
}
#endif   // TRACE_SIGNALS

static text_buf_t *signal_full_name(rt_signal_t *s)
{
   text_buf_t *tb = tb_new();
   if (is_signal_scope(s->parent))
      tb_printf(tb, "%s.", istr(s->parent->name));
   tb_cat(tb, istr(tree_ident(s->where)));
   return tb;
}

static void check_undriven_std_logic(rt_nexus_t *n)
{
   // Print a warning if any STD_LOGIC signal has multiple sources one
   // of which is an undriven port with initial value 'U'. The resolved
   // value will then always be 'U' which often confuses users.

   if (n->n_sources < 2 || !(n->signal->shared.flags & SIG_F_STD_LOGIC)
       || standard() == STD_MX)
      return;

   rt_signal_t *undriven = NULL;
   for (rt_source_t *s = &(n->sources); s; s = s->chain_input) {
      if (s->tag == SOURCE_PORT && s->u.port.conv_func == NULL) {
         rt_nexus_t *input = s->u.port.input;
         if (input->n_sources == 0) {
            const unsigned char *init = nexus_effective(input), *p = init;
            for (; *p == 0 && p < init + input->width; p++);

            if (p == init + input->width)
               undriven = s->u.port.input->signal;
         }
      }
   }

   if (undriven == NULL)
      return;

   LOCAL_TEXT_BUF sig_name = signal_full_name(n->signal);
   LOCAL_TEXT_BUF port_name = signal_full_name(undriven);

   const loc_t *sig_loc = tree_loc(n->signal->where);
   rt_scope_t *sig_scope = n->signal->parent;
   for (; is_signal_scope(sig_scope); sig_scope = sig_scope->parent)
      sig_loc = tree_loc(sig_scope->where);

   const loc_t *port_loc = tree_loc(undriven->where);
   rt_scope_t *port_scope = undriven->parent;
   for (; is_signal_scope(port_scope); port_scope = port_scope->parent)
      port_loc = tree_loc(port_scope->where);

   diag_t *d = diag_new(DIAG_WARN, sig_loc);
   diag_printf(d, "%ssignal %s has %d sources including port %s which has "
               "initial value 'U' and no driver in instance %s",
               n->signal->n_nexus > 1 ? "sub-element of " : "",
               tb_get(sig_name), n->n_sources,
               tb_get(port_name), istr(tree_ident(port_scope->where)));
   diag_hint(d, sig_loc, "signal %s declared here", tb_get(sig_name));
   diag_hint(d, port_loc, "sourced by port %s which always contributes 'U'",
             tb_get(port_name));
   diag_hint(d, NULL, "the resolved value will always be 'U' which was almost "
             "certainly not intended");
   diag_emit(d);

   // Prevent multiple warnings for the same signal
   n->signal->shared.flags &= ~SIG_F_STD_LOGIC;
}

static void create_processes(rt_model_t *m, rt_scope_t *s)
{
   for (int i = 0; i < s->children.count; i++) {
      if (s->children.items[i]->kind == SCOPE_INSTANCE)
         create_processes(m, s->children.items[i]);
   }

   if (s->kind != SCOPE_INSTANCE)
      return;

   tree_t hier = tree_decl(s->where, 0);
   assert(tree_kind(hier) == T_HIER);

   LOCAL_TEXT_BUF tb = tb_new();
   get_path_name(s, tb);

   ident_t path = ident_new(tb_get(tb));
   ident_t sym_prefix = tree_ident2(hier);

   const int nstmts = tree_stmts(s->where);
   for (int i = 0; i < nstmts; i++) {
      tree_t t = tree_stmt(s->where, i);
      switch (tree_kind(t)) {
      case T_VERILOG:
         {
            ident_t name = tree_ident(t);
            ident_t sym = ident_prefix(sym_prefix, name, '.');

            rt_proc_t *p = xcalloc(sizeof(rt_proc_t));
            p->vtable    = &proc_default_vtable;
            p->where     = t;
            p->name      = ident_prefix(path, ident_downcase(name), ':');
            p->wakeable.fused_cone =
               (strstr(istr(p->name), "comb_fused") != NULL);
            p->handle    = jit_lazy_compile(m->jit, sym);
            p->scope     = s;
            p->privdata  = mptr_new(m->mspace, "process privdata");

            switch (vlog_kind(tree_vlog(p->where))) {
            case V_ASSIGN:
            case V_UDP_TABLE:
            case V_GATE_INST:
               p->wakeable.kind = W_ASSIGN;
               break;
            case V_INITIAL:
            case V_ALWAYS:
               p->wakeable.kind = W_PROC;
               break;
            default:
               should_not_reach_here();
            }

            p->wakeable.pending   = false;
            p->wakeable.delayed   = false;
            p->wakeable.postponed = false;

            APUSH(s->procs, p);
         }
         break;

      case T_PROCESS:
         {
            ident_t name = tree_ident(t);
            ident_t sym = ident_prefix(sym_prefix, name, '.');

            rt_proc_t *p = xcalloc(sizeof(rt_proc_t));
            p->vtable    = &proc_default_vtable;
            p->where     = t;
            p->name      = ident_prefix(path, ident_downcase(name), ':');
            p->wakeable.fused_cone =
               (strstr(istr(p->name), "comb_fused") != NULL);
            p->handle    = jit_lazy_compile(m->jit, sym);
            p->scope     = s;
            p->privdata  = mptr_new(m->mspace, "process privdata");

            p->wakeable.kind      = W_PROC;
            p->wakeable.pending   = false;
            p->wakeable.delayed   = false;
            p->wakeable.postponed = !!(tree_flags(t) & TREE_F_POSTPONED);

            APUSH(s->procs, p);
         }
         break;

      case T_PSL_DIRECT:
         {
            psl_node_t psl = tree_psl(t);

            const psl_kind_t kind = psl_kind(psl);
            if (kind != P_ASSERT && kind != P_COVER)
               continue;

            ident_t name = tree_ident(t);
            ident_t sym = ident_prefix(s->name, name, '.');

            rt_prop_t *p = xcalloc(sizeof(rt_prop_t));
            p->where    = tree_psl(t);
            p->handle   = jit_lazy_compile(m->jit, sym);
            p->scope    = s;
            p->name     = sym;
            p->privdata = mptr_new(m->mspace, "property privdata");

            p->wakeable.kind    = W_PROPERTY;
            p->wakeable.pending = false;
            p->wakeable.delayed = false;

            APUSH(s->properties, p);
         }
         break;

      default:
         break;
      }
   }
}

void model_reset(rt_model_t *m)
{
   MODEL_ENTRY(m);

   // Re-read options as these may have changed
   m->stop_delta = opt_get_int(OPT_STOP_DELTA);
   m->shuffle    = opt_get_int(OPT_SHUFFLE_PROCS);

   __trace_on = opt_get_int(OPT_RT_TRACE);

   create_processes(m, m->root);

   nvc_rusage(&m->ready_rusage);

   // Initialisation is described in LRM 93 section 12.6.4

   reset_scope(m, m->root);

   if (m->force_stop)
      return;   // Error in intialisation

#if TRACE_SIGNALS > 0
   if (__trace_on)
      dump_signals(m, m->root);
#endif

   TRACE("calculate initial signal values");

   model_thread_t *thread = model_thread(m);
   thread->tlab = tlab_acquire(m->mspace);

   // The signals in the model are updated as follows in an order such
   // that if a given signal R depends upon the current value of another
   // signal S, then the current value of S is updated prior to the
   // updating of the current value of R.

   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
      // The initial value of each driver is the default value of the signal
      if (n->n_sources > 0) {
         for (rt_source_t *s = &(n->sources); s; s = s->chain_input) {
            if (s->tag == SOURCE_DRIVER)
               copy_value_ptr(n, &(s->u.driver.waveforms.value),
                              nexus_effective(n));
         }
      }

      const int rank = nexus_rank(n);
      if (rank > MAX_RANK)
         fatal_at(tree_loc(n->signal->where), "signal rank %d is greater "
                  "than the maximum supported %d", rank, MAX_RANK);
      else if (rank > 0 || n->n_sources > 1)
         heap_insert(m->driving_heap, rank, n);
      else {
         calculate_initial_value(m, n);
         check_undriven_std_logic(n);
      }
   }

   while (heap_size(m->driving_heap) > 0) {
      rt_nexus_t *n = heap_extract_min(m->driving_heap);
      calculate_initial_value(m, n);
      check_undriven_std_logic(n);
   }

   // Update effective values after all initial driving values calculated
   while (heap_size(m->effective_heap) > 0) {
      rt_nexus_t *n = heap_extract_min(m->effective_heap);
      n->flags &= ~NET_F_PENDING;

      calculate_effective_value(m, n);
   }

   tlab_reset(thread->tlab);   // No allocations can be live past here

   run_callbacks(m, END_OF_INITIALISATION);

   // Route escaping unconstrained results (the per-eval logic3d churn) into a
   // reset-per-eval arena instead of the collected heap, so GC never fires
   // during the run. Installed only AFTER init so the persistent live set
   // (allocated above) stays on the heap. RTL-only; gated.
   if (getenv("NVC_EVAL_ARENA") != NULL)
      jit_eval_arena_enable(true);

   // Install fast path for thread context access now that the thread local
   // has been allocated and init is complete. NB this caches thread 0's JIT
   // thread-local for ALL threads, which is only valid single-threaded; skip
   // it when the parallel process scheduler will run process bodies on worker
   // threads (each needs its own per-thread JIT state via jit_thread_local).
   if (getenv("NVC_PARALLEL_PROCS") == NULL)
      jit_thread_install_fast_path();

   // Load compiled acceleration if available via environment
   // (--accel command line option is handled in nvc.c after model_reset)
   const char *accel_env = getenv("NVC_USE_ACCEL");
   if (accel_env != NULL)
      accel_load(m, accel_env);
}

static void update_property(rt_model_t *m, rt_prop_t *prop)
{
   TRACE("update property %pi state %s", prop->name,
         trace_states(&prop->state));

   rt_wakeable_t *obj = &(prop->wakeable);

   if (obj->trigger != NULL && !run_trigger(m, obj->trigger))
      return;   // Filtered

   model_thread_t *thread = model_thread(m);
   assert(thread->tlab != NULL);

   thread->active_obj = obj;
   thread->active_scope = prop->scope;

   jit_scalar_t args[] = {
      { .pointer = *mptr_get(prop->privdata) ?: (void *)-1 },
      { .pointer = *mptr_get(prop->scope->privdata) },
      { .integer = -1 },
   };

   mask_clearall(&prop->newstate);
   prop->strong = false;

   size_t bit = -1;
   while (mask_iter(&prop->state, &bit)) {
      args[2].integer = bit;

      if (!jit_vfastcall(m->jit, prop->handle, args, ARRAY_LEN(args),
                         NULL, 0, thread->tlab))
         m->force_stop = true;
   }

   tlab_reset(thread->tlab);   // No allocations can be live past here

   thread->active_obj = NULL;
   thread->active_scope = NULL;

   TRACE("new state %s%s", trace_states(&prop->newstate),
         prop->strong ? " strong" : "");

   mask_copy(&prop->state, &prop->newstate);

   m->liveness |= prop->strong;
}

static void sched_event(rt_model_t *m, void **pending, rt_wakeable_t *obj)
{
   if (*pending == NULL)
      *pending = tag_pointer(obj, 1);
   else if (pointer_tag(*pending) == 1) {
      rt_wakeable_t *cur = untag_pointer(*pending, rt_wakeable_t);
      if (cur == obj)
         return;

      rt_pending_t *p = xmalloc_flex(sizeof(rt_pending_t), PENDING_MIN,
                                     sizeof(rt_wakeable_t *));
      p->max = PENDING_MIN;
      p->count = 2;
      p->wake[0] = cur;
      p->wake[1] = obj;

      *pending = tag_pointer(p, 0);
   }
   else {
      rt_pending_t *p = untag_pointer(*pending, rt_pending_t);

      for (int i = 0; i < p->count; i++) {
         if (p->wake[i] == NULL || p->wake[i] == obj) {
            p->wake[i] = obj;
            return;
         }
      }

      if (p->count == p->max) {
         p->max = MAX(PENDING_MIN, p->max * 2);
         p = xrealloc_flex(p, sizeof(rt_pending_t), p->max,
                           sizeof(rt_wakeable_t *));
         *pending = tag_pointer(p, 0);
      }

      p->wake[p->count++] = obj;
   }
}

static void clear_event(rt_model_t *m, void **pending, rt_wakeable_t *obj)
{
   if (pointer_tag(*pending) == 1) {
      rt_wakeable_t *wake = untag_pointer(*pending, rt_wakeable_t);
      if (wake == obj)
         *pending = NULL;
   }
   else if (*pending != NULL) {
      rt_pending_t *p = untag_pointer(*pending, rt_pending_t);
      for (int i = 0; i < p->count; i++) {
         if (p->wake[i] == obj) {
            p->wake[i] = NULL;
            return;
         }
      }
   }
}

static rt_source_t *find_driver(rt_nexus_t *nexus, rt_proc_t *proc)
{
   // Try to find this process in the list of existing drivers
   for (rt_source_t *d = &(nexus->sources); d; d = d->chain_input) {
      if (d->tag == SOURCE_DRIVER && d->u.driver.proc == proc)
         return d;
   }

   return NULL;
}

static inline bool insert_transaction(rt_model_t *m, rt_nexus_t *nexus,
                                      rt_source_t *source, waveform_t *w,
                                      uint64_t when, uint64_t reject)
{
   waveform_t *last = &(source->u.driver.waveforms);
   waveform_t *it   = last->next;
   while (it != NULL && it->when < when) {
      // If the current transaction is within the pulse rejection interval
      // and the value is different to that of the new transaction then
      // delete the current transaction
      assert(it->when >= m->now);
      if (it->when >= when - reject
          && !cmp_values(nexus, it->value, w->value)) {
         waveform_t *next = it->next;
         last->next = next;
         free_value(nexus, it->value);
         free_waveform(m, it);
         it = next;
      }
      else {
         last = it;
         it = it->next;
      }
   }
   last->next = w;

   // Delete all transactions later than this
   // We could remove this transaction from the deltaq as well but the
   // overhead of doing so is probably higher than the cost of waking
   // up for the empty event
   bool already_scheduled = false;
   for (waveform_t *next; it != NULL; it = next) {
      next = it->next;
      already_scheduled |= (it->when == when);
      free_value(nexus, it->value);
      free_waveform(m, it);
   }

   return already_scheduled;
}

static void sched_driver(rt_model_t *m, rt_nexus_t *n, uint64_t after,
                         uint64_t reject, const void *value, rt_proc_t *proc)
{
   if (after == 0 && (n->flags & NET_F_FAST_DRIVER)) {
      rt_source_t *d = &(n->sources);
      assert(n->n_sources == 1);

      waveform_t *w = &d->u.driver.waveforms;
      w->when = m->now;
      assert(w->next == NULL);

      rt_signal_t *signal = n->signal;
      rt_source_t *d0 = &(signal->nexus.sources);

      if (d->fastqueued)
         assert(m->next_is_delta);
      else if ((signal->shared.flags & NET_F_FAST_DRIVER) && d0->sigqueued) {
         assert(m->next_is_delta);
         d->fastqueued = 1;
      }
      else if (cmp_bytes(value, value_ptr(n, &w->value), n->width * n->size)) {
         m->next_is_delta = true;
         d->was_active = (n->active_delta == m->iteration);
         n->active_delta = m->iteration + 1;
         return;
      }
      else if (signal->shared.flags & NET_F_FAST_DRIVER) {
         deferq_do(&m->driverq, async_fast_all_drivers, signal);
         m->next_is_delta = true;
         d0->sigqueued = 1;
         d->fastqueued = 1;
      }
      else {
         deferq_do(&m->driverq, async_fast_driver, d);
         m->next_is_delta = true;
         d->fastqueued = 1;
      }

      copy_value_ptr(n, &w->value, value);
   }
   else {
      rt_source_t *d = find_driver(n, proc);
      assert(d != NULL);

      if ((n->flags & NET_F_FAST_DRIVER) && d->fastqueued) {
         // A fast update to this driver is already scheduled
         waveform_t *w0 = alloc_waveform(m);
         w0->when  = m->now;
         w0->next  = NULL;
         w0->value = alloc_value(m, n);

         const uint8_t *prev = value_ptr(n, &(d->u.driver.waveforms.value));
         copy_value_ptr(n, &w0->value, prev);

         assert(d->u.driver.waveforms.next == NULL);
         d->u.driver.waveforms.next = w0;
      }

      n->flags &= ~NET_F_FAST_DRIVER;

      waveform_t *w = alloc_waveform(m);
      w->when  = m->now + after;
      w->next  = NULL;
      w->value = alloc_value(m, n);

      copy_value_ptr(n, &w->value, value);

      if (!insert_transaction(m, n, d, w, w->when, reject))
         deltaq_insert_driver(m, after, d);
   }
}

static void sched_disconnect(rt_model_t *m, rt_nexus_t *nexus, uint64_t after,
                             uint64_t reject, rt_proc_t *proc)
{
   rt_source_t *d = find_driver(nexus, proc);
   assert(d != NULL);

   const uint64_t when = m->now + after;

   // Need update_driver to clear disconnected flag
   nexus->flags &= ~NET_F_FAST_DRIVER;

   waveform_t *w = alloc_waveform(m);
   w->when = -when;   // Use sign bit to represent null
   w->next = NULL;
   w->value.qword = 0;

   if (!insert_transaction(m, nexus, d, w, when, reject))
      deltaq_insert_driver(m, after, d);
}

static void async_watch_callback(rt_model_t *m, void *arg)
{
   rt_watch_t *w = arg;

   assert(w->wakeable.pending);
   w->wakeable.pending = false;

   if (w->wakeable.zombie)
      free(w);
   else
      (*w->fn)(m->now, w->signals[0], w, w->user_data);
}

static void async_timeout_callback(rt_model_t *m, void *arg)
{
   rt_callback_t *cb = arg;
   (*cb->fn)(m, cb->user);
   free(cb);
}

static void async_update_implicit_signal(rt_model_t *m, void *arg)
{
   rt_implicit_t *imp = arg;

   assert(imp->wakeable.pending);
   imp->wakeable.pending = false;

   update_implicit_signal(m, imp);
}

static void async_run_process(rt_model_t *m, void *arg)
{
   rt_proc_t *proc = arg;

   assert(proc->wakeable.pending);
   proc->wakeable.pending = false;

   run_process(m, proc);
}

static void async_update_property(rt_model_t *m, void *arg)
{
   rt_prop_t *prop = arg;

   assert(prop->wakeable.pending);
   prop->wakeable.pending = false;

   update_property(m, prop);
}

static bool heap_delete_proc_cb(uint64_t key, void *value, void *search)
{
   if (pointer_tag(value) != EVENT_PROCESS)
      return false;

   return untag_pointer(value, rt_proc_t) == search;
}

static bool run_trigger(rt_model_t *m, rt_trigger_t *t)
{
   if (t->epoch == m->trigger_epoch)
      return t->result.integer != 0;   // Cached

   switch (t->kind) {
   case FUNC_TRIGGER:
      {
         tlab_t tlab = jit_null_tlab(m->jit);
         if (!jit_vfastcall(m->jit, t->handle, t->args, t->nargs,
                            &t->result, 1, &tlab))
            m->force_stop = true;

         TRACE("run trigger %p %pi ==> %"PRIi64, t,
               jit_get_name(m->jit, t->handle), t->result.integer);
      }
      break;

   case OR_TRIGGER:
      {
         rt_trigger_t *left = t->args[0].pointer;
         rt_trigger_t *right = t->args[1].pointer;
         t->result.integer = run_trigger(m, left) || run_trigger(m, right);

         TRACE("or trigger %p ==> %"PRIi64, t, t->result.integer);
      }
      break;

   case CMP_TRIGGER:
      {
         rt_signal_t *s = t->args[0].pointer;
         uint32_t offset = t->args[1].integer;
         int64_t right = t->args[2].integer;

#define COMPARE_SCALAR(type) do {                                       \
            const type *data = (type *)s->shared.data;                  \
            t->result.integer = (data[offset] == right);                \
      } while (0)

         FOR_ALL_SIZES(s->nexus.size, COMPARE_SCALAR);

         TRACE("cmp trigger %p ==> %"PRIi64, t, t->result.integer);
      }
      break;

   case LEVEL_TRIGGER:
      {
         rt_signal_t *s = t->args[0].pointer;
         uint32_t offset = t->args[1].integer;
         int32_t count = t->args[2].integer;

         t->result.integer = 0;

         rt_nexus_t *n = split_nexus(m, s, offset, count);
         for (; count > 0; n = n->chain) {
            if (n->last_event == m->now && n->event_delta == m->iteration) {
               t->result.integer = 1;
               break;
            }

            count -= n->width;
            assert(count >= 0);
         }

         TRACE("level trigger %pi+%d ==> %"PRIi64, tree_ident(s->where),
               offset, t->result.integer);
      }
      break;
   }

   t->epoch = m->trigger_epoch;

   return t->result.integer != 0;
}

static void update_assignment(rt_model_t *m, rt_proc_t *proc)
{
   // This is a special case of run_process that handles continuous
   // assignment updates as a result of procedural blocking assignments
   assert(proc->wakeable.kind == W_ASSIGN);

   model_thread_t *thread = model_thread(m);
   assert(thread->tlab != NULL);

   rt_wakeable_t *const old_obj = thread->active_obj;
   rt_scope_t *const old_scope = thread->active_scope;

   thread->active_obj = &(proc->wakeable);
   thread->active_scope = proc->scope;

   // Stateless processes have NULL privdata so pass a dummy pointer
   // value in so it can be distinguished from a reset
   jit_scalar_t state = {
      .pointer = *mptr_get(proc->privdata) ?: (void *)-1
   };

   jit_scalar_t result;
   jit_scalar_t context = {
      .pointer = *mptr_get(proc->scope->privdata)
   };

   assert(proc->tlab == NULL);

   const uint32_t mark = tlab_mark(thread->tlab);

   if (!jit_fastcall(m->jit, proc->handle, &result, state, context,
                     thread->tlab))
      m->force_stop = true;

   assert(result.pointer == NULL);

   tlab_trim(thread->tlab, mark);

   thread->active_obj = old_obj;
   thread->active_scope = old_scope;
}

static void procq_do(rt_model_t *m, rt_wakeable_t *obj, defer_fn_t fn,
                     void *arg)
{
   if (obj->postponed)
      deferq_do(&m->postponedq, fn, arg);
   else {
      deferq_do(&m->procq, fn, arg);
      m->next_is_delta |= m->blocking_update;
   }

   set_pending(obj);
}

static void wakeup_one(rt_model_t *m, rt_wakeable_t *obj)
{
   if (obj->fastclk && m->fastclk_on) {
      // Clk-only process: never queued. Latch that the clock fanout fired this
      // delta; the posedge table runs it directly at the proc-dispatch site.
      m->fastclk_hit = true;
      return;
   }

   if (obj->pending)
      return;   // Already scheduled

   switch (obj->kind) {
   case W_PROC:
      {
         rt_proc_t *proc = container_of(obj, rt_proc_t, wakeable);
         TRACE("wakeup %sprocess %s", obj->postponed ? "postponed " : "",
               istr(proc->name));

         if (proc->wakeable.delayed) {
            // This process was already scheduled to run at a later
            // time so we need to delete it from the simulation queue
            heap_delete(m->eventq_heap, heap_delete_proc_cb, proc);
            proc->wakeable.delayed = false;
         }

         procq_do(m, obj, async_run_process, proc);
      }
      break;

   case W_PROPERTY:
      {
         rt_prop_t *prop = container_of(obj, rt_prop_t, wakeable);
         TRACE("wakeup property %s", istr(prop->name));
         procq_do(m, obj, async_update_property, prop);
      }
      break;

   case W_IMPLICIT:
      {
         rt_implicit_t *imp = container_of(obj, rt_implicit_t, wakeable);
         TRACE("wakeup implicit signal %s closure %s",
               istr(tree_ident(imp->signal.where)),
               istr(jit_get_name(m->jit, imp->closure.handle)));

         deferq_do(&m->implicitq, async_update_implicit_signal, imp);
         set_pending(obj);
      }
      break;

   case W_WATCH:
      {
         rt_watch_t *w = container_of(obj, rt_watch_t, wakeable);
         TRACE("wakeup %svalue change callback %p %s",
               obj->postponed ? "postponed " : "", w, debug_symbol_name(w->fn));

         assert(!w->wakeable.zombie);
         procq_do(m, obj, async_watch_callback, w);
      }
      break;

   case W_TRANSFER:
      {
         rt_transfer_t *t = container_of(obj, rt_transfer_t, wakeable);
         TRACE("wakeup signal transfer for %s",
               istr(tree_ident(t->target->signal->where)));

         procq_do(m, obj, async_transfer_signal, t);
      }
      break;

   case W_TRIGGER:
      {
         rt_trigger_t *t = container_of(obj, rt_trigger_t, wakeable);
         TRACE("wakeup trigger %p", t);

         if (!m->blocking_update) {
            deferq_do(&m->triggerq, async_run_trigger, t);
            set_pending(obj);
         }
         else if (run_trigger(m, t))
            wakeup_all(m, &(t->pending));
      }
      break;

   case W_ASSIGN:
      {
         rt_proc_t *proc = container_of(obj, rt_proc_t, wakeable);
         TRACE("wakeup continuous assignment %s", istr(proc->name));

         assert(!proc->wakeable.delayed);

         if (m->blocking_update)
            update_assignment(m, proc);
         else {
            deferq_do(&m->implicitq, async_run_process, proc);
            set_pending(obj);
         }
      }
      break;
   }
}

static void wakeup_all(rt_model_t *m, void **pending)
{
   if (pointer_tag(*pending) == 1) {
      rt_wakeable_t *wake = untag_pointer(*pending, rt_wakeable_t);
      wakeup_one(m, wake);
   }
   else if (*pending != NULL) {
      rt_pending_t *p = untag_pointer(*pending, rt_pending_t);
      for (int i = 0; i < p->count; i++) {
         if (p->wake[i] != NULL)
            wakeup_one(m, p->wake[i]);
      }
   }
}

static void notify_event_default(rt_model_t *m, rt_nexus_t *n)
{
   wakeup_all(m, &(n->pending));
}

static void notify_event(rt_model_t *m, rt_nexus_t *n)
{
   n->last_event = m->now;
   n->event_delta = m->iteration;

   if (n->flags & NET_F_CACHE_EVENT)
      n->signal->shared.flags |= SIG_F_EVENT_FLAG;

   n->vtable->notify(m, n);
}

static void put_effective_impl(rt_model_t *m, rt_nexus_t *n, const void *value)
{
   TRACE("update %s effective value %s", trace_nexus(n), fmt_nexus(n, value));

   unsigned char *eff = nexus_effective(n);
   unsigned char *last = nexus_last_value(n);

   const size_t valuesz = n->size * n->width;

   if (!cmp_bytes(eff, value, valuesz)) {
      copy2(last, eff, value, valuesz);
      notify_event(m, n);
   }
}

static void enqueue_effective(rt_model_t *m, rt_nexus_t *n)
{
   if (n->flags & NET_F_PENDING)
      return;

   n->flags |= NET_F_PENDING;
   heap_insert(m->effective_heap, MAX_RANK - n->rank, n);
}

static void update_effective(rt_model_t *m, rt_nexus_t *n)
{
   n->active_delta = m->iteration;
   n->flags &= ~NET_F_PENDING;

   calculate_effective_value(m, n);

   if (n->n_sources > 0) {
      for (rt_source_t *s = &(n->sources); s; s = s->chain_input) {
         if (s->tag != SOURCE_PORT)
            continue;
         else if (s->u.port.conv_func != NULL) {
            rt_conv_func_t *cf = s->u.port.conv_func;
            for (int i = 0; i < cf->ninputs; i++) {
               if (cf->inputs[i].nexus->flags & NET_F_INOUT)
                  enqueue_effective(m, cf->inputs[i].nexus);
            }
         }
         else if (s->u.port.input->flags & NET_F_INOUT)
            enqueue_effective(m, s->u.port.input);
      }
   }
}

static void put_driving(rt_model_t *m, rt_nexus_t *n, const void *value)
{
   if (n->flags & NET_F_EFFECTIVE) {
      TRACE("update %s driving value %s", trace_nexus(n), fmt_nexus(n, value));

      memcpy(nexus_driving(n), value, n->size * n->width);

      assert(!(n->flags & NET_F_PENDING));
      n->flags |= NET_F_PENDING;
      n->flags &= ~NET_F_HAS_INITIAL;
      heap_insert(m->effective_heap, MAX_RANK - n->rank, n);
   }
   else
      put_effective(m, n, value);
}

// Fast path: single SOURCE_DRIVER, no resolution function.
// Reads driver value directly and deposits it, skipping the full
// calculate_driving_value → call_resolution → source_value chain.
// Installed via vtable on nexuses that qualify after model_reset.
static void calculate_driving_single(rt_model_t *m, rt_nexus_t *n)
{
   rt_source_t *s = &(n->sources);
   assert(s->tag == SOURCE_DRIVER);
   assert(!s->disconnected);
   put_driving(m, n, value_ptr(n, &(s->u.driver.waveforms.value)));
}

// Fast path: single SOURCE_DRIVER with memoised resolution.
// Does table lookup on driver value without walking sources.
static void calculate_driving_memo1(rt_model_t *m, rt_nexus_t *n)
{
   rt_source_t *s = &(n->sources);
   assert(s->tag == SOURCE_DRIVER);
   assert(!s->disconnected);

   res_memo_t *r = n->signal->resolution;
   char *p0 = (char *)value_ptr(n, &(s->u.driver.waveforms.value));

   // Single-driver memoised: tab1 lookup per bit
   const size_t valuesz = n->width * n->size;
   uint8_t resolved[valuesz <= 64 ? 64 : valuesz];
   for (int j = 0; j < n->width; j++)
      resolved[j] = r->tab1[(int)(uint8_t)p0[j]];

   put_driving(m, n, resolved);
}

// Vtable for single-driver nexuses (no resolution needed)
static const rt_nexus_vtable_t nexus_single_driver_vtable = {
   .update_driving = calculate_driving_single,
   .deposit        = put_effective_impl,
   .read_source    = source_value,
   .notify         = notify_event_default,
};

// Vtable for single-driver with memoised resolution
static const rt_nexus_vtable_t nexus_memo1_vtable = {
   .update_driving = calculate_driving_memo1,
   .deposit        = put_effective_impl,
   .read_source    = source_value,
   .notify         = notify_event_default,
};

static void defer_driving_update(rt_model_t *m, rt_nexus_t *n)
{
   if (n->flags & NET_F_PENDING)
      return;

   TRACE("defer %s driving value update", trace_nexus(n));
   heap_insert(m->driving_heap, n->rank, n);
   n->flags |= NET_F_PENDING;
}

static void update_driving(rt_model_t *m, rt_nexus_t *n, bool safe)
{
   if (n->n_sources == 1 || safe) {
      n->active_delta = m->iteration;
      n->flags &= ~NET_F_PENDING;

      n->vtable->update_driving(m, n);

      // Update outputs if the effective value must be calculated
      // separately or there was an event on this signal
      const bool update_outputs = !!(n->flags & NET_F_EFFECTIVE)
         || (n->event_delta == m->iteration && n->last_event == m->now);

      if (update_outputs) {
         for (rt_source_t *o = n->outputs; o; o = o->chain_output) {
            switch (o->tag) {
            case SOURCE_PORT:
               if (o->u.port.conv_func != NULL)
                  defer_driving_update(m, o->u.port.output);
               else
                  update_driving(m, o->u.port.output, false);
               break;
            case SOURCE_IMPLICIT:
               update_driving(m, o->u.pseudo.nexus , false);
               break;
            default:
               should_not_reach_here();
            }
         }
      }
   }
   else
      defer_driving_update(m, n);
}

static void update_driver(rt_model_t *m, rt_nexus_t *n, rt_source_t *source)
{
   waveform_t *w_now  = &(source->u.driver.waveforms);
   waveform_t *w_next = w_now->next;

   if (likely(w_next != NULL && w_next->when == m->now)) {
      free_value(n, w_now->value);
      *w_now = *w_next;
      free_waveform(m, w_next);
      source->disconnected = 0;
      update_driving(m, n, false);
   }
   else if (unlikely(w_next != NULL && w_next->when == -m->now)) {
      // Disconnect source due to null transaction.  Revert any fast-path
      // vtable (single-driver / memo1) so the next driving-value update
      // runs the full resolution path, which for bus signals must call
      // the resolution function with an empty input to yield the default
      // (e.g. 'Z' for std_logic) rather than the driver's stale value.
      *w_now = *w_next;
      free_waveform(m, w_next);
      source->disconnected = 1;
      n->vtable = &nexus_default_vtable;
      update_driving(m, n, false);
   }
}

static void fast_update_driver(rt_model_t *m, rt_nexus_t *nexus)
{
   rt_source_t *src = &(nexus->sources);

   if (likely(nexus->flags & NET_F_FAST_DRIVER)) {
      // Preconditions for fast driver updates
      assert(nexus->n_sources == 1);
      assert(src->tag == SOURCE_DRIVER);
      assert(src->u.driver.waveforms.next == NULL);

      update_driving(m, nexus, false);
   }
   else
      update_driver(m, nexus, src);

   assert(src->fastqueued);
   src->fastqueued = 0;
}

static void fast_update_all_drivers(rt_model_t *m, rt_signal_t *signal)
{
   assert(signal->shared.flags & NET_F_FAST_DRIVER);

   rt_nexus_t *n = &(signal->nexus);
   assert(n->sources.sigqueued);
   n->sources.sigqueued = 0;

   int count = 0;
   for (int i = 0; i < signal->n_nexus; i++, n = n->chain) {
      if (n->sources.fastqueued) {
         fast_update_driver(m, n);
         count++;
      }
   }

   if (count < signal->n_nexus >> 1) {
      // Unlikely to be worth the iteration cost
      signal->shared.flags &= ~NET_F_FAST_DRIVER;
   }
}

static void async_update_driver(rt_model_t *m, void *arg)
{
   rt_source_t *src = arg;
   update_driver(m, src->u.driver.nexus, src);
}

static void async_fast_driver(rt_model_t *m, void *arg)
{
   rt_source_t *src = arg;
   fast_update_driver(m, src->u.driver.nexus);
}

static void async_fast_all_drivers(rt_model_t *m, void *arg)
{
   rt_signal_t *signal = arg;
   fast_update_all_drivers(m, signal);
}

static void async_pseudo_source(rt_model_t *m, void *arg)
{
   rt_source_t *src = arg;
   assert(src->tag == SOURCE_FORCING || src->tag == SOURCE_DEPOSIT);

   update_driving(m, src->u.pseudo.nexus, false);

   assert(src->pseudoqueued);
   src->pseudoqueued = 0;
}

static void async_transfer_signal(rt_model_t *m, void *arg)
{
   rt_transfer_t *t = arg;

   assert(t->wakeable.pending);
   t->wakeable.pending = false;

   rt_nexus_t *n = t->target;
   char *vptr = nexus_effective(t->source);
   for (int count = t->count; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      sched_driver(m, n, t->after, t->reject, vptr, t->proc);
      vptr += n->width * n->size;
   }
}

static void async_run_trigger(rt_model_t *m, void *arg)
{
   rt_trigger_t *t = arg;

   assert(t->wakeable.pending);
   t->wakeable.pending = false;

   if (run_trigger(m, t))
      wakeup_all(m, &(t->pending));
}

static void update_implicit_signal(rt_model_t *m, rt_implicit_t *imp)
{
   model_thread_t *thread = model_thread(m);
   assert(thread->active_obj == NULL);
   thread->active_obj = &(imp->wakeable);

   jit_scalar_t result;
   if (!jit_try_call(m->jit, imp->closure.handle, &result,
                     imp->closure.context, imp->signal.shared.data[0]))
      m->force_stop = true;

   thread->active_obj = NULL;

   TRACE("implicit signal %s new value %"PRIi64,
         istr(tree_ident(imp->signal.where)), result.integer);

   assert(imp->signal.n_nexus == 1);
   rt_nexus_t *n0 = &(imp->signal.nexus);

   n0->active_delta = m->iteration;

   if (n0->n_sources > 0 && n0->sources.tag == SOURCE_DRIVER) {
      if (!result.integer) {
         // Update driver for 'STABLE and 'QUIET
         // TODO: this should happen inside the callback
         waveform_t *w = alloc_waveform(m);
         w->when  = m->now + imp->delay;
         w->next  = NULL;
         w->value = alloc_value(m, n0);

         w->value.bytes[0] = 1;   // Boolean TRUE

         if (!insert_transaction(m, n0, &(n0->sources), w, w->when, imp->delay))
            deltaq_insert_driver(m, imp->delay, &(n0->sources));

         put_effective(m, n0, &result.integer);
      }
      else if (n0->sources.u.driver.waveforms.next == NULL)
         put_effective(m, n0, &result.integer);
   }
   else
      put_effective(m, n0, &result.integer);
}

static void iteration_limit_proc_cb(void *fn, void *arg, void *extra)
{
   diag_t *d = extra;
   rt_proc_t *proc = NULL;

   if (fn == async_run_process)
      proc = arg;
   else if (fn == async_transfer_signal) {
      rt_transfer_t *t = arg;
      proc = t->proc;
   }

   if (proc == NULL)
      return;

   const loc_t *loc = tree_loc(proc->where);
   diag_hint(d, loc, "process %s is active", istr(proc->name));
}

static void iteration_limit_driver_cb(void *fn, void *arg, void *extra)
{
   diag_t *d = extra;
   tree_t decl = NULL;

   if (fn == async_update_driver || fn == async_fast_driver) {
      rt_source_t *src = arg;
      if (src->tag == SOURCE_DRIVER)
         decl = src->u.driver.nexus->signal->where;
   }
   else if (fn == async_fast_all_drivers) {
      rt_signal_t *s = arg;
      decl = s->where;
   }

   if (decl == NULL)
      return;

   diag_hint(d, tree_loc(decl), "driver for %s %s is active",
             tree_kind(decl) == T_PORT_DECL ? "port" : "signal",
             istr(tree_ident(decl)));
}

static void reached_iteration_limit(rt_model_t *m)
{
   diag_t *d = diag_new(DIAG_FATAL, NULL);

   diag_printf(d, "limit of %d delta cycles reached", m->stop_delta);

   deferq_scan(&m->procq, iteration_limit_proc_cb, d);
   deferq_scan(&m->driverq, iteration_limit_driver_cb, d);

   diag_hint(d, NULL, "you can increase this limit with $bold$--stop-delta$$");
   diag_emit(d);

   m->force_stop = true;
}

static void sync_event_cache(rt_model_t *m)
{
   for (int i = 0; i < m->eventsigs.count; i++) {
      rt_signal_t *s = m->eventsigs.items[i];
      assert(s->shared.flags & SIG_F_CACHE_EVENT);

      const bool event = s->nexus.last_event == m->now
         && s->nexus.event_delta == m->iteration;

      TRACE("sync event flag %d for %s", event, istr(tree_ident(s->where)));

      if (event)
         assert(s->shared.flags & SIG_F_EVENT_FLAG);   // Set by notify_event
      else
         s->shared.flags &= ~SIG_F_EVENT_FLAG;
   }
}

static void swap_deferq(deferq_t *a, deferq_t *b)
{
   deferq_t tmp = *a;
   *a = *b;
   *b = tmp;
}

// ---------------------------------------------------------------------------
// Stage-1c parallel process dispatch — eval on workers, propagate on thread 0.
//
// At a wide delta, thread 0 partitions the woken-process table into per-worker
// slices and the hot-spinning workers EVALUATE their processes (reads of
// settled values are lock-free). A process that drives a signal does NOT touch
// shared signal/nexus state on the worker: x_sched_waveform pushes a
// propagation record into the worker's SPSC pipe and returns, so eval never
// blocks. Thread 0 is the sole propagator — it drains every pipe concurrently
// and replays each driver update via sched_driver, so all nexus splitting and
// driver-queue mutation happen single-threaded. Within a delta driver updates
// are order-independent (resolution is at delta end), so drain order doesn't
// affect the result. Enabled by NVC_PARALLEL_PROCS=<nthreads>; default off.
// ---------------------------------------------------------------------------
typedef struct {
   uint64_t      epoch __attribute__((aligned(64)));   // inbound (dispatcher)
   defer_task_t *tasks;
   unsigned      start;
   unsigned      end;
   int           tid;     // worker's nvc thread_id (NOT the array index)
   int           ready;   // worker has set up its pipe/tlab
} evproc_mb_t;

#define EVPROC_STOP  UINT_MAX     // slice.end sentinel: worker should exit
#define EVPROC_MIN   64           // don't parallelise tiny proc queues
#define PROP_VALSZ   256          // inline driver value bytes; wider -> heap

// One deferred driver write (x_sched_waveform), captured on a worker and
// replayed by thread 0.
typedef struct {
   sig_shared_t *ss;
   uint32_t      offset;
   int32_t       count;        // nexus element count (1 for the scalar form)
   int64_t       after;
   int64_t       reject;
   rt_proc_t    *proc;
   bool          scalar;       // true: value in `sval`; false: inline/heap bytes
   uint64_t      sval;
   uint32_t      nbytes;
   uint8_t      *heapval;      // non-NULL when value is too wide for `value`
   uint8_t       value[PROP_VALSZ];
} prop_rec_t;

// Per-worker propagation buffer: the worker appends during eval (never
// blocks — grows on demand), thread 0 drains it after the eval barrier. No
// atomics on count/recs: the remaining-countdown is the happens-before edge
// between a worker's appends and thread 0's drain.
typedef struct {
   prop_rec_t *recs;
   uint32_t    count;
   uint32_t    max;
} prop_pipe_t;

static struct {
   bool          started;
   int           nthreads;        // total participating incl thread 0
   rt_model_t   *model;
   uint64_t      epoch;
   int           remaining __attribute__((aligned(64)));  // workers still in eval
   nvc_thread_t *threads[MAX_THREADS];
   prop_pipe_t  *pipes[MAX_THREADS];
   evproc_mb_t   mb[MAX_THREADS];
} g_evproc;

// Runtime parallel-delta gate (EVPROC_MIN default; raised via NVC_PARALLEL_MIN).
static unsigned g_evproc_min = EVPROC_MIN;

// Worker push (single producer): append, growing on demand. Never blocks.
static inline prop_rec_t *prop_reserve(int tid)
{
   prop_pipe_t *p = g_evproc.pipes[tid];
   if (unlikely(p->count == p->max)) {
      p->max = p->max ? p->max * 2 : 1024;
      p->recs = xrealloc_array(p->recs, p->max, sizeof(prop_rec_t));
   }
   return &p->recs[p->count];
}

static inline void prop_commit(int tid)
{
   g_evproc.pipes[tid]->count++;
}

// Thread 0: apply one record — the sole site where split_nexus/sched_driver run.
static void prop_apply(rt_model_t *m, prop_rec_t *r)
{
   rt_signal_t *s = container_of(r->ss, rt_signal_t, shared);
   RT_LOCK(s->lock);   // clone_nexus asserts this; uncontended (sole propagator)
   if (r->scalar) {
      rt_nexus_t *n = split_nexus(m, s, r->offset, 1);
      sched_driver(m, n, r->after, r->reject, &r->sval, r->proc);
   }
   else {
      rt_nexus_t *n = split_nexus(m, s, r->offset, r->count);
      uint8_t *vptr = r->heapval ? r->heapval : r->value;
      int count = r->count;
      for (; count > 0; n = n->chain) {
         count -= n->width;
         sched_driver(m, n, r->after, r->reject, vptr, r->proc);
         vptr += n->width * n->size;
      }
      if (r->heapval) free(r->heapval);
   }
}

// Thread 0, after the eval barrier: apply all of a worker's records, reset.
static void prop_drain(rt_model_t *m, prop_pipe_t *p)
{
   for (uint32_t i = 0; i < p->count; i++)
      prop_apply(m, &p->recs[i]);
   p->count = 0;
}

static void evproc_run_slice(rt_model_t *m, defer_task_t *t,
                             unsigned s, unsigned e)
{
   for (unsigned i = s; i < e; i++)
      (*t[i].fn)(m, t[i].arg);
}

static void *evproc_worker(void *arg)
{
   evproc_mb_t *mb = arg;
   rt_model_t *m = g_evproc.model;

   // Per-thread runtime state the process bodies need on this worker. NB the
   // nvc thread_id is assigned by the pool and is NOT the mailbox index (the
   // JIT/GC pool may already hold low ids), so the pipe is keyed by thread_id
   // and the id is recorded for thread 0 to find.
   //   __model      — get_model()/get_active_wakeable() read it (__thread in MT)
   //   thread->tlab — proc_eval_jit's transient allocation buffer
   const int tid = thread_id();
   __model = m;
   model_thread(m)->tlab = tlab_acquire(m->mspace);
   g_evproc.pipes[tid] = xcalloc(sizeof(prop_pipe_t));
   mb->tid = tid;

   // Each worker gets its own eval arena so escaping results never touch the
   // shared collected heap (no GC stop-the-world to race the eval).
   if (getenv("NVC_EVAL_ARENA") != NULL)
      jit_eval_arena_enable(true);

   atomic_store(&mb->ready, 1);

   uint64_t seen = 0;
   for (;;) {
      uint64_t e;
      while ((e = atomic_load(&mb->epoch)) == seen)
         spin_wait();
      seen = e;
      if (mb->end == EVPROC_STOP)
         break;
      evproc_run_slice(m, mb->tasks, mb->start, mb->end);
      atomic_add(&g_evproc.remaining, -1);   // done-handler: count this worker out
   }
   return NULL;
}

static void evproc_ensure_started(rt_model_t *m)
{
   if (likely(g_evproc.started))
      return;
   g_evproc.started = true;

   const char *env = getenv("NVC_PARALLEL_PROCS");
   int nt = env ? atoi(env) : 0;
   if (nt < 2) { g_evproc.nthreads = 1; return; }   // disabled
   if (nt > MAX_THREADS) nt = MAX_THREADS;

   // Per-delta parallelism only pays off when a delta has enough INDEPENDENT
   // work to amortize the dispatch barrier. dq->count over-counts on rerouted
   // accel chunks (hundreds of duplicate procs collapse to one eval), so tiny
   // or duplicate-heavy deltas lose. NVC_PARALLEL_MIN raises the gate above the
   // EVPROC_MIN default for such workloads (set very high to keep it serial).
   const char *minenv = getenv("NVC_PARALLEL_MIN");
   if (minenv) {
      int mv = atoi(minenv);
      if (mv > 0) g_evproc_min = (unsigned)mv;
   }
   notef("NVC_PARALLEL_PROCS=%d, parallel-delta gate=%u procs", nt, g_evproc_min);

   g_evproc.nthreads = nt;
   g_evproc.model    = m;
   for (int t = 1; t < nt; t++) {
      g_evproc.mb[t].epoch = 0;
      g_evproc.mb[t].ready = 0;
      g_evproc.threads[t] =
         thread_create(evproc_worker, &g_evproc.mb[t], "evproc%d", t);
   }
   // Wait until every worker has allocated its pipe + recorded its tid, so the
   // first dispatch can drain them safely.
   for (int t = 1; t < nt; t++)
      while (!atomic_load(&g_evproc.mb[t].ready))
         spin_wait();
   notef("NVC_PARALLEL_PROCS: %d-way (eval on %d workers, propagate on thread 0)",
         nt, nt - 1);
}

static void evproc_dispatch(rt_model_t *m, deferq_t *dq)
{
   const unsigned n  = dq->count;
   const int      nw = g_evproc.nthreads - 1;   // workers; thread 0 propagates
   const unsigned per = (n + nw - 1) / nw;

   const uint64_t e = ++g_evproc.epoch;
   atomic_store(&g_evproc.remaining, nw);
   atomic_store(&g_par_active, 1);

   for (int t = 1; t < g_evproc.nthreads; t++) {
      evproc_mb_t *mb = &g_evproc.mb[t];
      const unsigned s = MIN(per * (unsigned)(t - 1), n);
      mb->tasks = dq->tasks;
      mb->start = s;
      mb->end   = MIN(s + per, n);
      atomic_store(&mb->epoch, e);          // publish epoch last
   }

   // Barrier: wait until every worker has finished eval. "All evaluation
   // completes before any update" — so we do NOT propagate during eval (that
   // would write signal state under concurrent reads); drain afterwards.
   while (atomic_load(&g_evproc.remaining) != 0)
      spin_wait();

   // Propagate phase: thread 0 is now the only runner, so clear g_par_active
   // (no scheduling locks needed) and apply every deferred driver update —
   // the sole site where split_nexus / sched_driver run.
   atomic_store(&g_par_active, 0);
   for (int t = 1; t < g_evproc.nthreads; t++)
      prop_drain(m, g_evproc.pipes[g_evproc.mb[t].tid]);

   dq->count = 0;
}

static void evproc_shutdown(void)
{
   if (!g_evproc.started || g_evproc.nthreads <= 1)
      return;
   const uint64_t e = ++g_evproc.epoch;
   for (int t = 1; t < g_evproc.nthreads; t++) {
      g_evproc.mb[t].end = EVPROC_STOP;
      atomic_store(&g_evproc.mb[t].epoch, e);
   }
   for (int t = 1; t < g_evproc.nthreads; t++)
      thread_join(g_evproc.threads[t]);
   g_evproc.started = false;
}

static inline void run_procq(rt_model_t *m, deferq_t *dq)
{
   if (g_evproc.nthreads > 1 && dq->count >= g_evproc_min)
      evproc_dispatch(m, dq);
   else
      deferq_run(m, dq);
}

static void model_cycle(rt_model_t *m)
{
   // Simulation cycle is described in LRM 93 section 12.6.4

   const bool is_delta_cycle = m->next_is_delta;
   m->next_is_delta = false;

   if (is_delta_cycle)
      m->iteration = m->iteration + 1;
   else {
      m->now = heap_min_key(m->eventq_heap);
      m->iteration = 0;
   }

   m->blocking_update = false;

   TRACE("begin cycle");

#if TRACE_DELTAQ > 0
   if (__trace_on)
      deltaq_dump(m);
#endif

   if (m->iteration == 0)
      run_callbacks(m, NEXT_TIME_STEP);

   run_callbacks(m, NEXT_CYCLE);

   if (!is_delta_cycle) {
      for (;;) {
         void *e = heap_extract_min(m->eventq_heap);
         switch (pointer_tag(e)) {
         case EVENT_PROCESS:
            {
               rt_proc_t *proc = untag_pointer(e, rt_proc_t);
               assert(proc->wakeable.delayed);
               proc->wakeable.delayed = false;
               procq_do(m, &proc->wakeable, async_run_process, proc);
            }
            break;
         case EVENT_DRIVER:
            {
               rt_source_t *source = untag_pointer(e, rt_source_t);
               deferq_do(&m->driverq, async_update_driver, source);
            }
            break;
         case EVENT_TIMEOUT:
            {
               rt_callback_t *cb = untag_pointer(e, rt_callback_t);
               deferq_do(&m->driverq, async_timeout_callback, cb);
            }
            break;
         case EVENT_PSEUDO:
            {
               rt_source_t *source = untag_pointer(e, rt_source_t);
               deferq_do(&m->driverq, async_pseudo_source, source);
            }
            break;
         }

         if (heap_size(m->eventq_heap) == 0)
            break;
         else if (heap_min_key(m->eventq_heap) > m->now)
            break;
      }
   }

   swap_deferq(&m->next_driverq, &m->driverq);
   deferq_run(m, &m->next_driverq);

   while (heap_size(m->driving_heap) > 0) {
      rt_nexus_t *n = heap_extract_min(m->driving_heap);
      update_driving(m, n, true);
   }

   while (heap_size(m->effective_heap) > 0) {
      rt_nexus_t *n = heap_extract_min(m->effective_heap);
      update_effective(m, n);
   }

   sync_event_cache(m);

   m->blocking_update = true;

   // Update implicit signals
   deferq_run(m, &m->implicitq);

   assert(model_thread(m)->tlab->alloc == 0);

#if TRACE_SIGNALS > 0
   if (__trace_on)
      dump_signals(m, m->root);
#endif

   m->trigger_epoch++;
   deferq_run(m, &m->triggerq);  // Sensitivity list filter

   run_callbacks(m, START_OF_PROCESSES);

   // NVC_FAST_CLK_AUTO: standalone posedge-table build, no accel install
   // needed. After the requested settle time, pick the widest-fanout
   // single-bit nexus as the clock (the clock pending list dwarfs all
   // others in translated RTL) and build the same table the accel install
   // would. USER DIRECTIVE: run past initialization, then block-dispatch
   // everything on the shared sensitivity.
   if (unlikely(m->fastclk_auto_at != 0 && !m->fastclk_on
                && m->now >= m->fastclk_auto_at)) {
      m->fastclk_auto_at = 0;   // one shot
      // Widest-fanout single-bit nexus is often rst (async-reset procs pend
      // on rst AND their clk, so all get filtered) — try candidates in
      // fanout order until one yields a non-empty table.
      rt_nexus_t *tried[4] = { NULL, NULL, NULL, NULL };
      for (int k = 0; k < 4 && !m->fastclk_on; k++) {
         rt_nexus_t *best = NULL;
         unsigned best_n = 15;
         for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
            if (n->width != 1 || n->signal->n_nexus != 1) continue;
            if (n == tried[0] || n == tried[1] || n == tried[2]) continue;
            const unsigned c = aj_pending_count(n->pending);
            if (c > best_n) { best_n = c; best = n; }
         }
         if (best == NULL) break;
         tried[k] = best;
         notef("accel-jit: NVC_FAST_CLK_AUTO try %s fanout %u",
               istr(tree_ident(best->signal->where)), best_n);
         aj_build_fastclk(m, best->signal, nexus_effective(best));
      }
   }

   // NVC_FAST_CLK fast path. A clk-only process woke this delta (latched in
   // wakeup_one, those procs were not queued). If clk is now high it was a
   // rising edge, so run the flat fanout table directly — at the exact point
   // run_procq would have run them (after the driving/effective heaps and the
   // trigger filter), so each proc reads the same settled values it would on
   // the normal path; only the dispatch mechanism differs. Bit-identical, but
   // skips the wakeup_all/procq_do/deferq/async_run_process round-trip.
   if (m->fastclk_hit) {
      m->fastclk_hit = false;
      if (m->fastclk_data[0] & 1) {   // rising edge (event fired + now high)
         rt_proc_t **pr = m->fastclk_table;
         for (unsigned i = 0; i < m->fastclk_count; i++, pr++)
            run_process(m, *pr);

         // NVC_ACCEL_BANK swap: the chunk STAGEd its registered outputs into
         // shadows; every clk-reader above has now read the OLD effective value
         // (the delta-delay / NBA semantics). Publish shadow->effective so the
         // new value is visible at the NEXT clock edge. No wakeup / no extra
         // delta — the gate proved all readers are in the table just dispatched.
         for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
            aj_chunk_t *c = m->aj_chunks[ci];
            if (!c->defer_pending) continue;
            for (unsigned i = 0; i < c->defer_count; i++) {
               aj_defer_out_t *d = &c->defer_outs[i];
               if (!d->dirty) continue;
               d->dirty = false;
               if (!cmp_bytes(d->eff, d->shadow, d->valuesz)) {
                  copy2(d->last, d->eff, d->shadow, d->valuesz);
                  d->nexus->event_delta = m->iteration + 1;
                  d->nexus->last_event  = m->now;
                  if (d->cache_event)
                     d->nexus->signal->shared.flags |= SIG_F_EVENT_FLAG;
                  m->trigger_epoch++;
               }
            }
            c->defer_pending = false;
         }
      }
   }

   if (m->shuffle)
      deferq_shuffle(&m->procq);

   // Run all non-postponed processes and event callbacks
   swap_deferq(&m->next_procq, &m->procq);
   evproc_ensure_started(m);
   if (unlikely(m->prof_enabled)) {
      const unsigned depth = m->next_procq.count;
      const int b = prof_bucket(depth);
      const uint64_t t0 = get_timestamp_ns();
      run_procq(m, &m->next_procq);
      const uint64_t dt = get_timestamp_ns() - t0;
      m->prof_deltas++;
      m->prof_activations += depth;
      m->prof_proc_ns += dt;
      m->prof_depth_hist[b]++;
      m->prof_depth_ns[b] += dt;
   }
   else
      run_procq(m, &m->next_procq);

   run_callbacks(m, END_OF_PROCESSES);

   // NVC_ACCEL_HANDOFF: this delta's procs have all run — publish staged packed
   // pokes (next-delta visibility, like deposit) and schedule the consumers.
   aj_apply_pokes(m);
   aj_apply_stage2(m);

   // NVC_ACCEL_VERIFY: advance each companion .so's state this delta (no compare)
   // so its multi-clock register ordering tracks the rerouted model delta-for-delta
   // (extra clocks like free_clk lag clk by a delta and must read post-clk state).
   if (g_aj_verify && g_aj_nvchunks > 0)
      aj_verify_step(m, false);

   // Verilog scheduling regions

   if (m->next_is_delta)
      goto next_delta;

   if (m->inactiveq.count > 0) {
      TRACE("begin inactive region");
      swap_deferq(&m->next_inactiveq, &m->inactiveq);
      deferq_run(m, &m->next_inactiveq);
   }

   if (m->next_is_delta)
      goto next_delta;

   if (m->nonblockq.count > 0) {
      TRACE("begin non-blocking assignment region");
      deferq_run(m, &m->nonblockq);
   }

 next_delta:
   if (!m->next_is_delta)
      run_callbacks(m, LAST_KNOWN_DELTA_CYCLE);

   if (!m->next_is_delta) {
      m->can_create_delta = false;

      // Run all postponed processes and event callbacks
      deferq_run(m, &m->postponedq);

      run_callbacks(m, END_TIME_STEP);

      // NVC_ACCEL_VERIFY: everything for this time step has settled — re-settle
      // each companion's combinational outputs (state already advanced above) and
      // compare per-net against the interpreted values (see aj_out).
      if (g_aj_verify && g_aj_nvchunks > 0)
         aj_verify_step(m, true);

      // NVC_FORK_AT: the state is settled — take the fork-and-test checkpoint.
      if (g_fork_at >= 0 && !g_fork_child && !g_forked
          && m->now >= (uint64_t)g_fork_at)
         fork_checkpoint(m);

      m->can_create_delta = true;
   }
   else if (m->stop_delta > 0 && m->iteration == m->stop_delta)
      reached_iteration_limit(m);
}

static bool should_stop_now(rt_model_t *m, uint64_t stop_time)
{
   if (m->force_stop) {
      // Make sure we print the interrupted message if this was the
      // result of an interrupt
      jit_check_interrupt(m->jit);
      return true;
   }
   else if (m->next_is_delta)
      return false;
   else if (heap_size(m->eventq_heap) == 0)
      return true;
   else
      return heap_min_key(m->eventq_heap) > stop_time;
}

static void check_liveness_properties(rt_model_t *m, rt_scope_t *s)
{
   model_thread_t *thread = model_thread(m);

   for (int i = 0; i < s->properties.count; i++) {
      rt_prop_t *p = s->properties.items[i];
      if (p->strong) {
         TRACE("property %s in strong state", istr(p->name));

         // Passing an invalid state triggers the assertion failure
         jit_scalar_t args[] = {
            { .pointer = *mptr_get(p->privdata) ?: (void *)-1 },
            { .pointer = *mptr_get(p->scope->privdata) },
            { .integer = INT_MAX },
         };
         jit_vfastcall(m->jit, p->handle, args, ARRAY_LEN(args),
                       NULL, 0, thread->tlab);
      }
   }

   for (int i = 0; i < s->children.count; i++)
      check_liveness_properties(m, s->children.items[i]);
}

void model_run(rt_model_t *m, uint64_t stop_time)
{
   MODEL_ENTRY(m);

   if (m->force_stop)
      return;   // Was error during intialisation

   if (g_fork_at == -2) {   // parse NVC_FORK_AT / NVC_FORK_TESTS once
      g_fork_at = parse_fork_time(getenv("NVC_FORK_AT"));
      const char *nt = getenv("NVC_FORK_TESTS");
      if (nt != NULL) { const int v = atoi(nt); if (v > 0) g_fork_tests = v; }
   }

   if (m->aj_chunk_count > 0)
      aj_link_handoff(m);   // wire packed chunk-to-chunk edges (NVC_ACCEL_HANDOFF)

   run_callbacks(m, START_OF_SIMULATION);

   while (!should_stop_now(m, stop_time))
      model_cycle(m);

   run_callbacks(m, END_OF_SIMULATION);

   if (m->liveness)
      check_liveness_properties(m, m->root);
}

bool model_step(rt_model_t *m)
{
   MODEL_ENTRY(m);

   if (!m->force_stop)
      model_cycle(m);

   return should_stop_now(m, TIME_HIGH);
}

void model_run_init(rt_model_t *m)
{
   MODEL_ENTRY(m);

   if (m->force_stop)
      return;

   run_callbacks(m, START_OF_SIMULATION);
}

void model_run_fini(rt_model_t *m)
{
   MODEL_ENTRY(m);

   run_callbacks(m, END_OF_SIMULATION);

   if (m->liveness)
      check_liveness_properties(m, m->root);
}

int64_t model_step_to(rt_model_t *m, uint64_t stop_time)
{
   MODEL_ENTRY(m);

   while (!should_stop_now(m, stop_time))
      model_cycle(m);

   return model_next_time(m);
}

static inline void check_postponed(int64_t after, rt_proc_t *proc)
{
   if (unlikely(proc->wakeable.postponed && after == 0))
      fatal("postponed process %s cannot cause a delta cycle",
            istr(proc->name));
}

static inline void check_reject_limit(rt_signal_t *s, uint64_t after,
                                      uint64_t reject)
{
   if (unlikely(reject > after))
      jit_msg(NULL, DIAG_FATAL, "signal %s pulse reject limit %s is greater "
              "than delay %s", istr(tree_ident(s->where)),
              trace_time(reject), trace_time(after));
}

static inline void check_delay(int64_t delay)
{
   if (unlikely(delay < 0)) {
      char buf[32];
      fmt_time_r(buf, sizeof(buf), delay, " ");
      jit_msg(NULL, DIAG_FATAL, "illegal negative delay %s", buf);
   }
}

void force_signal(rt_model_t *m, rt_signal_t *s, const void *values,
                  int offset, size_t count)
{
   RT_LOCK(s->lock);

   TRACE("force signal %s+%d to %s", istr(tree_ident(s->where)), offset,
         fmt_values(values, count));

   assert(m->can_create_delta);

   rt_nexus_t *n = split_nexus(m, s, offset, count);
   const char *vptr = values;
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      n->flags |= NET_F_FORCED;

      rt_proc_t *dep = hash_get(m->depositors, n);
      if (dep != NULL)
         deltaq_insert_proc(m, 0, dep);

      rt_source_t *src = get_pseudo_source(m, n, SOURCE_FORCING);
      copy_value_ptr(n, &(src->u.pseudo.value), vptr);
      src->disconnected = 0;

      // Verilog force takes effect IMMEDIATELY: the next read in the forcing
      // process must already see the forced value (VHDL-2008 force lands in
      // the next delta). In Verilog mode write the effective value in place,
      // deposit-style; the queued pseudo-source update then recomputes the
      // same value and wakes receivers through the normal machinery.
      if (standard() == STD_MX) {
         const size_t valuesz = n->size * n->width;
         unsigned char *eff = nexus_effective(n);
         unsigned char *last = nexus_last_value(n);
         if (!cmp_bytes(eff, vptr, valuesz)) {
            copy2(last, eff, vptr, valuesz);
            m->trigger_epoch++;
            n->last_event = m->now;
            n->event_delta = m->iteration + 1;
            if (n->flags & NET_F_CACHE_EVENT)
               n->signal->shared.flags |= SIG_F_EVENT_FLAG;
            m->next_is_delta = true;
            wakeup_all(m, &(n->pending));
         }
      }

      // A previous release may have left the nexus on a fast-path vtable
      // that ignores SOURCE_FORCING (e.g. nexus_single_driver_vtable when
      // there is a regular driver underneath).  Revert to the full driving-
      // value algorithm so this re-force is observed.
      n->vtable = &nexus_default_vtable;

      if (!src->pseudoqueued) {
         deltaq_insert_pseudo_source(m, src);
         src->pseudoqueued = 1;
      }

      vptr += n->width * n->size;
   }
}

void release_signal(rt_model_t *m, rt_signal_t *s, int offset, size_t count)
{
   RT_LOCK(s->lock);

   TRACE("release signal %s+%d", istr(tree_ident(s->where)), offset);

   assert(m->can_create_delta);

   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      n->flags &= ~NET_F_FORCED;

      rt_source_t *src = get_pseudo_source(m, n, SOURCE_FORCING);

      // Verilog reg release semantics: a variable with no real drivers
      // RETAINS the forced value after release (until the next procedural
      // assignment), where a net returns to its resolved driving value.
      // The effective value already IS the forced value, so for a driverless
      // (deposit-only) nexus just disconnect without queueing the recompute
      // that would revert it; with NET_F_FORCED now clear, later deposits
      // proceed normally.
      bool has_driver = false;
      for (rt_source_t *s0 = &(n->sources); s0; s0 = s0->chain_input) {
         if (!is_pseudo_source(s0->tag)) {
            has_driver = true;
            break;
         }
      }

      src->disconnected = 1;
      n->vtable = &nexus_default_vtable;

      // Deposit-only NET: re-run the fused cone so the wire returns to its
      // computed (driven) value per Verilog net release semantics.
      if (!has_driver) {
         rt_proc_t *dep = hash_get(m->depositors, n);
         if (dep != NULL)
            deltaq_insert_proc(m, 0, dep);
      }

      if (has_driver && !src->pseudoqueued) {
         deltaq_insert_pseudo_source(m, src);
         src->pseudoqueued = 1;
      }
   }
}

static void deposit_signal_impl(rt_model_t *m, rt_signal_t *s,
                                const void *values, int offset, size_t count,
                                bool wake_next)
{
   RT_LOCK(s->lock);

   TRACE("deposit signal %s+%d value=%s count=%zd", istr(tree_ident(s->where)),
         offset, fmt_values(values, count * s->nexus.size), count);

   assert(!get_active_proc()->wakeable.postponed);

   rt_nexus_t *n = split_nexus(m, s, offset, count);
   const char *vptr = values;
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      // Remember who deposits here: a later force/release on this nexus
      // must re-run the depositing process (fused comb cones are not
      // sensitive to their own defs, and deposits leave no driver to
      // re-assert the computed value at release). Cones only: waking an
      // arbitrary (e.g. completed initial) process would re-run its body.
      rt_proc_t *ap = get_active_proc();
      if (ap->wakeable.fused_cone)
         hash_put(m->depositors, n, ap);

      unsigned char *eff = nexus_effective(n);
      unsigned char *last = nexus_last_value(n);

      const size_t valuesz = n->size * n->width;

      // Verilog force semantics: a procedural assignment to a forced
      // variable is LOST (not queued) -- the forced value stays visible and
      // the write does not reappear at release. (IEEE 1364: the variable
      // retains the forced value until released, then keeps it until the
      // next procedural assignment.)
      if (n->flags & NET_F_FORCED) {
         vptr += valuesz;
         continue;
      }

      if (!cmp_bytes(eff, vptr, valuesz)) {
         copy2(last, eff, vptr, valuesz);
         m->trigger_epoch++;

         n->last_event = m->now;

         // A deposit applied from within a running process (wake_next) writes
         // the value immediately -- so a concurrent <= driver and same-time
         // reads are not perturbed -- but its woken receivers do not run until
         // the next delta iteration (the procq is swapped before it is
         // drained). Attribute the event to iteration+1 so S'event /
         // rising_edge read true when those receivers actually run; otherwise a
         // blocking-assigned clock toggles its value but produces no edge. The
         // bridge/immediate callers (wake_next=false) notify in the current
         // iteration as before. Set the cached event flag directly because
         // sync_event_cache only clears it -- notify_event normally sets it.
         n->event_delta = m->iteration + (wake_next ? 1 : 0);

         if (wake_next) {
            if (n->flags & NET_F_CACHE_EVENT)
               n->signal->shared.flags |= SIG_F_EVENT_FLAG;
            m->next_is_delta = true;
         }
         else
            assert(!(n->flags & NET_F_CACHE_EVENT));

         wakeup_all(m, &(n->pending));

         for (rt_source_t *o = n->outputs; o; o = o->chain_output) {
            switch (o->tag) {
            case SOURCE_PORT:
               defer_driving_update(m, o->u.port.output);
               break;
            case SOURCE_IMPLICIT:
               // Reverse implicit: receiver deposit propagates to parent
               defer_driving_update(m, o->u.pseudo.nexus);
               break;
            default:
               should_not_reach_here();
            }
            m->next_is_delta = true;
         }
      }

      vptr += valuesz;
   }
}

void deposit_signal(rt_model_t *m, rt_signal_t *s, const void *values,
                    int offset, size_t count)
{
   // Public/bridge entry point: immediate notify in the current iteration.
   deposit_signal_impl(m, s, values, offset, count, false);
}

void sched_deposit(rt_model_t *m, rt_signal_t *s, const void *values,
                   int offset, size_t count, int64_t after, bool nonblock)
{
   RT_LOCK(s->lock);

   TRACE("schedule deposit %s+%d value=%s count=%zd after=%s",
         istr(tree_ident(s->where)), offset,
         fmt_values(values, count * s->nexus.size), count, trace_time(after));

   assert(m->can_create_delta);

   rt_nexus_t *n = split_nexus(m, s, offset, count);
   const char *vptr = values;
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      rt_source_t *src = get_pseudo_source(m, n, SOURCE_DEPOSIT);
      copy_value_ptr(n, &(src->u.pseudo.value), vptr);
      src->disconnected = 0;

      if (!src->pseudoqueued) {
         if (after == 0) {
            if (nonblock)
               deferq_do(&m->nonblockq, async_pseudo_source, src);
            else
               deltaq_insert_pseudo_source(m, src);
         }
         else if (after > 0) {
            void *e = tag_pointer(src, EVENT_PSEUDO);
            heap_insert(m->eventq_heap, m->now + after, e);
         }

         src->pseudoqueued = 1;  // TODO: should be after == 0 branch
      }

      vptr += n->width * n->size;
   }
}

bool model_can_create_delta(rt_model_t *m)
{
   return m->can_create_delta;
}

int64_t model_now(rt_model_t *m, unsigned *deltas)
{
   if (deltas != NULL)
      *deltas = MAX(m->iteration, 0);

   return m->now;
}

int64_t model_next_time(rt_model_t *m)
{
   if (heap_size(m->eventq_heap) == 0)
      return TIME_HIGH;
   else
      return heap_min_key(m->eventq_heap);
}

void model_stop(rt_model_t *m)
{
   relaxed_store(&m->force_stop, true);
}

void model_set_phase_cb(rt_model_t *m, model_phase_t phase, rt_event_fn_t fn,
                        void *user)
{
   // Add to end of list so callbacks are called in registration order
   rt_callback_t **p = &(m->phase_cbs[phase]);
   for (; *p; p = &(*p)->next);

   rt_callback_t *cb = xcalloc(sizeof(rt_callback_t));
   cb->next = NULL;
   cb->fn   = fn;
   cb->user = user;

   *p = cb;
}

void model_set_timeout_cb(rt_model_t *m, uint64_t when, rt_event_fn_t fn,
                          void *user)
{
   rt_callback_t *cb = xcalloc(sizeof(rt_callback_t));
   cb->next = NULL;
   cb->fn   = fn;
   cb->user = user;

   assert(when > m->now);   // TODO: delta timeouts?

   void *e = tag_pointer(cb, EVENT_TIMEOUT);
   heap_insert(m->eventq_heap, when, e);
}

rt_watch_t *watch_new(rt_model_t *m, sig_event_fn_t fn, void *user,
                      watch_kind_t kind, unsigned slots)
{
   rt_watch_t *w = xcalloc_flex(sizeof(rt_watch_t), slots,
                                sizeof(rt_signal_t *));
   w->fn        = fn;
   w->chain_all = m->watches;
   w->user_data = user;
   w->num_slots = slots;

   w->wakeable.kind      = W_WATCH;
   w->wakeable.postponed = (kind == WATCH_POSTPONED);
   w->wakeable.pending   = false;
   w->wakeable.delayed   = false;

   m->watches = w;

   return w;
}

void watch_free(rt_model_t *m, rt_watch_t *w)
{
   assert(!w->wakeable.zombie);

   for (int i = 0; i < w->next_slot; i++) {
      rt_nexus_t *n = &(w->signals[i]->nexus);
      for (int j = 0; j < w->signals[i]->n_nexus; j++, n = n->chain)
         clear_event(m, &(n->pending), &(w->wakeable));
   }

   rt_watch_t **last = &m->watches;
   for (rt_watch_t *it = *last; it;
        last = &(it->chain_all), it = it->chain_all) {
      if (it == w) {
         *last = it->chain_all;
         if (w->wakeable.pending)
            w->wakeable.zombie = true;   // Will be freed in callback
         else
            free(w);
         return;
      }
   }

   should_not_reach_here();
}

rt_watch_t *model_set_event_cb(rt_model_t *m, rt_signal_t *s, rt_watch_t *w)
{
   assert(!w->wakeable.zombie);
   assert(w->next_slot < w->num_slots);

   w->signals[w->next_slot++] = s;

   rt_nexus_t *n = &(s->nexus);
   for (int i = 0; i < s->n_nexus; i++, n = n->chain)
      sched_event(m, &(n->pending), &(w->wakeable));

   return w;
}

static void handle_interrupt_cb(jit_t *j, void *ctx)
{
   rt_proc_t *proc = get_active_proc();

   if (proc != NULL)
      jit_msg(NULL, DIAG_FATAL, "interrupted in process %s", istr(proc->name));
   else {
      diag_t *d = diag_new(DIAG_FATAL, NULL);
      diag_printf(d, "interrupted");
      diag_emit(d);
   }
}

void model_interrupt(rt_model_t *m)
{
   model_stop(m);
   jit_interrupt(m->jit, handle_interrupt_cb, m);
}

int model_exit_status(rt_model_t *m)
{
   int status;
   if (jit_exit_status(m->jit, &status))
      return status;
   else if (m->stop_delta > 0 && m->iteration == m->stop_delta)
      return EXIT_FAILURE;
   else
      return get_vhdl_assert_exit_status();
}

static bool nexus_active(rt_model_t *m, rt_nexus_t *nexus)
{
   if (nexus->n_sources > 0) {
      for (rt_source_t *s = &(nexus->sources); s; s = s->chain_input) {
         if (s->tag == SOURCE_PORT) {
            rt_conv_func_t *cf = s->u.port.conv_func;
            if (cf == NULL) {
               RT_LOCK(s->u.port.input->signal->lock);
               if (nexus_active(m, s->u.port.input))
                  return true;
            }
            else {
               for (int i = 0; i < cf->ninputs; i++) {
                  if (nexus_active(m, cf->inputs[i].nexus))
                     return true;
               }
            }
         }
         else if (s->tag == SOURCE_DRIVER
                  && s->u.driver.waveforms.when == m->now) {
            if (nexus->active_delta == m->iteration)
               return true;
            else if (nexus->active_delta == m->iteration + 1 && s->was_active)
               return true;
         }
      }
   }

   return false;
}

static uint64_t nexus_last_active(rt_model_t *m, rt_nexus_t *nexus)
{
   int64_t last = TIME_HIGH;

   if (nexus->n_sources > 0) {
      for (rt_source_t *s = &(nexus->sources); s; s = s->chain_input) {
          if (s->tag == SOURCE_PORT) {
            rt_conv_func_t *cf = s->u.port.conv_func;
            if (cf == NULL) {
               RT_LOCK(s->u.port.input->signal->lock);
               last = MIN(last, nexus_last_active(m, s->u.port.input));
            }
            else {
               for (int i = 0; i < cf->ninputs; i++) {
                  RT_LOCK(cf->inputs[i].nexus->signal->lock);
                  last = MIN(last, nexus_last_active(m, cf->inputs[i].nexus));
               }
            }
         }
         else if (s->tag == SOURCE_DRIVER
                  && s->u.driver.waveforms.when <= m->now)
            last = MIN(last, m->now - s->u.driver.waveforms.when);
      }
   }

   return last;
}

void get_forcing_value(rt_signal_t *s, uint8_t *value)
{
   uint8_t *p = value;
   rt_nexus_t *n = &(s->nexus);
   for (int i = 0; i < s->n_nexus; i++) {
      assert(n->n_sources > 0);
      rt_source_t *s = NULL;
      for (s = &(n->sources); s; s = s->chain_input) {
         if (s->tag == SOURCE_FORCING)
            break;
      }
      assert(s != NULL);

      memcpy(p, s->u.pseudo.value.bytes, n->width * n->size);
      p += n->width * n->size;
   }
   assert(p == value + s->shared.size);
}

static void arm_trigger(rt_model_t *m, rt_trigger_t *t, rt_wakeable_t *obj)
{
   switch (t->kind) {
   case CMP_TRIGGER:
      {
         assert(t->nargs == 3);
         rt_signal_t *s = t->args[0].pointer;
         int32_t offset = t->args[1].integer;

         rt_nexus_t *n = split_nexus(m, s, offset, 1);
         sched_event(m, &(n->pending), obj);
      }
      break;
   case FUNC_TRIGGER:
      {
         if (t->nargs >= 3) {
            sig_shared_t *ss = t->args[1].pointer;
            int32_t offset = t->args[2].integer;

            rt_signal_t *s = container_of(ss, rt_signal_t, shared);

            rt_nexus_t *n = split_nexus(m, s, offset, 1);
            sched_event(m, &(n->pending), obj);
         }
      }
      break;
   case LEVEL_TRIGGER:
      {
         assert(t->nargs == 3);
         rt_signal_t *s = t->args[0].pointer;
         int32_t offset = t->args[1].integer;
         int32_t count = t->args[2].integer;

         rt_nexus_t *n = split_nexus(m, s, offset, count);
         for (; count > 0; n = n->chain) {
            sched_event(m, &(n->pending), obj);

            count -= n->width;
            assert(count >= 0);
         }
      }
      break;
   case OR_TRIGGER:
      {
         assert(t->nargs == 2);
         arm_trigger(m, t->args[0].pointer, obj);
         arm_trigger(m, t->args[1].pointer, obj);
      }
      break;
   }
}

static rt_trigger_t *new_trigger(rt_model_t *m, trigger_kind_t kind,
                                 uint64_t hash, jit_handle_t handle,
                                 unsigned nargs, const jit_scalar_t *args)
{
   rt_trigger_t **bucket = &(m->triggertab[hash % TRIGGER_TAB_SIZE]);

   for (rt_trigger_t *exist = *bucket; exist; exist = exist->chain) {
      bool hit = exist->handle == handle
         && exist->nargs == nargs
         && exist->kind == kind;

      for (int i = 0; hit && i < nargs; i++)
         hit &= (exist->args[i].integer == args[i].integer);

      if (hit)
         return exist;
   }

   const size_t argsz = nargs * sizeof(jit_scalar_t);

   rt_trigger_t *t = static_alloc(m, sizeof(rt_trigger_t) + argsz);
   memset(t, '\0', sizeof(rt_trigger_t));
   t->wakeable.kind = W_TRIGGER;
   t->handle = handle;
   t->nargs  = nargs;
   t->epoch  = UINT64_MAX;
   t->kind   = kind;
   t->chain  = *bucket;
   memcpy(t->args, args, argsz);

   return (*bucket = t);
}

void call_with_model(rt_model_t *m, void (*cb)(void *), void *arg)
{
   MODEL_ENTRY(m);
   (*cb)(arg);
}

void get_instance_name(rt_scope_t *s, text_buf_t *tb)
{
   if (s->kind == SCOPE_ROOT)
      return;

   tree_t hier = tree_decl(s->where, 0);
   assert(tree_kind(hier) == T_HIER);

   switch (tree_subkind(hier)) {
   case T_ARCH:
      {
         tree_t unit = tree_ref(hier);

         get_instance_name(s->parent, tb);

         if (s->parent->kind != SCOPE_ROOT) {
            tree_t hier2 = tree_decl(s->parent->where, 0);
            assert(tree_kind(hier2) == T_HIER);

            if (tree_subkind(hier2) != T_COMPONENT) {
               tb_append(tb, ':');
               tb_istr(tb, tree_ident(s->where));
            }

            tb_append(tb, '@');
         }
         else
            tb_append(tb, ':');

         const char *arch = strchr(istr(tree_ident(unit)), '-') + 1;
         tb_printf(tb, "%s(%s)", istr(tree_ident2(unit)), arch);
      }
      break;

   case T_BLOCK:
   case T_FOR_GENERATE:
   case T_IF_GENERATE:
   case T_CASE_GENERATE:
      get_instance_name(s->parent, tb);
      tb_append(tb, ':');
      tb_printf(tb, "%s", istr(tree_ident(s->where)));
      break;

   case T_COMPONENT:
      get_instance_name(s->parent, tb);
      tb_printf(tb, ":%s", istr(tree_ident(s->where)));
      break;

   default:
      should_not_reach_here();
   }

   tb_downcase(tb);
}

void get_path_name(rt_scope_t *s, text_buf_t *tb)
{
   if (s->kind == SCOPE_ROOT)
      return;

   get_path_name(s->parent, tb);

   if (s->parent->kind != SCOPE_ROOT) {
      tree_t hier = tree_decl(s->parent->where, 0);
      assert(tree_kind(hier) == T_HIER);

      if (tree_subkind(hier) == T_COMPONENT)
         return;   // Skip implicit block for components
   }

   tb_append(tb, ':');
   tb_istr(tb, tree_ident(s->where));
   tb_downcase(tb);
}

////////////////////////////////////////////////////////////////////////////////
// Entry points from compiled code

sig_shared_t *x_init_signal(int64_t count, uint32_t size, jit_scalar_t value,
                            bool scalar, sig_flags_t flags, tree_t where,
                            int32_t offset)
{
   TRACE("init signal %s count=%"PRIi64" size=%d value=%s flags=%x offset=%d",
         istr(tree_ident(where)), count, size,
         fmt_jit_value(value, scalar, size * count), flags, offset);

   rt_model_t *m = get_model();

   if (count > INT32_MAX)
      jit_msg(tree_loc(where), DIAG_FATAL, "signal %s has %"PRIi64
              " sub-elements which is greater than the maximum supported %d",
              istr(tree_ident(where)), count, INT32_MAX);

   const size_t datasz = MAX(3 * count * size, 8);
   rt_signal_t *s = static_alloc(m, sizeof(rt_signal_t) + datasz);
   setup_signal(m, s, where, count, size, flags, offset);

   // The driving value area is also used to save the default value
   void *driving = s->shared.data + 2*s->shared.size;

   if (scalar) {
#define COPY_SCALAR(type) do {                  \
         type *pi = (type *)s->shared.data;     \
         type *pd = (type *)driving;            \
         for (int i = 0; i < count; i++)        \
            pi[i] = pd[i] = value.integer;      \
      } while (0)

      FOR_ALL_SIZES(size, COPY_SCALAR);
   }
   else {
      memcpy(s->shared.data, value.pointer, s->shared.size);
      memcpy(driving, value.pointer, s->shared.size);
   }

   return &(s->shared);
}

void x_drive_signal(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("drive signal %s+%d count=%d", istr(tree_ident(s->where)),
         offset, count);

   rt_model_t *m = get_model();
   rt_proc_t *proc = get_active_proc();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      rt_source_t *s;
      for (s = &(n->sources); s; s = s->chain_input) {
         if (s->tag == SOURCE_DRIVER && s->u.driver.proc == proc)
            break;
      }

      if (s == NULL) {
         s = add_source(m, n, SOURCE_DRIVER);
         s->u.driver.waveforms.value = alloc_value(m, n);
         s->u.driver.proc = proc;
      }

      count -= n->width;
      assert(count >= 0);
   }
}

void x_sched_process(int64_t delay)
{
   rt_proc_t *proc = get_active_proc();
   if (proc == NULL)
      return;    // May be called during constant folding

   TRACE("schedule process %s delay=%s", istr(proc->name), trace_time(delay));

   check_delay(delay);

   // Verilog mode: a zero-delay wait is #0 -- resume in the INACTIVE region,
   // after every active-region delta at this time has settled, not merely one
   // delta later. The translated `wait for 0 ns` exists precisely to emulate
   // #0 and blocking-read settling, and one delta races comb reactions whose
   // driver updates land a delta after the wakeup.
   rt_model_t *m = get_model();
   if (delay == 0 && standard() == STD_MX) {
      set_pending(&proc->wakeable);
      deferq_do(&m->inactiveq, async_run_process, proc);
      m->next_is_delta = true;
      return;
   }

   deltaq_insert_proc(m, delay, proc);
}

void x_sched_inactive(void)
{
   rt_proc_t *proc = get_active_proc();
   rt_model_t *m = get_model();

   TRACE("schedule process %s in inactive region", istr(proc->name));

   set_pending(&proc->wakeable);
   deferq_do(&m->inactiveq, async_run_process, proc);
   m->next_is_delta = true;
}

void x_sched_waveform_s(sig_shared_t *ss, uint32_t offset, uint64_t scalar,
                        int64_t after, int64_t reject)
{
   if (unlikely(relaxed_load(&g_par_active))) {
      // Worker eval: defer the driver write to thread 0 via the pipe.
      const int tid = thread_id();
      prop_rec_t *r = prop_reserve(tid);
      r->ss = ss; r->offset = offset; r->count = 1;
      r->after = after; r->reject = reject; r->proc = get_active_proc();
      r->scalar = true; r->sval = scalar; r->heapval = NULL;
      prop_commit(tid);
      return;
   }

   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("_sched_waveform_s %s+%d value=%"PRIi64" after=%s reject=%s",
         istr(tree_ident(s->where)), offset, scalar, trace_time(after),
         trace_time(reject));

   rt_proc_t *proc = get_active_proc();

   check_delay(after);
   check_postponed(after, proc);
   check_reject_limit(s, after, reject);

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, 1);

   sched_driver(m, n, after, reject, &scalar, proc);
}

void x_sched_waveform(sig_shared_t *ss, uint32_t offset, void *values,
                      int32_t count, int64_t after, int64_t reject)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   if (unlikely(relaxed_load(&g_par_active))) {
      // Worker eval: defer the driver write to thread 0 via the pipe. Copy the
      // value (transient on the worker); wide values go to a heap buffer the
      // applier frees.
      const int tid = thread_id();
      const uint32_t nbytes = count * s->nexus.size;
      prop_rec_t *r = prop_reserve(tid);
      r->ss = ss; r->offset = offset; r->count = count;
      r->after = after; r->reject = reject; r->proc = get_active_proc();
      r->scalar = false; r->nbytes = nbytes;
      if (likely(nbytes <= PROP_VALSZ)) {
         r->heapval = NULL;
         memcpy(r->value, values, nbytes);
      }
      else {
         r->heapval = xmalloc(nbytes);
         memcpy(r->heapval, values, nbytes);
      }
      prop_commit(tid);
      return;
   }

   RT_LOCK(s->lock);

   TRACE("_sched_waveform %s+%d value=%s count=%d after=%s reject=%s",
         istr(tree_ident(s->where)), offset,
         fmt_values(values, count * s->nexus.size),
         count, trace_time(after), trace_time(reject));

   rt_proc_t *proc = get_active_proc();

   check_delay(after);
   check_postponed(after, proc);
   check_reject_limit(s, after, reject);

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   char *vptr = values;
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      sched_driver(m, n, after, reject, vptr, proc);
      vptr += n->width * n->size;
   }
}

void x_transfer_signal(sig_shared_t *target_ss, uint32_t toffset,
                       sig_shared_t *source_ss, uint32_t soffset,
                       int32_t count, int64_t after, int64_t reject)
{
   rt_signal_t *target = container_of(target_ss, rt_signal_t, shared);
   rt_signal_t *source = container_of(source_ss, rt_signal_t, shared);

   TRACE("transfer signal %s+%d to %s+%d count=%d",
         istr(tree_ident(source->where)), soffset,
         istr(tree_ident(target->where)), toffset, count);

   rt_proc_t *proc = get_active_proc();

   check_delay(after);
   check_postponed(after, proc);
   check_reject_limit(target, after, reject);

   rt_model_t *m = get_model();

   rt_transfer_t *t = static_alloc(m, sizeof(rt_transfer_t));
   t->proc   = proc;
   t->target = split_nexus(m, target, toffset, count);
   t->source = split_nexus(m, source, soffset, count);
   t->count  = count;
   t->after  = after;
   t->reject = reject;

   t->wakeable.kind      = W_TRANSFER;
   t->wakeable.postponed = false;
   t->wakeable.pending   = false;
   t->wakeable.delayed   = false;

   // Ensure each target nexus has a SOURCE_DRIVER for the active process.
   // Without this, async_transfer_signal -> sched_driver -> find_driver
   // returns NULL and we crash. This handles iverilog-generated VHDL where
   // the concurrent assignment doesn't pre-register a driver via x_drive_signal.
   {
      int tcount = count;
      for (rt_nexus_t *n = t->target; tcount > 0; n = n->chain) {
         rt_source_t *s;
         for (s = &(n->sources); s; s = s->chain_input) {
            if (s->tag == SOURCE_DRIVER && s->u.driver.proc == proc)
               break;
         }
         if (s == NULL) {
            s = add_source(m, n, SOURCE_DRIVER);
            s->u.driver.waveforms.value = alloc_value(m, n);
            s->u.driver.proc = proc;
         }
         tcount -= n->width;
         assert(tcount >= 0);
      }
   }

   for (rt_nexus_t *n = t->source; count > 0; n = n->chain) {
      sched_event(m, &(n->pending), &(t->wakeable));

      if (!t->wakeable.pending) {
         // Schedule initial update immediately
         deferq_do(&m->procq, async_transfer_signal, t);
         t->wakeable.pending = true;
      }

      count -= n->width;
      assert(count >= 0);
   }
}

int32_t x_test_net_event(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("_test_net_event %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   int32_t result = 0;
   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      if (n->last_event == m->now && n->event_delta == m->iteration) {
         result = 1;
         break;
      }

      count -= n->width;
      assert(count >= 0);
   }

   if (ss->size == s->nexus.size) {
      assert(!(ss->flags & SIG_F_CACHE_EVENT));   // Should have taken fast-path
      ss->flags |= SIG_F_CACHE_EVENT | (result ? SIG_F_EVENT_FLAG : 0);
      s->nexus.flags |= NET_F_CACHE_EVENT;
      APUSH(m->eventsigs, s);
   }

   return result;
}

int32_t x_test_net_active(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("_test_net_active %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      if (nexus_active(m, n))
         return 1;

      count -= n->width;
      assert(count >= 0);
   }

   return 0;
}

void x_sched_event(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("_sched_event %s+%d count=%d", istr(tree_ident(s->where)),
         offset, count);

   rt_wakeable_t *obj = get_active_wakeable();

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      sched_event(m, &(n->pending), obj);

      count -= n->width;
      assert(count >= 0);
   }
}

void x_clear_event(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("clear event %s+%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   rt_model_t *m = get_model();
   rt_proc_t *proc = get_active_proc();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      clear_event(m, &(n->pending), &(proc->wakeable));

      count -= n->width;
      assert(count >= 0);
   }
}

void x_enable_trigger(rt_trigger_t *trigger)
{
   TRACE("enable trigger %p", trigger);

   rt_wakeable_t *obj = get_active_wakeable();
   rt_model_t *m = get_model();

   if (trigger->pending == NULL)
      arm_trigger(m, trigger, &(trigger->wakeable));

   sched_event(m, &(trigger->pending), obj);
}

void x_disable_trigger(rt_trigger_t *trigger)
{
   TRACE("disable trigger %p", trigger);

   rt_wakeable_t *obj = get_active_wakeable();
   rt_model_t *m = get_model();

   clear_event(m, &(trigger->pending), obj);
}

void x_enter_state(int32_t state, bool strong)
{
   rt_wakeable_t *obj = get_active_wakeable();
   assert(obj->kind == W_PROPERTY);

   rt_prop_t *prop = container_of(obj, rt_prop_t, wakeable);
   mask_set(&prop->newstate, state);
   prop->strong |= strong;
}

void x_alias_signal(sig_shared_t *ss, tree_t where)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("alias signal %s to %s", istr(tree_ident(s->where)),
         istr(tree_ident(where)));

   rt_alias_t *a = xcalloc(sizeof(rt_alias_t));
   a->where  = where;
   a->signal = s;

   model_thread_t *thread = model_thread(get_model());
   APUSH(thread->active_scope->aliases, a);
}

int64_t x_last_event(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("_last_event %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   int64_t last = TIME_HIGH;

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      if (n->last_event <= m->now)
         last = MIN(last, m->now - n->last_event);

      count -= n->width;
      assert(count >= 0);
   }

   return last;
}

int64_t x_last_active(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("_last_active %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   int64_t last = TIME_HIGH;

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      last = MIN(last, nexus_last_active(m, n));

      count -= n->width;
      assert(count >= 0);
   }

   return last;
}

void x_map_signal(sig_shared_t *src_ss, uint32_t src_offset,
                  sig_shared_t *dst_ss, uint32_t dst_offset, uint32_t count)
{
   rt_signal_t *src_s = container_of(src_ss, rt_signal_t, shared);
   RT_LOCK(src_s->lock);

   rt_signal_t *dst_s = container_of(dst_ss, rt_signal_t, shared);
   RT_LOCK(dst_s->lock);

   TRACE("map signal %s+%d to %s+%d count %d",
         istr(tree_ident(src_s->where)), src_offset,
         istr(tree_ident(dst_s->where)), dst_offset, count);

   assert(src_s != dst_s);

   rt_model_t *m = get_model();

   rt_nexus_t *src_n = split_nexus(m, src_s, src_offset, count);
   rt_nexus_t *dst_n = split_nexus(m, dst_s, dst_offset, count);

   while (count > 0) {
      if (src_n->width > dst_n->width)
         clone_nexus(m, src_n, dst_n->width);
      else if (src_n->width < dst_n->width)
         clone_nexus(m, dst_n, src_n->width);

      assert(src_n->width == dst_n->width);
      assert(src_n->size == dst_n->size);

      // Effective value updates must propagate through ports
      src_n->flags |= (dst_n->flags & NET_F_EFFECTIVE);
      dst_n->flags |= (src_n->flags & NET_F_EFFECTIVE);

      rt_source_t *port = add_source(m, dst_n, SOURCE_PORT);
      port->u.port.input = src_n;

      port->chain_output = src_n->outputs;
      src_n->outputs = port;

      count -= src_n->width;
      assert(count >= 0);

      src_n = src_n->chain;
      dst_n = dst_n->chain;
   }
}

void x_map_const(sig_shared_t *ss, uint32_t offset,
                 const uint8_t *values, uint32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("map const %s to %s+%d count %d", fmt_values(values, count),
         istr(tree_ident(s->where)), offset, count);

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      const size_t valuesz = n->size * n->width;
      memcpy(nexus_effective(n), values, valuesz);
      memcpy(nexus_initial(n), values, valuesz);

      n->flags |= NET_F_HAS_INITIAL;
      values += valuesz;

      count -= n->width;
      assert(count >= 0);
   }
}

void x_map_implicit(sig_shared_t *src_ss, uint32_t src_offset,
                    sig_shared_t *dst_ss, uint32_t dst_offset,
                    uint32_t count)
{
   rt_signal_t *src_s = container_of(src_ss, rt_signal_t, shared);
   RT_LOCK(src_s->lock);

   rt_signal_t *dst_s = container_of(dst_ss, rt_signal_t, shared);
   RT_LOCK(dst_s->lock);

   TRACE("map implicit signal %s+%d to %s+%d count %d",
         istr(tree_ident(src_s->where)), src_offset,
         istr(tree_ident(dst_s->where)), dst_offset, count);

   assert(src_s != dst_s);
   assert(dst_offset == 0);

   rt_model_t *m = get_model();
   rt_nexus_t *src_n = split_nexus(m, src_s, src_offset, count);

   // For implicit signals like 'stable/'quiet the destination is a scalar
   // fan-in from every source nexus: its width is 1 even when the prefix is
   // wider.  Split dst using its actual total width, not the src count.
   const uint32_t dst_total = dst_s->shared.size / dst_s->nexus.size;
   rt_nexus_t *dst_n = split_nexus(m, dst_s, dst_offset, MIN(count, dst_total));

   for (; count > 0; src_n = src_n->chain) {
      count -= src_n->width;
      assert(count >= 0);

      rt_source_t *src = add_source(m, dst_n, SOURCE_IMPLICIT);
      src->u.port.input = src_n;

      src->chain_output = src_n->outputs;
      src_n->outputs = src;

      src_n->flags |= NET_F_EFFECTIVE;   // Update outputs when active
      src_n->flags &= ~NET_F_FAST_DRIVER;

      if (count > 0 && dst_n->chain != NULL)
         dst_n = dst_n->chain;
   }
}

void x_push_scope(tree_t where, int32_t size, rt_scope_kind_t kind)
{
   TRACE("push scope %s size=%d kind=%d", istr(tree_ident(where)), size, kind);

   rt_model_t *m = get_model();
   model_thread_t *thread = model_thread(m);

   ident_t name;
   if (thread->active_scope && thread->active_scope->kind == SCOPE_ARRAY)
      name = ident_sprintf("%s(%d)", istr(thread->active_scope->name),
                           thread->active_scope->children.count);
   else if (thread->active_scope && thread->active_scope->kind == SCOPE_RECORD)
      name = ident_prefix(thread->active_scope->name, tree_ident(where), '.');
   else
      name = tree_ident(where);

   rt_scope_t *s = xcalloc(sizeof(rt_scope_t));
   s->where    = where;
   s->name     = name;
   s->kind     = kind;
   s->parent   = thread->active_scope;
   s->size     = size;
   s->privdata = MPTR_INVALID;

   if (kind != SCOPE_PACKAGE) {
      type_t type = tree_type(where);
      assert(type_is_composite(type));
      if (type_kind(type) == T_SUBTYPE && type_has_resolution(type))
         s->flags |= SCOPE_F_RESOLVED;
   }

   thread->active_scope = s;
}

void x_pop_scope(void)
{
   rt_model_t *m = get_model();
   model_thread_t *thread = model_thread(m);

   rt_scope_t *pop = thread->active_scope, *old = pop->parent;

   TRACE("pop scope %s", istr(tree_ident(pop->where)));

   int offset = INT_MAX;
   for (int i = 0; i < pop->children.count; i++)
      offset = MIN(offset, pop->children.items[i]->offset);
   for (int i = 0; i < pop->signals.count; i++)
      offset = MIN(offset, pop->signals.items[i]->offset);
   pop->offset = offset;

   thread->active_scope = old;

   if (pop->kind == SCOPE_PACKAGE)
      pop->parent = m->root;   // Always attach packages to root scope

   APUSH(pop->parent->children, pop);
}

bool x_driving(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("_driving %s offset=%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   int ntotal = 0, ndriving = 0;
   bool found = false;
   rt_model_t *m = get_model();
   rt_proc_t *proc = get_active_proc();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      if (n->n_sources > 0) {
         rt_source_t *src = find_driver(n, proc);
         if (src != NULL) {
            if (!src->disconnected) ndriving++;
            found = true;
         }
      }

      ntotal++;
      count -= n->width;
      assert(count >= 0);
   }

   if (!found)
      jit_msg(NULL, DIAG_FATAL, "process %s does not contain a driver for %s",
              istr(proc->name), istr(tree_ident(s->where)));

   return ntotal == ndriving;
}

void *x_driving_value(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("driving value %s offset=%d count=%d", istr(tree_ident(s->where)),
         offset, count);

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);

   rt_proc_t *proc = get_active_proc();
   if (proc == NULL) {   // Called in output conversion
      if (n->flags & NET_F_EFFECTIVE)
         return nexus_driving(n);
      else
         return nexus_effective(n);
   }

   void *result = tlab_alloc(model_thread(m)->tlab, s->shared.size);

   uint8_t *p = result;
   for (; count > 0; n = n->chain) {
      rt_source_t *src = find_driver(n, proc);
      if (src == NULL)
         jit_msg(NULL, DIAG_FATAL, "process %s does not contain a driver "
                 "for %s", istr(proc->name), istr(tree_ident(s->where)));

      const uint8_t *driving;
      if (n->flags & NET_F_FAST_DRIVER)
         driving = nexus_effective(n);
      else
         driving = value_ptr(n, &(src->u.driver.waveforms.value));

      memcpy(p, driving, n->width * n->size);
      p += n->width * n->size;

      count -= n->width;
      assert(count >= 0);
   }

   return result;
}

sig_shared_t *x_implicit_signal(uint32_t count, uint32_t size, tree_t where,
                                implicit_kind_t kind, ffi_closure_t *closure,
                                int64_t delay)
{
   TRACE("implicit signal %s count=%d size=%d kind=%d",
         istr(tree_ident(where)), count, size, kind);

   rt_model_t *m = get_model();

   const size_t datasz = MAX(3 * count * size, 8);
   rt_implicit_t *imp = static_alloc(m, sizeof(rt_implicit_t) + datasz);
   setup_signal(m, &(imp->signal), where, count, size, SIG_F_IMPLICIT, 0);

   imp->closure = *closure;
   imp->delay = delay;
   imp->wakeable.kind = W_IMPLICIT;

   deferq_do(&m->implicitq, async_update_implicit_signal, imp);
   set_pending(&(imp->wakeable));

   if (kind == IMPLICIT_STABLE || kind == IMPLICIT_QUIET) {
      add_source(m, &(imp->signal.nexus), SOURCE_DRIVER);
      imp->signal.shared.data[0] = 1;    // X'STABLE initally true
   }

   return &(imp->signal.shared);
}

void x_disconnect(sig_shared_t *ss, uint32_t offset, int32_t count,
                  int64_t after, int64_t reject)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("_disconnect %s+%d len=%d after=%s reject=%s",
         istr(tree_ident(s->where)), offset, count, trace_time(after),
         trace_time(reject));

   rt_proc_t *proc = get_active_proc();

   check_postponed(after, proc);
   check_reject_limit(s, after, reject);

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      sched_disconnect(m, n, after, reject, proc);
   }
}

void x_force(sig_shared_t *ss, uint32_t offset, int32_t count, void *values)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("force signal %s+%d value=%s count=%d", istr(tree_ident(s->where)),
         offset, fmt_values(values, count), count);

   rt_proc_t *proc = get_active_proc();
   rt_model_t *m = get_model();

   check_postponed(0, proc);

   force_signal(m, s, values, offset, count);
}

void x_release(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("release signal %s+%d count=%d", istr(tree_ident(s->where)),
         offset, count);

   rt_proc_t *proc = get_active_proc();
   rt_model_t *m = get_model();

   check_postponed(0, proc);

   release_signal(m, s, offset, count);
}

void x_deposit_signal(sig_shared_t *ss, uint32_t offset, int32_t count,
                      void *values)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   rt_model_t *m = get_model();

   // A blocking-assigned signal (Verilog := / T_DEPOSIT) must (a) take its new
   // value immediately, so a concurrent <= driver on the same signal and any
   // same-time reads are not perturbed, and (b) wake edge-sensitive receivers
   // with S'event true. A plain deposit_signal() sets event_delta in the
   // current iteration, but the receivers it wakes do not run until the next
   // delta cycle, where S'event reads false -- so a blocking-assigned clock
   // toggled its value but never produced an edge. Routing through the delta
   // queue (sched_deposit) fixes the event but leaves a persistent DEPOSIT
   // pseudo-source that resolves against a real <= driver to 'U'. Instead apply
   // the value immediately and attribute the event to the next iteration
   // (wake_next), which the receivers actually run in. At initialization (no
   // delta cycles yet, no receivers waiting) notify in the current iteration.
   deposit_signal_impl(m, s, values, offset, count, m->can_create_delta);
}

void x_sched_deposit(sig_shared_t *ss, uint32_t offset, int32_t count,
                     void *values, int64_t after)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   rt_model_t *m = get_model();

   sched_deposit(m, s, values, offset, count, after, true);
}

void x_put_driver(sig_shared_t *ss, uint32_t offset, int32_t count,
                  void *values)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("put driver %s+%d value=%s count=%d", istr(tree_ident(s->where)),
         offset, fmt_values(values, count * s->nexus.size), count);

   rt_proc_t *proc = get_active_proc();
   assert(proc->wakeable.kind == W_ASSIGN);

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   const char *vptr = values;
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      rt_source_t *d = find_driver(n, proc);
      assert(d != NULL);

      assert(d->u.driver.waveforms.next == NULL);
      copy_value_ptr(n, &d->u.driver.waveforms.value, vptr);

      n->vtable->update_driving(m, n);

      for (rt_source_t *o = n->outputs; o; o = o->chain_output) {
         assert(o->tag == SOURCE_PORT);
         defer_driving_update(m, o->u.port.output);
         m->next_is_delta = true;
      }

      vptr += n->size * n->width;
   }
}

void x_put_conversion(rt_conv_func_t *cf, sig_shared_t *ss, uint32_t offset,
                      int32_t count, void *values)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("put conversion %s+%d value=%s count=%d", istr(tree_ident(s->where)),
         offset, fmt_values(values, count * s->nexus.size), count);

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      rt_source_t *s = &(n->sources);
      for (; s; s = s->chain_input) {
         if (s->tag == SOURCE_PORT && s->u.port.conv_func == cf)
            break;
      }

      rt_value_t *result;
      if (s != NULL)
         result = &(s->u.port.conv_result);
      else {
         assert(n->flags & NET_F_EFFECTIVE);
         result = find_conversion_input(cf, n);
         assert(result != NULL);
      }

      copy_value_ptr(n, result, values);

      values += n->width * n->size;
   }
}

void x_init_pipe(sig_shared_t *ss, int32_t depth)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("init pipe %s depth=%d", istr(tree_ident(s->where)), depth);

   if (depth < 1)
      depth = 1;

   rt_nexus_t *n = &(s->nexus);
   for (int i = 0; i < s->n_nexus; i++, n = n->chain) {
      const uint32_t elem_size = n->size * n->width;
      const size_t fifo_size = sizeof(rt_pipe_fifo_t) + depth * elem_size;
      rt_pipe_fifo_t *fifo = xcalloc(fifo_size);
      fifo->capacity = depth;
      fifo->count = 0;
      fifo->head = 0;
      fifo->tail = 0;
      fifo->elem_size = elem_size;
      fifo->rd_wait = NULL;
      fifo->wr_wait = NULL;
      n->pipe_fifo = fifo;
   }
}

void x_pipe_write(sig_shared_t *ss, uint32_t offset, int32_t count,
                  void *values)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("pipe write %s+%d count=%d", istr(tree_ident(s->where)),
         offset, count);

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   const char *vptr = values;
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      rt_pipe_fifo_t *fifo = n->pipe_fifo;
      if (fifo != NULL && fifo->count < fifo->capacity) {
         uint8_t *dst = fifo->data + fifo->tail * fifo->elem_size;
         memcpy(dst, vptr, fifo->elem_size);
         fifo->tail = (fifo->tail + 1) % fifo->capacity;
         fifo->count++;
      }

      // Also update the signal data for normal propagation
      memcpy(ss->data + n->offset * n->size, vptr, n->size * n->width);

      vptr += n->size * n->width;
   }
}

void x_pipe_read(sig_shared_t *ss, uint32_t offset, int32_t count,
                 void *result)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("pipe read %s+%d count=%d", istr(tree_ident(s->where)),
         offset, count);

   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);
   char *rptr = result;
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      rt_pipe_fifo_t *fifo = n->pipe_fifo;
      if (fifo != NULL && fifo->count > 0) {
         uint8_t *src = fifo->data + fifo->head * fifo->elem_size;
         memcpy(rptr, src, fifo->elem_size);
         fifo->head = (fifo->head + 1) % fifo->capacity;
         fifo->count--;
      }
      else {
         // FIFO empty or not a pipe: read from signal data
         memcpy(rptr, ss->data + n->offset * n->size, n->size * n->width);
      }

      rptr += n->size * n->width;
   }
}

bool x_pipe_full(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);

   rt_pipe_fifo_t *fifo = n->pipe_fifo;
   return fifo != NULL && fifo->count >= fifo->capacity;
}

bool x_pipe_empty(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   rt_model_t *m = get_model();
   rt_nexus_t *n = split_nexus(m, s, offset, count);

   rt_pipe_fifo_t *fifo = n->pipe_fifo;
   return fifo == NULL || fifo->count == 0;
}

void x_resolve_signal(sig_shared_t *ss, jit_handle_t handle, void *context,
                      int32_t nlits, int32_t flags)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("resolve signal %s", istr(tree_ident(s->where)));

   ffi_closure_t closure = {
      .handle = handle,
      .context = context
   };

   rt_model_t *m = get_model();
   s->resolution = memo_resolution_fn(m, s, closure, nlits, flags);

   // Copy R_IDENT into the nexus flags to avoid rt_resolve_nexus_fast
   // having to dereference the resolution pointer in the common case
   if (s->resolution->flags & R_IDENT) {
      s->shared.flags |= NET_F_R_IDENT;

      rt_nexus_t *n = &(s->nexus);
      for (int i = 0; i < s->n_nexus; i++, n = n->chain)
         n->flags |= NET_F_R_IDENT;
   }
}

void x_process_init(jit_handle_t handle, tree_t where)
{
   rt_model_t *m = get_model();
   ident_t name = jit_get_name(m->jit, handle);

   TRACE("init process %s", istr(name));

   rt_scope_t *s = model_thread(m)->active_scope;
   assert(s != NULL);
   assert(s->kind == SCOPE_INSTANCE);

   rt_proc_t *p = xcalloc(sizeof(rt_proc_t));
   p->vtable    = &proc_default_vtable;
   p->where     = where;
   p->name      = name;
   p->handle    = handle;
   p->scope     = s;
   p->privdata  = mptr_new(m->mspace, "process privdata");

   p->wakeable.kind      = W_PROC;
   p->wakeable.pending   = false;
   p->wakeable.postponed = false;
   p->wakeable.delayed   = false;

   APUSH(s->procs, p);
}

void *x_function_trigger(jit_handle_t handle, unsigned nargs,
                         const jit_scalar_t *args)
{
   rt_model_t *m = get_model();

   uint64_t hash = mix_bits_32(handle);
   for (int i = 0; i < nargs; i++)
      hash ^= mix_bits_64(args[i].integer);

   TRACE("function trigger %s nargs=%u hash=%"PRIx64,
         istr(jit_get_name(m->jit, handle)), nargs, hash);

   return new_trigger(m, FUNC_TRIGGER, hash, handle, nargs, args);
}

rt_trigger_t *x_or_trigger(rt_trigger_t *left, rt_trigger_t *right)
{
   rt_model_t *m = get_model();

   uint64_t hash = mix_bits_64(left) ^ mix_bits_64(right);

   TRACE("or trigger %p %p hash=%"PRIx64, left, right, hash);

   const jit_scalar_t args[] = {
      { .pointer = left < right ? left : right },
      { .pointer = left < right ? right : left }
   };

   return new_trigger(m, OR_TRIGGER, hash, JIT_HANDLE_INVALID, 2, args);
}

void *x_cmp_trigger(sig_shared_t *ss, uint32_t offset, int64_t right)
{
   rt_model_t *m = get_model();
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   uint64_t hash = mix_bits_64(s) ^ mix_bits_32(offset) ^ mix_bits_64(right);

   TRACE("cmp trigger %s+%d right=%"PRIi64" hash=%"PRIx64,
         istr(tree_ident(s->where)), offset, right, hash);

   const jit_scalar_t args[] = {
      { .pointer = s },
      { .integer = offset },
      { .integer = right }
   };

   return new_trigger(m, CMP_TRIGGER, hash, JIT_HANDLE_INVALID, 3, args);
}

void *x_level_trigger(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_model_t *m = get_model();
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   uint64_t hash = mix_bits_64(s) ^ mix_bits_32(offset) ^ mix_bits_32(count);

   TRACE("level trigger %s+%d count=%d hash=%"PRIx64,
         istr(tree_ident(s->where)), offset, count, hash);

   const jit_scalar_t args[] = {
      { .pointer = s },
      { .integer = offset },
      { .integer = count }
   };

   return new_trigger(m, LEVEL_TRIGGER, hash, JIT_HANDLE_INVALID, 3, args);
}

void x_add_trigger(void *ptr)
{
   TRACE("add trigger %p", ptr);

   rt_wakeable_t *obj = get_active_wakeable();
   assert(obj->trigger == NULL);

   obj->trigger = ptr;
}

void *x_port_conversion(const ffi_closure_t *driving,
                        const ffi_closure_t *effective)
{
   rt_model_t *m = get_model();

   TRACE("port conversion %s context %p",
         istr(jit_get_name(m->jit, driving->handle)), driving->context);

   if (effective->handle != JIT_HANDLE_INVALID)
      TRACE("effective value conversion %s context %p",
            istr(jit_get_name(m->jit, effective->handle)), effective->context);

   const size_t tail_bytes = ALIGN_UP(sizeof(rt_conv_func_t), MEMBLOCK_ALIGN)
      - sizeof(rt_conv_func_t);
   const int tail_max_inputs = tail_bytes / sizeof(conv_input_t);
   assert(tail_max_inputs > 0);

   const size_t total_bytes =
      sizeof(rt_conv_func_t) + tail_max_inputs * sizeof(conv_input_t);

   rt_conv_func_t *cf = static_alloc(m, total_bytes);
   cf->driving   = *driving;
   cf->effective = *effective;
   cf->ninputs   = 0;
   cf->maxinputs = tail_max_inputs;
   cf->outputs   = NULL;
   cf->inputs    = cf->tail;
   cf->when      = TIME_HIGH;
   cf->iteration = UINT_MAX;

   return cf;
}

void x_convert_in(void *ptr, sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("convert in %p %s+%d count=%d", ptr, istr(tree_ident(s->where)),
         offset, count);

   rt_conv_func_t *cf = ptr;
   rt_model_t *m = get_model();

   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      add_conversion_input(m, cf, n);

      rt_source_t **p = &(n->outputs);
      for (; *p != NULL && *p != cf->outputs; p = &((*p)->chain_output));
      *p = cf->outputs;
   }
}

void x_convert_out(void *ptr, sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   TRACE("convert out %p %s+%d count=%d", ptr, istr(tree_ident(s->where)),
         offset, count);

   rt_conv_func_t *cf = ptr;
   rt_model_t *m = get_model();

   assert(cf->ninputs == 0);    // Add outputs first

   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      count -= n->width;
      assert(count >= 0);

      rt_source_t *src = add_source(m, n, SOURCE_PORT);
      src->u.port.conv_func   = cf;
      src->u.port.conv_result = alloc_value(m, n);

      src->chain_output = cf->outputs;
      cf->outputs = src;
   }
}

void x_instance_name(attr_kind_t kind, text_buf_t *tb)
{
   rt_model_t *m = get_model();
   rt_scope_t *s = get_active_scope(m);

   switch (kind) {
   case ATTR_INSTANCE_NAME:
      get_instance_name(s, tb);
      break;
   case ATTR_PATH_NAME:
      get_path_name(s, tb);
      break;
   default:
      should_not_reach_here();
   }
}
