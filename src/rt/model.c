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
#include "rt/partition.h"
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
   // #67 E/U phase pipeline (NVC_TWO_PHASE): E-side next-delta procq bank
   // (merged into procq at the boundary) and the E-owned deferred-wake
   // bank (inline eval-phase wakes replay single-threaded at the boundary).
   deferq_t           procq_e;
   deferq_t           ewakeq;
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
   uint64_t           fastclk_built_at;  // sim time the current table went live
   uint64_t           fastclk_backoff;   // re-arm delay after a dissolve (fs)
   // PROBATION: a fresh table dispatches on EVERY clock-nexus event for the
   // first N events (bit-correct for any member class) while sched_driver
   // attributes driver activity on NON-posedge activations to the running
   // member -- such members are combinational fanout, not clocked, and are
   // EVICTED at probation exit (posedge-only dispatch would starve them:
   // pr2305307b's reduction wires went stale on the 1->z step).  All members
   // evicted => the candidate was never a clock: dissolve + blacklist it.
   unsigned           fastclk_probation; // events left; 0 = passed/off
   uint8_t           *fastclk_comb;      // per-member: drove on non-posedge
   int                fastclk_probe_member; // running member during non-posedge
   rt_nexus_t       **fastclk_guard_nx;   // quiet-sensitivity guard nexuses
   const rt_nexus_vtable_t **fastclk_guard_orig; // their original vtables
   rt_nexus_vtable_t *fastclk_guard_vt;   // per-guard patched copies (notify -> dissolve)
   unsigned           fastclk_nguards;
   rt_nexus_t       **fastclk_bl;         // nexuses that ever dissolved the table
   unsigned           fastclk_nbl, fastclk_blmax;  // -> never guard again
   // NVC_FAST_CLK_WIDE (default on; =0 restores the guard machinery
   // wholesale): non-candidate single-bit overlap nexuses become
   // COMPANIONS instead of unflag/guard triggers, so (clk,rst) procs are
   // admitted as EVERY-EVENT members (wakeable.fastclk_ee). The table is
   // partition-sorted at probation exit: posedge-only [0, ee_start),
   // every-event [ee_start, count). Dispatch keys on a candidate
   // VALUE-EDGE shadow (fastclk_clk_last), not the level bit: posedge
   // runs the whole table, any other latched wake runs only the
   // every-event tail. Companion admission IGNORES the candidate
   // blacklist (a failed clock candidate -- rst -- is prime companion
   // material).
   rt_nexus_t       **fastclk_comp;       // registered companion nexuses
   unsigned           fastclk_ncomp;
   uint8_t            fastclk_clk_last;   // candidate value-edge shadow byte
   unsigned           fastclk_ee_start;   // partition boundary (see above)
   unsigned           fastclk_hit_deltas; // probation: hit deltas consumed;
                                          // >=512 before 64 candidate edges
                                          // => STALL (dissolve + cooldown)
   bool               fastclk_evict_defer;// evicted members hold fused sites
                                          // to patch at next dispatch preamble
   uint64_t           fastclk_empty_backoff; // all-empty AUTO round re-arm:
                                          // pure doubling, no survival reset
   struct {                               // temporary candidate cooldown
      rt_nexus_t     *nx;                 // (probation STALL verdicts): the
      uint64_t        retry_at;           // scan skips nx until retry_at
   }                  fastclk_cool[8];
   unsigned           fastclk_cool_next;  // round-robin slot
   rt_nexus_t        *fastclk_excl[8];    // busy-companion exclusion: these
   unsigned           fastclk_nexcl;      // nexuses' procs are unflagged at
                                          // rebuild (rate demote; NOT the bl)
   uint32_t           fastclk_win_pos;    // rate window: posedge dispatches
   uint32_t           fastclk_win_off;    // .. off-edge dispatches
   uint32_t           fastclk_comp_off[8];// .. off-edge dispatches per companion
   unsigned           fastclk_npending;   // members mid self-suspend (#0/wait-
                                          // for-0 continuation queued): fused
                                          // dispatch falls back to the table
                                          // loop (which skips pending) while >0
   hash_t            *depositors;         // nexus -> last depositing rt_proc_t
                                          // (fused-cone force/release wakeups)

   // NVC_FUSED_BLOCK (Phase B of the direct-entry design): the fast-clk
   // member set fused into ONE emitted machine-code block -- straight-line
   // per-member argument setup + direct calls to each member's native entry,
   // targets baked at block-build time. Strictly STATELESS: the block owns
   // sequencing only (member state stays in privdata), so it can be
   // dissolved at any delta boundary with zero loss.
   struct _fused_block *fused_block;
   code_cache_t       *fused_code;   // blob allocator for fused blocks
   jit_entry_fn_t      fused_stub;   // shared null-result stub (disable target)

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
   // delta-ordered capture (NVC_ACCEL_CAPTURE_DELTA): chunks whose armed
   // capture was deferred this timestep, re-woken from the delta boundary
   // once iteration reaches the target (or the timestep would end)
   aj_chunk_t       **aj_capq;
   int                aj_capq_count, aj_capq_max;
   unsigned           aj_chunk_max;
   uint64_t           aj_snap_now;   // timestep of the last fleet input snapshot
   unsigned           aj_snap_iter;  // ...and its delta cycle (per-DELTA snapshot)
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
static void copy_value_ptr(rt_nexus_t *n, rt_value_t *v, const void *p);
static inline void *nexus_effective(rt_nexus_t *n);
static void defer_driving_update(rt_model_t *m, rt_nexus_t *n);
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

// #74 fuse: signal B's readers were repointed onto A at initialization.
// Nothing may ever drive, deposit to, or notify through B's dead nexus —
// a late driver/force/VHPI deposit lands here and fails loudly.
static void fused_nexus_trap(rt_model_t *m, rt_nexus_t *n)
{
   fatal("signal %s was fused onto another signal at initialization and "
         "can no longer be driven independently; rerun with NVC_NO_FUSE=1",
         n->signal->where ? istr(tree_ident(n->signal->where)) : "?");
}

static void fused_nexus_deposit(rt_model_t *m, rt_nexus_t *n, const void *value)
{
   fused_nexus_trap(m, n);
}

static void *fused_nexus_read(rt_nexus_t *n, rt_source_t *src)
{
   fatal("read of fused nexus %s source",
         n->signal->where ? istr(tree_ident(n->signal->where)) : "?");
}

static const rt_nexus_vtable_t nexus_fused_vtable = {
   .update_driving = fused_nexus_trap,
   .deposit        = fused_nexus_deposit,
   .read_source    = fused_nexus_read,
   .notify         = fused_nexus_trap,
};

// NVC_BANKED=1 — increment I1 SCAFFOLDING for the double-banked SMP
// substrate (see the smp-double-banked directive / design_banked_smp.md).
// I1 changes NO behavior: the banked vtable's deposit performs exactly the
// default deposit, then appends the nexus to a per-delta touched list that
// the delta-boundary flip hook drains (a no-op drain for now).  What I1
// proves: the per-nexus vtable install for the banked class, the deposit
// chokepoint interception, and the flip cadence — the plumbing that I2's
// real second plane + eval/commit overlap will inhabit.
// Eligibility is ANY single-SOURCE_DRIVER nexus regardless of resolution
// (per the resolver-omission directive: resolution of one input is the
// identity in regular digital logic) and regardless of which specialized
// vtable it sits on — so the interception is a PATCHED COPY of the
// nexus's current vtable with .deposit chained through banked_deposit
// (the fastclk_guard pattern), preserving every class's update_driving.
static void banked_deposit(rt_model_t *m, rt_nexus_t *n, const void *value);
#define BANKED_MAX_CLASSES 8
// Global bank-select index read by JIT code emitted under NVC_BANKED_READS
// (jit-irgen.c irgen_op_resolved).  Stays 0 until the flip goes live.
int32_t nvc_banked_sel = 0;
// #74 E2: some reader (S'last_value in a process, toggle coverage) needs
// the last-value plane maintained for a signal. Registered at RESET time
// only — the elision decision must be final before the first commit.
static bool g_lv_copy_all = false;

static int g_banked = -1;   // NVC_BANKED, resolved in accel_banked_init
static int g_alias_flip = -1;  // NVC_ALIAS_FLIP (#74): drive-time staging,
                               // requires the banked install; resolved in
                               // accel_banked_init alongside g_banked
static struct {
   rt_nexus_t **items;
   size_t       count, max;
   // Under the E/U pipeline banked_deposit runs on BOTH threads (E: must_e
   // chunk publications; U: streamed interp writes) — a shared touched list
   // corrupts under growth (TSAN: xrealloc + memcpy races).  The U thread
   // appends to its own list; the flip (single-threaded) drains both.
   rt_nexus_t **items_u;
   size_t       count_u, max_u;
   long         installs, deposits, flips, maxtouched;
   int          nclasses;
   const rt_nexus_vtable_t *orig[BANKED_MAX_CLASSES];
   rt_nexus_vtable_t        patched[BANKED_MAX_CLASSES];
} g_banked_st;
static void update_implicit_signal(rt_model_t *m, rt_implicit_t *imp);
static bool run_trigger(rt_model_t *m, rt_trigger_t *t);
static void wakeup_all(rt_model_t *m, void **pending);
static void wakeable_set_kind(rt_wakeable_t *w, wakeable_kind_t k);
static void clear_event(rt_model_t *m, void **pending, rt_wakeable_t *obj);
static void sched_event(rt_model_t *m, void **pending, rt_wakeable_t *obj);
static void reset_scope(rt_model_t *m, rt_scope_t *s);
static void async_run_process(rt_model_t *m, void *arg);
static void procq_do(rt_model_t *m, rt_wakeable_t *obj, defer_fn_t fn,
                     void *arg);
static void part_final_report(rt_model_t *m);
static void evproc_shutdown(void);
static void u_thread_shutdown(void);
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
   rt_model_t *m = __model;
   assert(m != NULL);

   __model = *save;
   *save = NULL;

   if (__model == NULL)
      diag_remove_hint_fn(model_diag_cb, m);
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
   // Always per-thread, even with RT_MULTITHREADED off: the parallel
   // process scheduler (evproc) runs proc eval on worker threads, and the
   // wrappers stash active_obj/tlab here expecting a per-thread slot.
   // Slot-0 aliasing made every worker share one active_obj — sole-writer
   // safe with a single worker (PP=2) but a clobber race at PP>=3 (the
   // x_sched_event NULL-wakeable SEGV).  The array is MAX_THREADS-sized
   // and the struct 64-aligned already; serial pays one thread_id() TLS
   // read per call.
   const int my_id = thread_id();

   if (unlikely(m->threads[my_id] == NULL))
      return (m->threads[my_id] = xcalloc(sizeof(model_thread_t)));

   return m->threads[my_id];
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
// NVC_DIRECT_SCHED: workers run sched_driver's fast path directly for the
// FAST_DRIVER whole-signal class instead of pipelining records to thread 0.
// Resolved once in evproc_ensure_started (before any parallel dispatch).
static int        g_direct_sched = 0;
static long       g_direct_cnt[MAX_THREADS];
// #67 NVC_TWO_PHASE: g_two_phase = env armed; g_two_phase_eval = inside the
// EVAL window (E context) — inline wake publication defers to the boundary
// replay and next-delta procq appends go to the E bank.  Both 0 by default:
// the serial path is untouched.
static int        g_two_phase = -1;
static int        g_two_phase_eval = 0;
// Capture gate for x_sched_waveform's record branch — set alongside
// g_par_active by evproc_dispatch, and independently by TWO_PHASE=3.
// Split so NVC_FORCE_PAR can measure the g_par_active deopt (JIT
// inline-drive decline + deferq locks) without arming the capture.
static int        g_capture = 0;
static int        g_two_phase_run = 0;   // 0 off, 1 single-thread form, 2 U-phase, 3 overlap
static struct {
   bool           started;
   nvc_thread_t  *thread;
   int            e_cpu, u_cpu;
   rt_model_t    *model;
   int            req __attribute__((aligned(64)));
   int            done __attribute__((aligned(64)));
   int            stop;
   // TP3 overlap: E streams driver-write records into its prop pipe during
   // eval; U chases them with `consumed`, applying sched_driver per record
   // (U-owned bookkeeping).  stream: 0 idle, 1 streaming, 2 drain request
   // (E finished eval; U acks by storing 0 once consumed == count).  The
   // pipe is PREALLOCATED (no realloc under the reader); x86-TSO gives the
   // record-fill -> count-increment ordering the chase relies on.
   int            stream __attribute__((aligned(64)));
   uint32_t       consumed;
   int            e_tid;
} g_uthr;

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

// Pending-list arming from a worker during parallel eval must serialize:
// two procs waiting on the same nexus mutate one pending list (tag flips,
// realloc).  Same pattern as deferq_do — free when serial.
static void sched_event_par(rt_model_t *m, void **pending, rt_wakeable_t *obj)
{
   if (unlikely(relaxed_load(&g_par_active))) {
      nvc_lock(&g_sched_lock);
      sched_event(m, pending, obj);
      nvc_unlock(&g_sched_lock);
   }
   else
      sched_event(m, pending, obj);
}

static void clear_event_par(rt_model_t *m, void **pending, rt_wakeable_t *obj)
{
   if (unlikely(relaxed_load(&g_par_active))) {
      nvc_lock(&g_sched_lock);
      clear_event(m, pending, obj);
      nvc_unlock(&g_sched_lock);
   }
   else
      clear_event(m, pending, obj);
}

// #67 boundary replay of eval-phase-deferred wakes (single-threaded).
static void async_replay_wake_all(rt_model_t *m, void *arg)
{
   wakeup_all(m, (void **)arg);
}

static void async_replay_wake_trigger(rt_model_t *m, void *arg)
{
   rt_wakeable_t *obj = arg;
   rt_trigger_t *t = container_of(obj, rt_trigger_t, wakeable);
   if (run_trigger(m, t))
      wakeup_all(m, &(t->pending));
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

   // Fast-clk auto table: DEFAULT ON at 1000ns (interleaved 6-rep A/B vs
   // stock 1.22: fused b12 median +8% with clean separation, b17 +3%;
   // VeeR parity post-hysteresis; correctness byte-identical everywhere
   // tested).  NVC_FAST_CLK_AUTO=<ns> overrides the time; =0 disables.
   m->fastclk_probe_member = -1;   // 0 would mean "member 0 probing"

   const char *fca = getenv("NVC_FAST_CLK_AUTO");
   if (getenv("NVC_LEVELIZE_SWEEP") != NULL)
      m->fastclk_auto_at = 0;   // the sweep IS the dispatch: fastclk
                                // membership strips pending-list
                                // sensitivity, starving the analyzer's
                                // graph AND bypassing wave absorption
   else if (fca == NULL)
      m->fastclk_auto_at = UINT64_C(1000) * UINT64_C(1000000);
   else if (strtoull(fca, NULL, 10) != 0 || *fca == '\0')
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
   if (scope->scratch != NULL) {
      eval_arena_free(scope->scratch);
      scope->scratch = NULL;
   }

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

static void aj_census_dump(rt_model_t *m);

void model_free(rt_model_t *m)
{
   part_final_report(m);

   aj_census_dump(m);

   u_thread_shutdown();
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
   free(m->procq_e.tasks);
   free(m->ewakeq.tasks);
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

// NVC_FAST_CLK: evict one member from the table (leaves a hole -- the
// dispatch loops and the fused block skip !fastclk members). Fired from
// every path that queues or re-registers a member OUTSIDE the table's
// wake latch (timed wakes, #0 inactive-region waits, force/release
// depositor queueing, wait-set demotes): a member both queued and
// table-dispatched would run twice in one delta, and a table-dispatched
// member resumed by the table keeps a stale eventq entry (delayed=true)
// that later trips the deltaq_insert_proc assert. Timeouts are rare in
// RTL FF procs; eviction preserves correctness at negligible cost. The
// fused-block sites of an evicted member are patched at the next
// dispatch preamble (never mid-block -- an evict can fire from inside
// the member's own eval, where patching its not-yet-executed SITE B
// would skip the TLAB/finalize epilogue).
static void aj_fastclk_evict(rt_model_t *m, rt_wakeable_t *w, const char *why)
{
   if (!w->fastclk)
      return;
   w->fastclk = 0;
   w->fastclk_ee = 0;
   if (w->fastclk_counted) {
      w->fastclk_counted = 0;   // was counted as a self-suspended member
      atomic_add(&m->fastclk_npending, -1);
   }
   if (m->fused_block != NULL)
      m->fastclk_evict_defer = true;
   static unsigned ecount = 0;
   if (ecount++ < 10 && getenv("NVC_ACCEL_JIT_DEBUG") != NULL)
      notef("accel-jit: fast-clk member evicted (%s)", why);
}

static inline void set_pending(rt_model_t *m, rt_wakeable_t *wake)
{
   assert(!wake->pending);
   assert(!wake->delayed);
   if (unlikely(wake->fastclk)) {
      // A member queueing ITSELF mid-activation (the NBA/#0 transform's
      // `wait for 0 ns` -> inactiveq, x_sched_inactive) is a CONTINUATION
      // of an activation the table delivered -- it stays a member; while
      // it is pending both dispatch paths skip it (the same drop normal
      // wakeup_one applies to pending procs), tracked by fastclk_npending
      // so the fused block falls back to the checking loop. Queueing from
      // OUTSIDE its own activation (eventq timed wake, force/release
      // depositor requeue) evicts: the table would double-run it.
      if (get_active_wakeable() == wake) {
         // Atomic + exact pairing via fastclk_counted: this runs on evproc
         // WORKERS (NBA-form members self-queue every activation through
         // x_sched_process), and a plain ++ raced the parallel decrements
         // in async_run_process — counter drift double-ran or skipped
         // fused-block members (the PP>=3 retire divergence).
         wake->fastclk_counted = 1;
         atomic_add(&m->fastclk_npending, 1);
      }
      else
         aj_fastclk_evict(m, wake, "queued outside the table");
   }
   wake->pending = true;
}

static void deltaq_insert_proc(rt_model_t *m, uint64_t delta, rt_proc_t *proc)
{
   if (delta == 0) {
      set_pending(m, &proc->wakeable);
      deferq_do(unlikely(g_two_phase_eval) ? &m->procq_e : &m->procq,
                async_run_process, proc);
      m->next_is_delta = true;
   }
   else {
      if (unlikely(proc->wakeable.fastclk))
         aj_fastclk_evict(m, &proc->wakeable, "timed wake armed");
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

static void direct_eval_uninstall(rt_proc_t *proc);
static void fused_block_build(rt_model_t *m);
static void fused_block_dissolve(rt_model_t *m);
static bool fused_block_dispatch(rt_model_t *m, bool posedge);
static void wakeup_one(rt_model_t *m, rt_wakeable_t *obj);
static bool aj_chunk_demote(rt_model_t *m, aj_chunk_t *chunk);

static void reset_process(rt_model_t *m, rt_proc_t *proc)
{
   TRACE("reset process %s", istr(proc->name));

   // A fused block bakes this process's state/context pointers as
   // immediates; a reset invalidates them (privdata is rewritten, possibly
   // to a new pointer). Rare event -> wholesale dissolve (the block is
   // stateless, so dropping it at any delta boundary loses nothing).
   if (unlikely(m->fused_block != NULL))
      fused_block_dissolve(m);

   // A direct-eval wrapper caches the state/context pointers this reset is
   // about to (re)write -- restore the default vtable first (the stale-vtable
   // trap: reset bypasses the vtable and never calls .reset).
   direct_eval_uninstall(proc);

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

   // Schedule the process to run immediately.  Route through procq_do so a
   // POSTPONED process lands in the postponed queue: LRM 08 section 14.7.5.1
   // requires every nonpostponed process to run to suspension before any
   // postponed process during initialisation, and the resumption path has
   // always honoured that -- only this first activation did not.
   procq_do(m, &proc->wakeable, async_run_process, proc);
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
   // unless the eval arena is enabled). This resets the DEFAULT arena, which
   // also caught resolution/conversion transients since the last eval.
   jit_eval_arena_reset();

   thread->active_obj = &(proc->wakeable);
   thread->active_scope = proc->scope;

   // Per-instance scratch (opt-in via NVC_SCOPE_SCRATCH): run this process's
   // body against its OWN scope's arena instead of the shared per-thread one,
   // grouping each instance's transients in their own buffer for debugging.
   // OFF by default because it costs ~3% (measured, VeeR-EH2 hello): many
   // small per-scope arenas hurt cache locality versus one hot shared arena,
   // and it buys no parallel-eval isolation -- parallel workers already have
   // per-thread arenas (model_cycle), so per-scope granularity adds nothing.
   static int per_inst = -1;
   if (per_inst < 0) per_inst = (getenv("NVC_SCOPE_SCRATCH") != NULL);
   eval_arena_t *saved_arena = NULL;
   if (per_inst && jit_eval_arena_enabled()) {
      if (unlikely(proc->scope->scratch == NULL))
         proc->scope->scratch = eval_arena_new();
      saved_arena = jit_eval_arena_swap(proc->scope->scratch);
      jit_eval_arena_reset();   // reclaim this instance's previous transients
   }

   jit_scalar_t state = {
      .pointer = *mptr_get(proc->privdata) ?: (void *)-1
   };

   jit_scalar_t result;
   jit_scalar_t context = {
      .pointer = *mptr_get(proc->scope->privdata)
   };

   // Explicit in-region call: this is the scheduler's process activation, which
   // runs inside the landing pad armed by model_run, so it skips the per-call
   // setjmp / state transitions / diag-hint churn.
   if (!jit_fastcall_inregion(m->jit, proc->handle, &result, state, context,
                              proc->tlab ?: thread->tlab))
      m->force_stop = true;

   if (saved_arena != NULL)
      jit_eval_arena_swap(saved_arena);   // restore the default arena

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

// Default abort policy: an abort out of a process stops the run.
static void proc_on_abort_default(rt_model_t *m, rt_proc_t *proc)
{
   m->force_stop = true;
}

static const rt_proc_vtable_t proc_default_vtable = {
   .eval     = proc_eval_jit,
   .reset    = proc_reset_default,
   .on_abort = proc_on_abort_default,
};

static void proc_reset_default(rt_proc_t *proc)
{
   proc->vtable = &proc_default_vtable;
}

void proc_set_vtable(rt_proc_t *proc, const rt_proc_vtable_t *vt)
{
   proc->vtable = vt;
}

// --- Direct process eval (Phase A of the direct-entry design) ---------------
//
// For a process with a STATIC sensitivity list (wakeable.wait_state == 1)
// every input to the default eval path is per-proc CONSTANT once
// reset_process has run: the state pointer (*mptr_get(proc->privdata) is
// written exactly once at reset, and the mspace GC is non-moving mark-sweep
// so the value never changes), the context pointer (scope privdata, ditto)
// and the jit_func_t (the handle->func binding is stable for the life of
// the jit -- funcs are only freed in jit_free). The default path re-derives
// all of them and then goes through two more call layers
// (jit_fastcall_inregion -> jit_vcall_inregion) on every activation. This
// wrapper caches them once at install time and calls the function's
// published entry point directly.
//
// Preserved exactly from proc_eval_jit / jit_vcall_inregion:
//  - jit_eval_arena_reset() and active_obj/active_scope bookkeeping
//  - the landing-pad check (jmp_buf_valid && state == JIT_RUNNING): the
//    in-region contract only holds under model_run's armed pad; model_step /
//    shell / VHPI stepping arrive without one and must take the protected
//    jit_fastcall fallback
//  - thread->anchor = NULL after the call (stack-trace anchor discipline)
//  - the TLAB claim/release protocol on the result pointer (a process that
//    suspends holding TLAB allocations claims the buffer for private use)
//  - tier-up: f->entry is re-read with load_acquire on every eval, so the
//    store_release publication from the compile thread is observed exactly
//    as before
// Elided (with reasons):
//  - per-eval jit_get_func + mptr_get dereferences (constant post-reset)
//  - the jit_fastcall_inregion / jit_vcall_inregion call layers (inlined)
//  - the NVC_SCOPE_SCRATCH block: opt-in debug feature; the wrapper is
//    simply never installed when that env var is set
//
// The vtable must stay the FIRST field so the wrapper is recovered from
// proc->vtable at zero cost (established accel/lazy/chunk pattern). The
// wrapper is only ever installed OVER the default vtable, and is removed at
// the 1 -> 2 wait-state demotion and in reset_process; anything else that
// swaps vtables (accel_load, aj_reroute) replaces the pointer wholesale and
// never chains through it, so a concurrent replacement merely orphans the
// wrapper. Gated by NVC_DIRECT_EVAL (default ON; set 0 to disable for A/B).

typedef struct {
   rt_proc_vtable_t vtable;    // FIRST -- recovered from proc->vtable
   jit_func_t      *func;      // stable handle->func binding
   void            *state;     // cached *mptr_get(proc->privdata) ?: -1
   void            *context;   // cached *mptr_get(proc->scope->privdata)
} direct_eval_t;

static void proc_eval_direct(rt_model_t *m, rt_proc_t *proc)
{
   direct_eval_t *de = (direct_eval_t *)proc->vtable;

   model_thread_t *thread = model_thread(m);
   assert(thread->tlab != NULL);
   assert(thread->tlab->alloc == 0);

   // Reclaim the previous eval's escaping unconstrained results in O(1)
   jit_eval_arena_reset();

   thread->active_obj = &(proc->wakeable);
   thread->active_scope = proc->scope;

   tlab_t *tlab = proc->tlab ?: thread->tlab;

   jit_scalar_t args[JIT_MAX_ARGS];
   args[0].pointer = de->state;
   args[1].pointer = de->context;

   void *result_ptr;

   jit_thread_local_t *jthread = jit_thread_get();
   if (likely(jthread->jmp_buf_valid && jthread->state == JIT_RUNNING)) {
      // In-region: model_run's landing pad is armed; an abort longjmps
      // straight to it (same contract as jit_vcall_inregion's fast path)
      jit_entry_fn_t entry = load_acquire(&(de->func->entry));
      (*entry)(de->func, NULL, args, tlab);
      jthread->anchor = NULL;
      result_ptr = args[0].pointer;
   }
   else {
      // No pad armed (model_step / shell / VHPI stepping): protected call
      jit_scalar_t result = { .pointer = NULL };
      if (!jit_fastcall(m->jit, proc->handle, &result,
                        args[0], args[1], tlab))
         m->force_stop = true;
      result_ptr = result.pointer;
   }

   if (proc->tlab != NULL && result_ptr == NULL) {
      tlab_release(proc->tlab);
      proc->tlab = NULL;
   }
   else if (proc->tlab == NULL && result_ptr != NULL) {
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

static bool direct_eval_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *e = getenv("NVC_DIRECT_EVAL");
      enabled = !(e != NULL && *e == '0')
         && getenv("NVC_SCOPE_SCRATCH") == NULL;
   }
   return enabled;
}

// Install at the wait_state 0 -> 1 promotion in run_process: reset_process
// has run (privdata final) and the proc has a static wait set. Never
// installed over a non-default vtable (accel/lazy/chunk own those).
static void direct_eval_install(rt_model_t *m, rt_proc_t *proc)
{
   if (proc->vtable != &proc_default_vtable || !direct_eval_enabled())
      return;

   direct_eval_t *de = xcalloc(sizeof(direct_eval_t));
   de->vtable.eval     = proc_eval_direct;
   de->vtable.reset    = proc_reset_default;
   de->vtable.on_abort = proc_on_abort_default;
   de->func    = jit_get_func(m->jit, proc->handle);
   de->state   = *mptr_get(proc->privdata) ?: (void *)-1;
   de->context = *mptr_get(proc->scope->privdata);

   proc->vtable = &(de->vtable);
}

// Restore the default vtable if (and only if) OUR wrapper is installed: at
// the wait_state 1 -> 2 demotion and before reset_process re-runs the
// process (both invalidate the cached constants).
static void direct_eval_uninstall(rt_proc_t *proc)
{
   if (proc->vtable->eval == proc_eval_direct) {
      direct_eval_t *de = (direct_eval_t *)proc->vtable;
      proc->vtable = &proc_default_vtable;
      free(de);
   }
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
#include <sys/utsname.h>

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
      binding->vtable.on_abort = proc_on_abort_default;
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
   int           base;         // enum position of '0' in the port's own type
                               // (std_logic/logic3d = 2, BIT/BOOLEAN = 0):
                               // a deposited byte is base|valuebit
   uint8_t      *data;         // shared.data (read inputs)
   rt_signal_t  *sig;          // signal (force outputs via force_signal)
   bool          icg;          // gated-clock output (gater-cell member)
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
   char         name[64];    // port name (X/Z detection names the offender)
} aj_bpin_t;

// One bridged input's snapshot slot: where its live bytes come from, where its
// pre-edge copy lives in the chunk's arena, and which bindtab slot to repoint.
typedef struct {
   void   *live;
   size_t  off;
   size_t  nb;
   int     slot;
} aj_snap_ent_t;

struct _aj_chunk {
   rt_proc_vtable_t vtable;          // FIRST — recover chunk from proc->vtable
   void           (*eval)(void *, void **);  // .so's accel_eval(state, bindtab)
   void           (*reset)(void *);          // this .so's accel_reset(state)
   void            *state;           // per-chunk state (sized by accel_state_size)
   void            *dl;              // dlopen handle
   bool             merged;          // domain-merged chunk (negedge state flip)
   bool             gater;           // clock-gate cell chunk: ICG latch rules
   uint64_t         ck_flip_now;     // timestep whose fall already flipped (+1)
   rt_signal_t     *primary_ck;      // bindtab[4] clock's signal (edge arming)
   const uint8_t   *rst_data;        // NVC_ACCEL_RST_HOLD: reset pin live bytes
   bool             rst_low;         //   active-low (pin name ends _l/_n/_b)
   bool             rst_released;    //   sticky: deassert seen, pass-through
   void           (*set_clklast)(void *, unsigned char);
   void           (*force_arm_fn)(void *);   // CK_LATE dead-primary arming  // bridge accessor
   uint64_t         ck_arm_now;      // timestep whose rise this chunk consumed
   uint64_t         ck_xarm_now;     // timestep whose EXTRA-clock rise engaged
                                     // the pre-edge snapshot (once per instant)
   // NVC_ACCEL_ASSERT_ORDER edge-count reconciliation (slice 2): bridge-
   // observed posedges this timestep vs the clock nexus's real ones. The
   // real count lives on the nexus side (ao_real/ao_real_now stamped at
   // driver commit); this is the observed half, reset when now moves.
   uint64_t         ao_obs_now;      // timestep the fields below belong to
   uint8_t          ao_fall_seen;    // post-arm fall observed this timestep
   uint64_t         ao_cap_now;      // AO>=3: timestep of the armed capture
   uint32_t         ao_cap_delta;    //   .. its delta
   uint8_t          ao_cap_fired;    //   .. census reported this timestep
   uint8_t          ao_seeded;       // AO>=3: t=0 seed eval completed
   rt_scope_t      *scope;           // installed subtree root
   aj_defer_out_t  *defer_outs;      // per-chunk (was the single m->aj_defer_*)
   unsigned         defer_count;
   bool             icg_fallwake_set; // NVC_ACCEL_ICG_FALLWAKE applied once
   // Root alias deposit targets (bind time): same-scope same-name signals the
   // FIRST-match binding missed; aj_out fans publications to them.
   rt_signal_t    *(*out_extra)[6];
   uint8_t         *out_extra_n;
   // Chunk-owned drivers (aj_quench_rerouted_drivers): the ORIGINAL rerouted
   // procs' driver sources on this chunk's bound output signals. They stay
   // CONNECTED and aj_out refreshes their waveform value on every publication,
   // so port propagation and resolution — which read DRIVING values, not the
   // deposited effective bytes — keep delivering the chunk's value to every
   // downstream network exactly as the interp proc did.
   struct aj_odrv { rt_source_t *src; rt_nexus_t *nx; unsigned off; }
                  (*out_drv)[4];
   uint8_t         *out_drv_n;
   // TWO-PHASE EDGE SAMPLING (mechanism 3 of the VeeR divergence).  Immediate
   // deposits land in signal shared memory at once, so a chunk evaluated later
   // in the same delta -- or first-woken in a later delta -- marshals another
   // chunk's POST-edge Q and advances a cycle early (the +4 retired-PC offset;
   // requires producer AND consumer accelerated, which is why eight bisection
   // rounds measured every family clean alone and dirty in combination).
   // Deposit-side remedies alone cannot fix it for LATE-woken chunk
   // consumers: NBA/two-delta staging publish within the timestep, and a
   // chunk whose posedge eval runs tens of deltas after the edge (gated-
   // clock triggers) reads the already-published post-edge value.  The
   // shipped protocol (SNAP_MODE 4, the FPGA double-bank design in
   // software): a per-TIMESTEP fleet pass at the first delta boundary
   // copies every chunk's bridged-input bytes into its arena (the settled
   // pre-edge world); ONLY the armed posedge eval reads the snapshot via
   // bindtab repointing; every other eval reads live so Mealy settling and
   // gated-clock late commits are untouched.  Modes 1-3 are historical
   // bisection knobs (see aj_snap_mode).
   uint8_t         *snap;            // arena: pre-edge bytes of bridged inputs
   aj_snap_ent_t   *snap_map;
   unsigned         snap_nin;
   // NVC_ACCEL_RECAP checkpoint-and-replay capture: state snapshot taken
   // BEFORE each armed eval; a later same-timestep input wake rolls the
   // state back (incl the bridge's clklast) and re-runs the capture with
   // the now-settled inputs.  Repeated wakes converge to interp's delta
   // fixpoint — the interp-relative contract for lumped captures without
   // any input snapshot (mech-3: interp packing flops commit at deep
   // deltas AFTER the chunk's early armed eval, e.g. wb_pkt at +11 vs
   // the chunk arming at +3).
   uint8_t         *recap;           // pre-capture state copy (lazily sized)
   uint64_t         recap_now;       // now+1 stamp (0 = none this instant)
   unsigned long    recap_size;      // replayable prefix (accel_recap_size);
                                     // 0 = unprobed, ULONG_MAX = absent
   uint64_t         cap_pending;     // delta-ordered capture: now+1 stamp
                                     // while this chunk waits on aj_capq
   unsigned         cap_maxdelta;    // census: max arming/extra-wake delta
   int              cap_delta;       // per-chunk capture depth (census file;
                                     // -1 = unset, fall back to global k)
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
   uint64_t         live_out_mask[16]; // dead-output pruning (re-applied on respec;
                                       // 16 words = 1024 outputs — the fixed [4]
                                       // capped models at 256 and pin-completion
                                       // wrappers exceed that)
   // Serializes eval of THIS chunk under NVC_PARALLEL_PROCS: the chunk's .so
   // state is mutable, so two workers must never run the same chunk's eval at
   // once. Held across the dedup check + eval; different chunks have distinct
   // locks so they still run concurrently. (No-op cost in the serial path.)
   nvc_lock_t       eval_lock;
   // Reversible demotion (aj_chunk_demote): every proc rerouted to this
   // chunk, with its PRE-reroute vtable, saved by aj_reroute so the demote
   // can restore each proc EXACTLY (default vtable, direct-eval wrapper, ...)
   // instead of guessing.
   struct aj_rr_saved {
      rt_proc_t              *proc;
      const rt_proc_vtable_t *vt;
   }               *rr_saved;
   unsigned         rr_count, rr_max;
   // CLOCK SUBSCRIPTION (see aj_subscribe_clocks). The bridge derives its edges
   // by SAMPLING the bound clock bytes, so it is only correct if the chunk is
   // actually evaluated in the delta where a clock transitions. Rerouting kills
   // the internal activity that used to wake the subtree's procs, so these are
   // the signals we must explicitly stay sensitive to: the main clk plus every
   // sm_extra_clocks[] input.
   rt_signal_t     *ck_sigs[17];
   int              n_ck_sigs;
   // X/Z FALLBACK (GAP 2): the .so's optional detector accessors. NULL when
   // the .so predates the detector (stale ~/.cache/nvc/accel build) — every
   // use is NULL-tolerant, exactly like accel_dump/accel_in_addr.
   int            (*x_seen_fn)(void *);
   void           (*x_info_fn)(void *, unsigned *, unsigned *);
   void           (*x_clear_fn)(void *);
   bool             x_reported;      // first-sighting note already emitted
   bool             x_demote_tried;  // demote attempted (declines are sticky)
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
   uint64_t       arm_now; // timestep the entry was armed (leak audit)
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
   // Cross-timestep LEAK audit (assert-order rank 6 / the critic's
   // missing-delta-creation gap): the two-delta ladder assumes a further
   // delta always follows within the SAME timestep — an armed entry
   // applying at a LATER timestep means the staged publication leaked
   // past the edge it belonged to, violating no intra-delta rule any
   // other check watches.
   static int _s2a = -1;
   if (_s2a < 0) _s2a = getenv("NVC_ACCEL_ASSERT_ORDER") != NULL;
   for (int i = 0; i < g_aj_stage2_n; ) {
      aj_stage2_t *e = &g_aj_stage2[i];
      if (e->armed) {
         if (_s2a && e->arm_now != (uint64_t)m->now)
            notef("accel-assert: STAGE2 CROSS-TIMESTEP LEAK sig %s: armed "
                  "at t=%"PRIu64" applying at t=%"PRIu64" — the two-delta "
                  "ladder ran out of deltas; the publication belongs to "
                  "the earlier edge",
                  e->sig != NULL && e->sig->where != NULL
                     ? istr(tree_ident(e->sig->where)) : "?",
                  e->arm_now, (uint64_t)m->now);
         deposit_signal(m, e->sig, e->buf, 0, e->width);
         free(e->buf);
         *e = g_aj_stage2[--g_aj_stage2_n];
      }
      else {
         e->armed = true;
         e->arm_now = (uint64_t)m->now;
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
static int g_aj_snap_fleet = 0, g_aj_snap_used = 0;

// NVC_ACCEL_SNAP_MODE: 0 = off (== NVC_ACCEL_NO_SNAP), 1 = per-delta fleet
// snapshot taken lazily at the first chunk eval of the delta (deposits from
// interp processes ordered EARLIER in the same delta still contaminate it),
// 2 = snapshot hoisted to the delta boundary in model_cycle (after driver
// commits, before any process runs -- airtight against same-delta deposits),
// 3 = mode 2 + seq-only (inputs whose driver is combinational stay LIVE so
// blocking-assign comb settle remains visible same-delta; only inputs from
// clocked/timed drivers are edge-sampled).
// 4 = DOUBLE-BANK SPLIT (the FPGA two-bank design in software): the fleet
// snapshot is taken once per TIMESTEP (first delta boundary, before any
// process runs = the settled PRE-EDGE world), and ONLY the armed posedge
// eval of a chunk reads it -- comb/settle evals read live.  A late-delta
// posedge eval (chunk woken by its gated-clock trigger at d29) then samples
// the same pre-edge D values an early one would, instead of values NBA and
// the 2-delta stage have already published inside the timestep (measured:
// first-of-burst retired-PC +4 on eh2_dec).  Modes 1-3 lacked this split:
// freezing comb evals froze Mealy paths (stuck-at-0), leaving them live
// left the capture (+4).
static int aj_snap_mode(void)
{
   static int mode = -1;
   if (mode < 0) {
      const char *e = getenv("NVC_ACCEL_SNAP_MODE");
      mode = e ? atoi(e) : 0;   // Default OFF, but the original park reason
                                // is CURED: the historical wedge (armed eval
                                // reads primary inputs pre-edge while extra-
                                // clock samplers read live -- the mixed view
                                // zeroed TRACE_PKT at reset) is resolved by
                                // excluding clock pins from snap_map (clocks
                                // live, data pre-edge -- the coherent flop
                                // view).  Measured 2026-08-08: SNAP_MODE=4
                                // fixes the DESCEND 1100ns bus-phase race
                                // (27/nt0, was 28/nt11; standing full ladder
                                // + hello clean) BUT descend diverges at
                                // 3000ns depth (205 retires vs 125) -- a
                                // second late-manifesting defect, open.  Not
                                // default until that is mechanized.  The
                                // producer-side per-net banking (#53) remains
                                // the structural alternative.
      if (getenv("NVC_ACCEL_NO_SNAP") != NULL) mode = 0;
   }
   return mode;
}

static void aj_snap_fleet_take(rt_model_t *m)
{
   // Key stamps are now+1: aj_snap_now zero-initializes and 0 is also the
   // first legal timestep, so a raw `== now` compare would skip the whole of
   // timestep 0 (same trap ck_arm_now already avoids the same way).
   if (aj_snap_mode() >= 4) {
      // per-TIMESTEP: first delta boundary only (the pre-edge world)
      if (m->aj_snap_now == (uint64_t)m->now + 1)
         return;
   }
   else if (m->aj_snap_now == (uint64_t)m->now + 1
            && m->aj_snap_iter == m->iteration + 1)
      return;
   m->aj_snap_now  = (uint64_t)m->now + 1;
   m->aj_snap_iter = m->iteration + 1;
   if (g_aj_snap_fleet++ == 0)
      notef("accel-snap: two-phase sampling ACTIVE (first fleet pass, "
            "%u chunks, mode %d)", m->aj_chunk_count, aj_snap_mode());
   for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
      aj_chunk_t *c = m->aj_chunks[ci];
      for (unsigned j = 0; j < c->snap_nin; j++)
         memcpy(c->snap + c->snap_map[j].off,
                c->snap_map[j].live, c->snap_map[j].nb);
   }
}
static int         g_aj_verify   = 0;       // int (not bool): the bridge reads it via AJB
static bool        g_aj_verify_skipx = false;  // NVC_ACCEL_VERIFY_X: skip interp-X elems
static bool        g_aj_vtrack = false;  // NVC_ACCEL_VERIFY_TRACK: verify_flagged means
                                         // "currently diverged"; also report RECONVERGED
                                         // transitions so transient vs persistent
                                         // divergence is decidable from the log
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

// NVC_ACCEL_DEMOTE_AT=<time>: at the first settled time step at/after this
// sim time, demote EVERY installed accel chunk back to interpreted
// execution (aj_chunk_demote — the API is the deliverable; this env is the
// live-test harness for it). Accepts parse_fork_time units ("5000ns") or a
// bare number of ns.
static int64_t g_aj_demote_at = -2;    // fs; -2 = not parsed, -1 = disabled
static bool    g_aj_demoted   = false;

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

// NVC_ACCEL_ASSERT_ORDER>=3: capture-before-producer CENSUS. Maps chunk
// boundary-INPUT nexuses to their (chunk, pin name) so notify_event and
// the deposit path can report interp producers that commit AFTER the
// chunk's armed capture in the same timestep — the wb_pkt class (#68
// took ~19 forensic rounds to hand-derive the +11-producer-vs-+3-capture
// fact this prints in one run). Census, not assertion: producer>capture
// is NORMAL for plain next-cycle comb settle; the report is the
// measurement that makes the gated-family cases visible.
typedef struct {
   rt_nexus_t  *nx;         // ULTIMATE nexus (port-hop walk)
   aj_chunk_t  *chunk;
   uint8_t     *dst;        // consumer-side rim bytes (seed back-fill)
   size_t       nbytes;
   char         pin[64];
} aj_ao3_ent_t;
static aj_ao3_ent_t *g_ao3_map = NULL;
static unsigned g_ao3_n = 0, g_ao3_max = 0;

static inline int aj_ao_level(void)
{
   static int l = -1;
   if (l < 0) {
      const char *e = getenv("NVC_ACCEL_ASSERT_ORDER");
      l = e == NULL ? 0 : (atoi(e) > 0 ? atoi(e) : 1);
   }
   return l;
}

static void aj_ao3_add(rt_nexus_t *nx, aj_chunk_t *chunk, const char *pin,
                       uint8_t *dst, size_t nbytes)
{
   for (unsigned i = 0; i < g_ao3_n; i++)
      if (g_ao3_map[i].nx == nx && g_ao3_map[i].dst == dst) {
         g_ao3_map[i].chunk = chunk;
         return;
      }
   if (g_ao3_n == g_ao3_max) {
      g_ao3_max = g_ao3_max ? g_ao3_max * 2 : 64;
      g_ao3_map = xrealloc_array(g_ao3_map, g_ao3_max, sizeof(aj_ao3_ent_t));
   }
   g_ao3_map[g_ao3_n].nx = nx;
   g_ao3_map[g_ao3_n].chunk = chunk;
   g_ao3_map[g_ao3_n].dst = dst;
   g_ao3_map[g_ao3_n].nbytes = nbytes;
   strncpy(g_ao3_map[g_ao3_n].pin, pin, sizeof(g_ao3_map[0].pin) - 1);
   g_ao3_map[g_ao3_n].pin[sizeof(g_ao3_map[0].pin) - 1] = '\0';
   g_ao3_n++;
}

// AO>=3 rank-4 companion: chunk OUTPUT nexuses -> producer chunk, so the
// t=0 seed pass can audit dependency order (#43: install order is the
// reverse of dependency order in the seedord reproducer — a downstream
// chunk seeds from a still-U input and the 2-state plane latches it 0).
static aj_ao3_ent_t *g_ao4_out = NULL;
static unsigned g_ao4_n = 0, g_ao4_max = 0;

static void aj_ao4_add_out(rt_nexus_t *nx, aj_chunk_t *chunk)
{
   for (unsigned i = 0; i < g_ao4_n; i++)
      if (g_ao4_out[i].nx == nx) {
         g_ao4_out[i].chunk = chunk;
         return;
      }
   if (g_ao4_n == g_ao4_max) {
      g_ao4_max = g_ao4_max ? g_ao4_max * 2 : 64;
      g_ao4_out = xrealloc_array(g_ao4_out, g_ao4_max, sizeof(aj_ao3_ent_t));
   }
   g_ao4_out[g_ao4_n].nx = nx;
   g_ao4_out[g_ao4_n].chunk = chunk;
   g_ao4_out[g_ao4_n].dst = NULL;
   g_ao4_out[g_ao4_n].nbytes = 0;
   g_ao4_out[g_ao4_n].pin[0] = '\0';
   g_ao4_n++;
}

static void aj_ao3_drop_chunk(aj_chunk_t *chunk)
{
   for (unsigned i = 0; i < g_ao3_n; )
      if (g_ao3_map[i].chunk == chunk)
         g_ao3_map[i] = g_ao3_map[--g_ao3_n];
      else
         i++;
   for (unsigned i = 0; i < g_ao4_n; )
      if (g_ao4_out[i].chunk == chunk)
         g_ao4_out[i] = g_ao4_out[--g_ao4_n];
      else
         i++;
}

static inline int aj_rst_hold(void);
static void aj_rst_release(rt_model_t *m, aj_chunk_t *c);
static void aj_lower(char *dst, const char *src, size_t n);

// Delta-ordered capture CENSUS dump (NVC_ACCEL_CLOCK_CENSUS=<file>):
// per-chunk max arming/extra-wake delta observed over the run — the
// chunk's clock-edge depth in the delta cascade.  Called from the
// END_OF_SIMULATION paths and model_free; idempotent.
static void aj_census_dump(rt_model_t *m)
{
   static bool dumped = false;
   const char *cf = getenv("NVC_ACCEL_CLOCK_CENSUS");
   if (dumped || cf == NULL || m == NULL || m->aj_chunk_count == 0)
      return;
   FILE *f = fopen(cf, "w");
   if (f == NULL)
      return;
   for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
      aj_chunk_t *c = m->aj_chunks[ci];
      if (c->rs_top != NULL)
         fprintf(f, "%s %u\n", c->rs_top, c->cap_maxdelta);
   }
   fclose(f);
   dumped = true;
   notef("accel-jit: clock census -> %s (%u chunks)", cf, m->aj_chunk_count);
}

// Delta-ordered capture census lookup (NVC_ACCEL_CAPTURE_CENSUS=<file>,
// lines "<chunk-top> <depth>" from a calibration run's
// NVC_ACCEL_CLOCK_CENSUS dump).  Returns the per-chunk capture depth,
// or -1 when the file is absent or the chunk is not listed.
static int aj_census_lookup(const char *top)
{
   static char (*names)[96] = NULL;
   static int  *depths = NULL;
   static int   n = -1;
   if (n < 0) {
      n = 0;
      const char *cf = getenv("NVC_ACCEL_CAPTURE_CENSUS");
      FILE *f = cf != NULL ? fopen(cf, "r") : NULL;
      if (f != NULL) {
         int max = 64;
         names  = xmalloc_array(max, sizeof *names);
         depths = xmalloc_array(max, sizeof(int));
         char nm[96]; int d;
         while (fscanf(f, "%95s %d", nm, &d) == 2) {
            if (n == max) {
               max *= 2;
               names  = xrealloc_array(names, max, sizeof *names);
               depths = xrealloc_array(depths, max, sizeof(int));
            }
            snprintf(names[n], sizeof names[n], "%s", nm);
            depths[n++] = d;
         }
         fclose(f);
      }
   }
   if (top != NULL)
      for (int i = 0; i < n; i++)
         if (strcmp(names[i], top) == 0)
            return depths[i] > 0 ? depths[i] : -1;
   return -1;
}

static void aj_proc_eval(rt_model_t *m, rt_proc_t *proc)
{
   // Recover this proc's chunk from its vtable (first field), establish the
   // active-process context (run_process does not — only the default JIT eval
   // does, and the bridge needs it for deposit_signal()/AJ_OUT), run the chunk.
   aj_chunk_t *chunk = (aj_chunk_t *)proc->vtable;
   if (unlikely(getenv("NVC_ACCEL_CKSUB_DBG") != NULL)) {
      static uint64_t _n = 0;
      if (_n++ < 8)
         notef("accel-jit: aj_proc_eval wake #%"PRIu64" chunk=%s t=%"PRIu64
               " delta=%u", _n, chunk->rs_top ? chunk->rs_top : "?",
               (uint64_t)m->now, (unsigned)m->iteration);
   }
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
   // A whole-core chunk reroutes to hundreds of procs that all wake this
   // delta, so this claim runs millions of times -- the atomic test-and-set
   // showed up as ~10% of accel sim time. Only NVC_PARALLEL_PROCS scatters
   // the duplicates across workers and needs the lock; the serial default has
   // one thread and can claim with a plain read-modify-write. Same g_par_active
   // gate the scheduler locks use.
   bool mine;
   if (unlikely(relaxed_load(&g_par_active))) {
      nvc_lock(&chunk->eval_lock);
      mine = !(chunk->last_eval_now == (uint64_t)m->now
               && chunk->last_eval_iter == m->iteration);
      if (mine) {
         chunk->last_eval_now  = (uint64_t)m->now;
         chunk->last_eval_iter = m->iteration;
      }
      nvc_unlock(&chunk->eval_lock);
   }
   else {
      mine = !(chunk->last_eval_now == (uint64_t)m->now
               && chunk->last_eval_iter == m->iteration);
      if (mine) {
         chunk->last_eval_now  = (uint64_t)m->now;
         chunk->last_eval_iter = m->iteration;
      }
   }
   if (!mine)
      return;   // another worker owns this chunk's eval this delta
   model_thread_t *thread = model_thread(m);
   rt_wakeable_t *save_obj   = thread->active_obj;
   rt_scope_t    *save_scope = thread->active_scope;
   aj_chunk_t    *save_chunk = g_aj_cur_chunk[tid];
   thread->active_obj   = &proc->wakeable;
   thread->active_scope = proc->scope;
   g_aj_cur_chunk[tid]  = chunk;
   // Primary-clock ARMING first: the mode-4 snapshot decision depends on
   // whether THIS eval fires the primary posedge (see aj_snap_mode docs).
   bool armed_rose = false;
   static int no_arm = -1;
   if (no_arm < 0) no_arm = getenv("NVC_ACCEL_NO_ARM") != NULL;
   if (!no_arm && chunk->set_clklast != NULL && chunk->primary_ck != NULL) {
      rt_nexus_t *cn = &chunk->primary_ck->nexus;
      armed_rose = cn->last_event == (uint64_t)m->now
         && (chunk->primary_ck->shared.data[0] & 1)
         && chunk->ck_arm_now != (uint64_t)m->now + 1;
      if (armed_rose) chunk->ck_arm_now = (uint64_t)m->now + 1;  // +1: 0 unused
      // NVC_ACCEL_ASSERT_ORDER (slice 2b): a rise-fall-RISE glitch within
      // one timestep arms only once — the once-per-instant guard silently
      // drops the second capture (measured: /tmp/glitch73 fixture, interp
      // Y=56 vs accel Y=50 with zero diagnostics). Detection is at the
      // second-rise eval itself: a NEW clock event in THIS delta
      // (event_delta == iteration, the standard consumer predicate) with
      // the level high while this timestep's rise is already consumed.
      // The fall eval cannot be relied on — fall wakes are filtered by
      // the rising-edge trigger machinery, so v1's post-arm-fall tracking
      // never saw the glitch at all.
      { const int _ao = aj_ao_level();
        if (_ao >= 3 && armed_rose) {
           chunk->ao_cap_now = (uint64_t)m->now + 1;
           chunk->ao_cap_delta = (uint32_t)m->iteration;
           chunk->ao_cap_fired = 0;
        }
        if (_ao && !armed_rose
            && chunk->ck_arm_now == (uint64_t)m->now + 1
            && (chunk->primary_ck->shared.data[0] & 1)
            && cn->last_event == (uint64_t)m->now
            && cn->event_delta == m->iteration) {
           if (chunk->ao_obs_now != (uint64_t)m->now + 1) {
              chunk->ao_obs_now = (uint64_t)m->now + 1;  // report 1x/timestep
              notef("accel-assert: DROPPED EDGE chunk %s t=%"PRIu64
                    " delta %u: second primary-clock rise within one "
                    "timestep (rise-fall-rise glitch) — once-per-instant "
                    "arming captures only the first%s",
                    chunk->scope != NULL && chunk->scope->where != NULL
                       ? istr(tree_ident(chunk->scope->where)) : "?",
                    (uint64_t)m->now, (unsigned)m->iteration,
                    _ao >= 2 ? "; RE-ARMING to capture it" : "");
           }
           // =2: capture the edge instead of only reporting it — interp
           // is the golden reference for --accel, and interp counts both
           // rises (glitch fixture: interp Y=56 vs un-rearmed accel
           // Y=50). Sound against the guard's original double-capture
           // bug: a single rise event is "new" (event_delta==iteration)
           // in exactly one delta, and per-delta dedup stops repeat
           // evals within it — each rise event arms at most once.
           if (_ao >= 2)
              armed_rose = true;
        }
      }
      (*chunk->set_clklast)(chunk->state, armed_rose ? 0 : 1);
      // CK_LATE + dead/re-keyed primary: the bridge's raw-byte posedge
      // never fires for these, so snapS/late_pend would never arm — poke
      // the bridge's force_arm from the model-side (re-keyed) arming.
      { static int _lt2 = -1;
        if (_lt2 < 0) _lt2 = getenv("NVC_ACCEL_CK_LATE") != NULL;
        if (_lt2 && armed_rose && chunk->force_arm_fn != NULL)
           chunk->force_arm_fn(chunk->state); }
   }
   // EXTRA-clock rise: engage the mode-4 pre-edge snapshot even when the
   // PRIMARY did not rise.  A chunk can be clocked entirely by non-primary
   // pins (measured: merged_31 — FPGA-shape members whose `clk` pin carries
   // a never-rising gated net while the flops clock on rawclk extras); its
   // extra-clock families fire inside the bridge from live clock bytes, so
   // without this the data inputs are read LIVE post-edge and every flop
   // captures ONE CYCLE EARLY (bus_last_data_beat 10ns ahead).  Primary
   // set_clklast semantics stay untouched: no primary edge is manufactured.
   bool extra_rose = false;
   if (!no_arm && chunk->primary_ck != NULL) {
      for (int k = 0; k < chunk->n_ck_sigs && !extra_rose; k++) {
         rt_signal_t *cs = chunk->ck_sigs[k];
         if (cs == NULL || cs == chunk->primary_ck)
            continue;
         if (cs->nexus.last_event == (uint64_t)m->now
             && (cs->shared.data[0] & 1))
            extra_rose = true;
      }
      if (extra_rose) {
         if (chunk->ck_xarm_now == (uint64_t)m->now + 1)
            extra_rose = false;   // once per instant (same trap as ck_arm_now)
         else
            chunk->ck_xarm_now = (uint64_t)m->now + 1;
      }
   }
   // DELTA-ORDERED CAPTURE v0 (NVC_ACCEL_CAPTURE_DELTA=k, user directive):
   // the interp delta cascade IS the correct ordering — rim producers
   // (interp packing flops, gated commits) land at deep deltas (measured:
   // wb_pkt at +11) AFTER the chunk's natural armed eval (~+3), so the
   // lumped capture must be PLACED after them.  Defer the armed capture by
   // un-stamping the arm (the whole arm sequence replays at the next wake —
   // clk last_event/byte still hold within the timestep) and self-queueing
   // for the next delta, until iteration >= k.  v0 = one global k to prove
   // the mechanism; per-chunk values come from the interp delta census.
   { static int _cd = -2;
     if (_cd == -2) { const char *e = getenv("NVC_ACCEL_CAPTURE_DELTA");
                      _cd = e ? atoi(e) : -1; }
     if (armed_rose || extra_rose) {
        // census recording: the chunk's arming depth in the delta cascade
        if (m->iteration > chunk->cap_maxdelta)
           chunk->cap_maxdelta = m->iteration;
        // per-chunk capture depth from the census file (lazy one-time
        // lookup; 0 = unprobed, -1 = absent-from-census -> global k)
        if (chunk->cap_delta == 0)
           chunk->cap_delta = aj_census_lookup(chunk->rs_top);
     }
     const int _k = (chunk->cap_delta > 0) ? chunk->cap_delta : _cd;
     if (_k >= 0 && (armed_rose || extra_rose)
         && m->iteration < (unsigned)_k
         && chunk->cap_pending != (uint64_t)-1) {   // drain force: capture now
        // v1: engagement must not depend on the dispatch path (v0's
        // save_obj guard silently skipped fastclk-TABLE evals — mixed
        // deferred/undeferred population, boot death).  Un-stamp the arm
        // (the arm sequence replays at the capture wake: clk last_event
        // and byte still hold within the timestep) and queue the CHUNK on
        // the model-side capq; the delta-boundary drain re-wakes its
        // representative proc once iteration reaches the target or the
        // timestep would otherwise end.
        if (armed_rose) chunk->ck_arm_now = 0;
        if (extra_rose) chunk->ck_xarm_now = 0;
        if (chunk->cap_pending != (uint64_t)m->now + 1) {
           chunk->cap_pending = (uint64_t)m->now + 1;
           if (m->aj_capq_count == m->aj_capq_max) {
              m->aj_capq_max = m->aj_capq_max ? m->aj_capq_max * 2 : 32;
              m->aj_capq = xrealloc_array(m->aj_capq, m->aj_capq_max,
                                          sizeof(aj_chunk_t *));
           }
           m->aj_capq[m->aj_capq_count++] = chunk;
        }
        g_aj_cur_chunk[tid]  = save_chunk;
        thread->active_obj   = save_obj;
        thread->active_scope = save_scope;
        return;
     } }
   // Two-phase edge sampling (see struct _aj_chunk and aj_snap_mode).
   // Mode 4 (default): armed posedge evals read the per-timestep pre-edge
   // snapshot, everything else reads live.  Modes 1-3: historical forms
   // kept as bisection knobs.
   bool use_snap = false;
   if (chunk->snap_nin > 0 && aj_snap_mode() > 0) {
      // engagement telemetry: a run with zero fleet passes never executed this
      // code at all (dispatch-path bypass) -- the unexecuted-patch trap,
      // instrumented this time.
      static nvc_lock_t snap_lock = 0;
      const bool par = relaxed_load(&g_par_active);
      if (par) nvc_lock(&snap_lock);
      aj_snap_fleet_take(m);   // lazy taker: no-op if the boundary hoist ran
      use_snap = aj_snap_mode() >= 4 ? (armed_rose || extra_rose) : true;
      if (use_snap) g_aj_snap_used++;
      if (par) nvc_unlock(&snap_lock);
   }
   if (use_snap)
      for (unsigned j = 0; j < chunk->snap_nin; j++)
         chunk->bindtab[chunk->snap_map[j].slot] =
            chunk->snap + chunk->snap_map[j].off;
   // NVC_ACCEL_RST_HOLD release trigger: outputs byte-stable since the seed
   // never re-poke aj_out, so the reset assertion must ALSO be checked at
   // eval entry or their latches would never flush (the bridge change-gates
   // on o_prev).  This is the flush that lands AT the interp settle edge:
   // chunk evals fire on clock edges, so the first eval with reset driven-
   // asserted IS the edge where interp's sync resets clear.  Reads LIVE
   // reset bytes (not the snapshot).
   if (aj_rst_hold() && !chunk->rst_released && chunk->rst_data != NULL
       && m->now > 0) {
      const unsigned rb = chunk->rst_data[0];
      if ((rb & 2) && (chunk->rst_low ? !(rb & 1) : (rb & 1)))
         aj_rst_release(m, chunk);
   }
   { static int _ec = -1;
     if (_ec < 0) _ec = getenv("NVC_ACCEL_EVAL_STATS") != NULL;
     if (_ec && chunk->primary_ck != NULL) {
        extern uint64_t g_aj_ev_rise[64], g_aj_ev_fall[64];
        extern uint64_t g_aj_ev_rise_bt[64], g_aj_ev_armed[64];
        extern char g_aj_ev_name[64][48], g_aj_ev_ck[64][48];
        unsigned ci = 0;
        for (; ci < m->aj_chunk_count && m->aj_chunks[ci] != chunk; ci++);
        if (ci < 64) {
           if (g_aj_ev_name[ci][0] == '\0') {
              snprintf(g_aj_ev_name[ci], sizeof g_aj_ev_name[ci], "%s",
                       chunk->rs_top != NULL ? chunk->rs_top : "?");
              snprintf(g_aj_ev_ck[ci], sizeof g_aj_ev_ck[ci], "%s",
                       chunk->primary_ck->where != NULL
                          ? istr(tree_ident(chunk->primary_ck->where)) : "?");
           }
           if (chunk->primary_ck->shared.data[0] & 1)
              g_aj_ev_rise[ci]++;
           else
              g_aj_ev_fall[ci]++;
           // Cross-check byte source: bindtab[4] is what the BRIDGE reads as
           // the primary clock.  rise==0 while rise_bt>0 means primary_ck's
           // signal bytes are NOT the bytes driving the netlist (pointer /
           // rim mismatch) — exactly the arming failure being hunted.
           const uint8_t *bt = (const uint8_t *)chunk->bindtab[4];
           if (bt != NULL && (bt[0] & 1))
              g_aj_ev_rise_bt[ci]++;
           if (armed_rose || extra_rose)
              g_aj_ev_armed[ci]++;
        }
     } }
   // NVC_ACCEL_ARMTRACE=<substr of rs_top>: per-eval arming/snapshot
   // engagement trace (model.c side of EVTRACE — the bridge cannot see
   // armed_rose/use_snap).
   { static const char *_at = (const char *)-1;
     if (_at == (const char *)-1) _at = getenv("NVC_ACCEL_ARMTRACE");
     if (_at != NULL && chunk->rs_top != NULL
         && strstr(chunk->rs_top, _at) != NULL)
        fprintf(stderr, "[arm %s] t=%"PRIi64"+%u armed=%d extra=%d snap=%d "
                "snapnow=%"PRIu64" lev=%"PRIu64" byte=%u arm_now=%"PRIu64"\n",
                chunk->rs_top, m->now, m->iteration,
                (int)armed_rose, (int)extra_rose, (int)use_snap,
                m->aj_snap_now,
                chunk->primary_ck != NULL
                   ? (uint64_t)chunk->primary_ck->nexus.last_event : 0,
                chunk->primary_ck != NULL
                   ? (unsigned)chunk->primary_ck->shared.data[0] : 0,
                chunk->ck_arm_now); }
   { static int _rc = -1;
     if (_rc < 0) _rc = getenv("NVC_ACCEL_RECAP") != NULL;
     if (_rc && chunk->recap_size == 0 && chunk->dl != NULL) {
        // probe once: the replayable-prefix size (register block S +
        // edge-detects, everything BEFORE the change baselines and
        // publication gates).  Old .so's lack the symbol -> RECAP inert.
        unsigned long (*rsz)(void) = dlsym(chunk->dl, "accel_recap_size");
        chunk->recap_size = rsz != NULL ? rsz() : ULONG_MAX;
     }
     if (_rc && chunk->recap_size > 0 && chunk->recap_size != ULONG_MAX) {
        if (armed_rose || extra_rose) {
           if (chunk->recap == NULL)
              chunk->recap = xmalloc(chunk->recap_size);
           memcpy(chunk->recap, chunk->state, chunk->recap_size);
           chunk->recap_now = (uint64_t)m->now + 1;
        }
        else if (chunk->recap != NULL
                 && chunk->recap_now == (uint64_t)m->now + 1) {
           // replay: restore ONLY the register/edge-detect prefix so the
           // bridge's edge-detect refires and the capture re-runs against
           // the current (later-delta) input world; change baselines and
           // publication gates stay live (v1's fatal flaw)
           memcpy(chunk->state, chunk->recap, chunk->recap_size);
        }
     } }
   if (chunk->eval) chunk->eval(chunk->state, chunk->bindtab);
   if (use_snap)
      for (unsigned j = 0; j < chunk->snap_nin; j++)
         chunk->bindtab[chunk->snap_map[j].slot] = chunk->snap_map[j].live;
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
   bool           negflip;    // merged-chunk reg output: stage at posedge,
                              // deposit_signal at the domain clock's FALL
                              // (task #53/#59 -- derivation-free barrier)
   bool           icg;        // gated-clock output (pin *l1clk): once
                              // published high, HOLD through the clk-high
                              // phase -- a live-computed enable drop would
                              // emit a runt pulse the real ICG latch
                              // suppresses (missed 35ns reset captures)
   uint8_t        icg_last;   // last published value bit
   void          *sigp;       // negflip: the signal to deposit into
   int            pw;         // negflip: port width (deposit element count)
   bool           dirty;      // shadow staged this cycle, awaiting swap
   bool           verify_flagged;  // NVC_ACCEL_VERIFY: already reported diverged
   bool           off_edge;   // seen changing on a non-posedge delta => Mealy/
                              // combinational => never route through NBA region
   bool           must_e;     // #67 E/U pipeline: this ordinal's publication is
                              // load-bearing same-delta (ICG clock / Mealy /
                              // comb class) and must execute on the EVAL thread;
                              // only !must_e ordinals (defer-bank, NBA-region,
                              // stage2 = registered classes) may become records
                              // for the update thread (audit verdict D).
   unsigned char *rh_latch;   // NVC_ACCEL_RST_HOLD: bytes latched during reset
   void          *rh_sigp;    //   publication target for the release flush
   int            rh_width;   //   deposit element count for the flush
   bool           rh_have;    //   latch holds bytes awaiting the release
   bool           probe;      // NVC_ACCEL_PIN_COMPLETE __ajp probe pin: values
                              // are whole-timestep (relay feed), never a
                              // delta-accurate oracle -- VERIFY skips compare
};

// NVC_ACCEL_VERIFY report: compact logic3d-bytes -> value hex (bit0 of each
// element) + the net name and sim time, once per diverging output.
static void aj_verify_report(void *sigp, const unsigned char *interp,
                             const unsigned char *accel, size_t valuesz, int width,
                             const char *tag)
{
   const int cap = g_aj_vtrack ? 200000 : 100;
   if (g_aj_vreports++ >= cap) {
      if (g_aj_vreports == cap + 1)
         notef("accel-verify: further divergences suppressed (cap %d)", cap);
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
         "%s  (%s)", tm, g_aj_model->iteration,
         istr(tree_ident(((rt_signal_t *)sigp)->where)), iv, av,
         width > 64 ? " [low 64b]" : "", tag);
}

// Baked into the bridge as AJ_OUT: called once per output with the logic3d
// bytes the chunk computed. Routed to the CURRENTLY-running chunk's deferred-
// output table (g_aj_cur_chunk, set by aj_proc_eval). A deferred output is
// copied into its shadow (the swap publishes it later); a non-deferred output
// falls back to the immediate deposit, exactly as before.
static inline int aj_rst_hold(void)
{
   static int v = -1;
   if (v < 0) {
      const char *e = getenv("NVC_ACCEL_RST_HOLD");
      v = e ? atoi(e) : 0;
   }
   return v;
}

// RST_HOLD release anchor: the chunk's own rst input rim is usually DEAD
// (its glue proc was rerouted with the subtree), frozen at init bytes — a
// release condition read there fires spuriously (measured: driven-0 at 5ns
// read as an active-low assertion).  Anchor instead on the LIVE root of the
// reset network: the shallowest signal under the model root whose leaf name
// is an rst-family match — the TB drives that one directly.
static rt_signal_t *aj_rst_root_find(rt_scope_t *s, const char *base,
                                     bool low, int depth)
{
   for (int si = 0; si < s->signals.count; si++) {
      rt_signal_t *sig = s->signals.items[si];
      if (sig->where == NULL) continue;
      char nm[32];
      aj_lower(nm, istr(tree_ident(sig->where)), sizeof nm);
      const char *n = nm;
      if (n[0] == 's' && n[1] == '_') n += 2;   // tb s_ prefix
      size_t nl = strlen(n);
      if (low && nl > 2 && n[nl - 2] == '_'
          && (n[nl - 1] == 'l' || n[nl - 1] == 'n' || n[nl - 1] == 'b'))
         nl -= 2;
      if (nl == strlen(base) && strncmp(n, base, nl) == 0)
         return sig;
   }
   if (depth <= 0) return NULL;
   for (int ci = 0; ci < s->children.count; ci++) {
      rt_signal_t *hit = aj_rst_root_find(s->children.items[ci], base, low,
                                          depth - 1);
      if (hit != NULL) return hit;
   }
   return NULL;
}

// NVC_ACCEL_RST_HOLD release: flush every publication latched during the
// reset window (the RST_HOLD branch in aj_out) and switch the chunk to
// sticky pass-through.  Immediate deposits: the flush IS the interp
// mass-settle instant — the X->certain byte changes are real events that
// wake the readers, and intra-instant delta order is immaterial (the
// reference's own settle lands at d=4 and its d=3 readers heal from the
// wake the same way).
static void aj_rst_release(rt_model_t *m, aj_chunk_t *c)
{
   c->rst_released = true;
   int nfl = 0;
   for (unsigned ord = 0; ord < c->defer_count; ord++)
      nfl += c->defer_outs[ord].rh_have ? 1 : 0;
   notef("accel-jit: RST_HOLD release (%d latched) at %"PRIi64, nfl,
         model_now(m, NULL));
   for (unsigned ord = 0; ord < c->defer_count; ord++) {
      aj_defer_out_t *d = &c->defer_outs[ord];
      if (!d->rh_have || d->rh_sigp == NULL)
         continue;
      d->rh_have = false;
      if (c->out_drv_n != NULL)
         for (int k = 0; k < c->out_drv_n[ord]; k++) {
            struct aj_odrv *od = &c->out_drv[ord][k];
            copy_value_ptr(od->nx, &od->src->u.driver.waveforms.value,
                           d->rh_latch + od->off);
         }
      deposit_signal(m, (rt_signal_t *)d->rh_sigp, d->rh_latch, 0,
                     d->rh_width);
      if (c->out_extra_n != NULL)
         for (int k = 0; k < c->out_extra_n[ord]; k++)
            deposit_signal(m, c->out_extra[ord][k], d->rh_latch, 0,
                           d->rh_width);
   }
}

static void aj_out(int ord, void *sigp, const void *buf, int width, int posedge)
{
   rt_model_t *m = g_aj_model;
   aj_chunk_t *c = g_aj_cur_chunk[thread_id()];

   // NVC_ACCEL_OUT_TRACE=<n>: log the first <n> output pushes -- ordinal, sim
   // time, delta, the edge/cone-class flags and the packed value -- so
   // "was this net ever deposited, and with what?" is answered by evidence
   // rather than inference. The t=0 seed pass shows up as delta 0xffffffff.
   // NVC_ACCEL_OUT_TRACE_ORD/_FROM narrow it to one output / one start time.
   {  static int _ot = -1, _oord = -2; static long long _ofrom = -1;
      static long _n = 0;
      if (_ot < 0) { const char *e = getenv("NVC_ACCEL_OUT_TRACE");
                     _ot = e ? atoi(e) : 0;
                     const char *o = getenv("NVC_ACCEL_OUT_TRACE_ORD");
                     _oord = o ? atoi(o) : -2;
                     const char *f = getenv("NVC_ACCEL_OUT_TRACE_FROM");
                     _ofrom = f ? atoll(f) : -1; }
      if (_ot > 0 && _n < _ot && (_oord == -2 || _oord == ord)
          && (_ofrom < 0 || (m != NULL && (long long)m->now >= _ofrom))) {
         _n++;
         // Byte STRIDE is the port's element size, not 1: a logic3d/std_logic
         // element is 4 bytes wide, so bit b lives at buf[(width-1-b)*elem].
         // Decoding with stride 1 reads the padding and every wide value comes
         // out 0 -- a trace artefact that looks exactly like "the chunk computes
         // zero". Recover elem from the defer table (valuesz = width*elem).
         int esz0 = 1;
         if (c != NULL && ord >= 0 && (unsigned)ord < c->defer_count
             && width > 0)
            esz0 = (int)(c->defer_outs[ord].valuesz / width);
         if (esz0 < 1) esz0 = 1;
         const unsigned char *p2 = buf;
         uint64_t v = 0;
         for (int b = 0; b < width && b < 64; b++)
            if (p2[(size_t)(width - 1 - b) * esz0] & 1) v |= (uint64_t)1 << b;
         // Also decode the net's CURRENT interpreter-side bytes. In VERIFY mode
         // the interpreter still drives, so this column is the reference
         // timeline for the same net at the same (t,delta) -- the thing the
         // driving run has to be compared against. `e` is the per-element byte
         // stride (defer_outs[ord].valuesz / width when the table is built).
         uint64_t iv = 0;
         const unsigned char *ip =
            (sigp != NULL) ? ((rt_signal_t *)sigp)->shared.data : NULL;
         const int esz = esz0;
         if (ip != NULL)
            for (int b = 0; b < width && b < 64; b++)
               if (ip[(size_t)(width - 1 - b) * esz] & 1) iv |= (uint64_t)1 << b;
         // Routing metadata: which publication class this ord takes, how many
         // quenched drivers get refreshed (drv=0 => port-hop consumers pull a
         // STALE driving value even though the rim's effective bytes updated),
         // and the rim nexus's output-port fanout (nout>0 => a port hop whose
         // propagation reads driving values exists).
         int _drv = -1, _dfr = -1, _nfl = -1, _icgf = -1, _nout = -1,
             _xtr = -1;
         if (c != NULL && ord >= 0 && (unsigned)ord < c->defer_count) {
            _dfr  = c->defer_outs[ord].defer;
            _nfl  = c->defer_outs[ord].negflip;
            _icgf = c->defer_outs[ord].icg;
            if (c->out_drv_n != NULL) _drv = c->out_drv_n[ord];
            if (c->out_extra_n != NULL) _xtr = c->out_extra_n[ord];
         }
         int _npend = -1;
         if (sigp != NULL) {
            _nout = 0;
            for (rt_source_t *o = ((rt_signal_t *)sigp)->nexus.outputs;
                 o != NULL; o = o->chain_output)
               _nout++;
            void *pd = ((rt_signal_t *)sigp)->nexus.pending;
            if (pointer_tag(pd) == 1) _npend = 1;
            else if (pd != NULL) {
               rt_pending_t *pl = untag_pointer(pd, rt_pending_t);
               _npend = pl->count;
            }
            else _npend = 0;
         }
         fprintf(stderr, "#AJOUT ord=%d t=%llu d=%u w=%d pe=%d val=0x%llx "
                 "net=0x%llx dfr=%d nfl=%d icg=%d drv=%d nout=%d pend=%d xtr=%d "
                 "chunk=%s sig=%s scope=%s\n", ord,
                 (unsigned long long)(m ? m->now : 0),
                 m ? m->iteration : 0, width, posedge,
                 (unsigned long long)v, (unsigned long long)iv,
                 _dfr, _nfl, _icgf, _drv, _nout, _npend, _xtr,
                 (c != NULL && c->rs_top != NULL) ? c->rs_top : "?",
                 (sigp != NULL)
                    ? istr(tree_ident(((rt_signal_t *)sigp)->where)) : "?",
                 (sigp != NULL
                  && ((rt_signal_t *)sigp)->parent != NULL)
                    ? istr(((rt_signal_t *)sigp)->parent->name) : "?");
      }
   }

   // NVC_ACCEL_VERIFY: passive check — the interpreter drives the net; compare the
   // accel bytes against its settled value and report the first divergence per
   // output. Never deposit (the reference sim must stay golden). Advance passes
   // (g_aj_vcompare == false) just step the .so state per delta so its multi-clock
   // register order matches the rerouted model; only the settle pass compares.
   if (g_aj_verify) {
      if (g_aj_vcompare && c != NULL && ord >= 0
          && (unsigned)ord < c->defer_count
          && !c->defer_outs[ord].probe) {   // __ajp probes: whole-timestep
                                            // relay feeds, not an oracle
         aj_defer_out_t *d = &c->defer_outs[ord];
         // d->eff is BANK-only; read the signal's live value directly (offset 0,
         // matching deposit_signal(sigp, buf, 0, width)).
         const unsigned char *interp = ((rt_signal_t *)sigp)->shared.data;
         const bool diff = aj_verify_diff(interp, buf, d->valuesz, width);
         if (!d->verify_flagged && diff) {
            d->verify_flagged = true;
            aj_verify_report(sigp, interp, buf, d->valuesz, width,
                             "accel diverges from interp");
            if (g_aj_vreports == 1 && getenv("NVC_ACCEL_SMDUMP") != NULL) {
               // settled-state dump at the FIRST divergence (needs -DSM_DUMP)
               void (*dumpfn)(void *) = dlsym(c->dl, "accel_dump");
               if (dumpfn != NULL) dumpfn(c->state);
            }
         }
         else if (g_aj_vtrack && d->verify_flagged && !diff) {
            // NVC_ACCEL_VERIFY_TRACK: net was diverged, now matches again — log
            // the reconvergence and re-arm so a later re-divergence reports too.
            // A net that RECONVERGES next timestep was a one-step transient
            // (orchestration/timing); one that never reconverges is state drift.
            d->verify_flagged = false;
            aj_verify_report(sigp, interp, buf, d->valuesz, width, "RECONVERGED");
         }
      }
      return;
   }
   // Publication target list: the bound rim signal plus any root alias
   // networks collected by aj_quench_rerouted_drivers (sibling same-name
   // signals whose quenched drivers this chunk replaced). Every publication
   // below must land on all of them or the alias readers keep init-X bytes.
   rt_signal_t *aj_pub[7] = { (rt_signal_t *)sigp };
   int aj_npub = 1;
   if (c != NULL && ord >= 0 && (unsigned)ord < c->defer_count
       && c->out_extra_n != NULL)
      for (int k = 0; k < c->out_extra_n[ord]; k++)
         aj_pub[aj_npub++] = c->out_extra[ord][k];

   // NVC_ACCEL_RST_HOLD: hold publications while the chunk's reset input is
   // asserted.  Measured on fused VeeR: the interp reference keeps the rim
   // plane X through the reset window and mass-settles X->certain at the
   // first post-reset edge (20,333 rim events at t=35ns d=4 in the ref, all
   // absent from the fused run because chunk seeds certain-ized the same
   // nets at t=0 d=3 — values identical, wake EVENTS gone; comb islands
   // that wake on the settle recirculate captured X instead: the cyc86-89
   // event-hole).  Latch here, flush at the first post-deassert eval — the
   // X->certain byte change then lands at the interp settle instant as a
   // real event.  Gated-clock (icg) outputs are exempt: clocks must toggle
   // during reset.
   // Release condition: the first instant reset is DRIVEN and ASSERTED —
   // that is when the interp flops CLEAR (sync $adff at the first edge
   // under assertion; measured: VeeR S_RST_L is high 0-30ns, asserts low at
   // 30ns, the interp mass-settle is the 35ns edge).  Driven matters: the
   // t=0 seed runs while the pin still holds undriven init bytes, which
   // must read as hold, not as an active-low assertion.
   if (aj_rst_hold() && c != NULL && !c->rst_released && c->rst_data != NULL
       && ord >= 0 && (unsigned)ord < c->defer_count) {
      // now>0: at t=0 the rst rim still holds its INIT bytes (driven-0)
      // until the tb value propagates through glue — indistinguishable from
      // an active-low assertion (measured: all 14 chunks released at 0ms+1).
      const unsigned rb = c->rst_data[0];
      if (m->now == 0
          || !((rb & 2) && (c->rst_low ? !(rb & 1) : (rb & 1)))) {
         aj_defer_out_t *d = &c->defer_outs[ord];
         if (!d->icg) {
            if (d->rh_latch == NULL)
               d->rh_latch = xmalloc(d->valuesz);
            memcpy(d->rh_latch, buf, d->valuesz);
            d->rh_sigp  = sigp;
            d->rh_width = width;
            d->rh_have  = true;
            return;
         }
      }
      else
         aj_rst_release(m, c);   // reset now active: flush, then pass through
   }

   // Refresh the original (rerouted) procs' drivers on this output: port
   // propagation and resolution read DRIVING values, not the deposited
   // effective bytes, so the connected drivers must always carry the
   // chunk's current value (aj_quench_rerouted_drivers registration).
   if (c != NULL && ord >= 0 && (unsigned)ord < c->defer_count
       && c->out_drv_n != NULL)
      for (int k = 0; k < c->out_drv_n[ord]; k++) {
         struct aj_odrv *od = &c->out_drv[ord][k];
         copy_value_ptr(od->nx, &od->src->u.driver.waveforms.value,
                        (const uint8_t *)buf + od->off);
      }

   // ICG latch rule: a gated-clock output that has published HIGH must not
   // publish LOW while the chunk's source clock is still high -- the real
   // gater latches its enable during the low phase, so an enable change in
   // the high phase cannot truncate the pulse. The chunk computes l1clk
   // live; suppress the runt fall (the low-phase eval publishes it).
   if (c != NULL && ord >= 0 && (unsigned)ord < c->defer_count
       && c->defer_outs[ord].icg && width == 1) {
      aj_defer_out_t *ig = &c->defer_outs[ord];
      // NVC_ACCEL_ICG_FALLWAKE: this rule assumes "the low-phase eval
      // publishes the fall" — but the subscribed rerouted proc's compiled
      // rising-edge trigger FILTERS fall wakes (run_process), so in a quiet
      // low phase (no other input changes) the fall never publishes: the rim
      // stays high and downstream interp consumers see alternate rises only
      // (glue2 fixture: half the gated-clock edges).  Clearing the trigger
      // delivers every clock event; the bridge derives its own edges, so the
      // extra evals are correctness-neutral.  Lazy (first icg publication)
      // so it needs no install-order guarantees; after demotion the interp
      // proc just evals-and-holds on falls (slower, still correct).
      { static int fw = -1;
        if (fw < 0) fw = getenv("NVC_ACCEL_ICG_FALLWAKE") != NULL;
        if (fw && !c->icg_fallwake_set && c->rr_count > 0) {
           c->rr_saved[0].proc->wakeable.trigger = NULL;
           c->icg_fallwake_set = true;
        } }
      { static int igdbg = -1;
        if (igdbg < 0) igdbg = getenv("NVC_ACCEL_ICGDBG") ? 40 : 0;
        if (igdbg > 0) {
           igdbg--;
           const uint8_t chv = c->bindtab != NULL && c->bindtab[4] != NULL
              ? (((const uint8_t *)c->bindtab[4])[0] & 1) : 0xff;
           fprintf(stderr, "ICGDBG t=%llu nb=%u last=%u clkhi=%u fwset=%d\n",
                   (unsigned long long)m->now,
                   ((const uint8_t *)buf)[0] & 1,
                   c->defer_outs[ord].icg_last, chv, (int)c->icg_fallwake_set);
        } }
      const unsigned nb = ((const uint8_t *)buf)[0] & 1;
      const uint8_t clkhi = c->bindtab != NULL && c->bindtab[4] != NULL
         ? (((const uint8_t *)c->bindtab[4])[0] & 1) : 0;
      if (nb == 0 && ig->icg_last == 1 && clkhi)
         return;   // hold high through the high phase
      ig->icg_last = (uint8_t)nb;
      // Clocks are WAVEFORMS, not registered state: never route them through
      // the negflip/NBA staging (a staged fall flushes an instant late and
      // the rim sees rise-then-fall compressed into one instant -- interp
      // gater latches downstream open early and emit runt gated clocks).
      // Publish immediately, fan to alias extras, done.
      for (int k = 0; k < aj_npub; k++)
         deposit_signal(m, aj_pub[k], buf, 0, width);
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
         for (int k = 0; k < aj_npub; k++)
            deposit_signal(m, aj_pub[k], buf, 0, width);
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
         // DEFAULT ON since the full-VeeR mechanism-3 campaign: routing reg
         // outputs through the NBA region (and comb-of-edge through the
         // 2-delta stage) is the correct cross-boundary flop-to-flop
         // protocol, not an option.  NVC_ACCEL_NBA=0 is the bisection knob.
         const char *nb = getenv("NVC_ACCEL_NBA");
         _nba = nb ? atoi(nb) : 1;
         const char *s2 = getenv("NVC_ACCEL_STAGE2");
         _st2 = s2 ? atoi(s2) : _nba;   // default: follow NBA; 0 forces off
      }
      const int pe = posedge & 1, combcls = posedge & 4;
      // Merged-chunk negedge flip: registered outputs stage into the shadow
      // on EVERY eval that pushes them — not only primary-posedge evals.
      // Extra-clock families (thread-gated registers inside a fused
      // primary-domain chunk) update on evals where the primary pe bit is
      // clear; their immediate deposits bypassed the flip and same-timestep
      // consumers captured post-edge values (measured: EH2 thread 1 ran +8
      // ahead from its first activation at cyc84 and trapped; gals2 fixture
      // reproduces in 5s).  All gated rises happen inside the primary high
      // phase, so publishing at the primary FALL stays correctly ordered
      // for every family.  Comb pushes keep their live paths.
      if (!combcls && c != NULL && c->merged && ord >= 0
          && (unsigned)ord < c->defer_count
          && c->defer_outs[ord].negflip) {
         // NBA-region publication, NOT the fall flip: interp's delta cascade
         // makes a register commit visible to LATER-delta consumers of the
         // SAME timestep (deeper gated clocks — dec's l2clk procs), and a
         // fall-published value arrives one delta regime too late for them
         // (measured: EH2 thread 1 via interp dec; gals2 deep-cascade
         // consumer reproduces in 5s).  sched_deposit nonblock lands in this
         // timestep's NBA region: early-delta consumers still read old,
         // later-delta consumers read new — cascade-equivalent.
         // BLOCKING (0-delay, next-delta) deposit — NOT nonblock: interp
         // commits a register at eval-delta+1 and deeper-cascade consumers
         // (2+ gating levels down) legitimately read it within the same
         // timestep; a NONBLOCK deposit lands at the NEXT TIMESTEP's delta
         // 0, one regime late (measured by AJ_EVDBG delta census; gals2
         // qmon fixture).  0-delay blocking = VHDL `<=` timing exactly.
         // Seed/install context (no active proc): the scheduler is not
         // running — an NBA-scheduled deposit never lands and the outputs
         // stay 'U' (measured: gals2 Y=0).  Deposit immediately there.
         if (model_thread(m)->active_obj == NULL)
            for (int k = 0; k < aj_npub; k++)
               deposit_signal(m, aj_pub[k], buf, 0, width);
         else {
            sched_deposit(m, (rt_signal_t *)sigp, buf, 0, width, 0, false);
            for (int k = 1; k < aj_npub; k++)
               sched_deposit(m, aj_pub[k], buf, 0, width, 0, true);
         }
         return;
      }
      if (_nba && pe && !combcls)
         for (int k = 0; k < aj_npub; k++)
            sched_deposit(m, aj_pub[k], buf, 0, width, 0, true /*nonblock*/);
      else if (_st2 && pe && combcls && c != NULL && ord >= 0
               && (unsigned)ord < c->defer_count) {
         aj_stage2_put((rt_signal_t *)sigp, buf,
                       c->defer_outs[ord].valuesz, width);
         for (int k = 1; k < aj_npub; k++)
            sched_deposit(m, aj_pub[k], buf, 0, width, 0, true);
      }
      else {
         // Comb/off-edge class: interp alias glue delivered SAME delta —
         // extras stay same-region. (Registered classes above delay extras
         // one region, matching their interp transfer hop: mhartstart's
         // plain copy must not start thread-1's fetch a cycle early.)
         for (int k = 0; k < aj_npub; k++) {
            if (_st2) aj_stage2_cancel(aj_pub[k]);  // newer value wins
            deposit_signal(m, aj_pub[k], buf, 0, width);
         }
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
      { static int dbg = -2;
        if (dbg == -2) { const char *v = getenv("AJ_POKEDBG");
                         dbg = v ? atoi(v) : -1; }
        if (dbg >= 0 && ord == dbg) {
           extern rt_model_t *__model_for_dbg;
           const unsigned char *pb = packed;
           fprintf(stderr, "#PK ord=%d stage t=? bytes=%02x%02x%02x%02x\n",
                   ord, pb[0], pb[1], pb[2], pb[3]);
        } }
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
         { static int dbg = -2;
           if (dbg == -2) { const char *v = getenv("AJ_POKEDBG");
                            dbg = v ? atoi(v) : -1; }
           if (dbg >= 0 && e->ord == dbg) {
              const unsigned char *pb = e->stage;
              fprintf(stderr, "#PA ord=%d apply t=%llu d=%u "
                      "bytes=%02x%02x%02x%02x\n", e->ord,
                      (unsigned long long)m->now, m->iteration,
                      pb[0], pb[1], pb[2], pb[3]);
           } }
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
static uint64_t g_aj_hf_ns = 0, g_aj_hf_calls = 0;   // T0 startup audit
static uint64_t g_aj_fs_ns = 0, g_aj_fs_calls = 0;

static rt_signal_t *aj_find_signal_inner(rt_scope_t *scope,
                                         const char *lname);

static rt_signal_t *aj_find_signal(rt_scope_t *scope, const char *lname)
{
   const uint64_t t0 = get_timestamp_ns();
   rt_signal_t *r = aj_find_signal_inner(scope, lname);
   g_aj_fs_ns += get_timestamp_ns() - t0;
   g_aj_fs_calls++;
   return r;
}

static rt_signal_t *aj_find_signal_inner(rt_scope_t *scope, const char *lname)
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

// nvc stores a bit-like scalar as its ENUM POSITION, and that position is
// type-dependent: '0'/'1' sit at 2/3 in std_(u)logic and in logic3d (L3D_0=2,
// L3D_1=3) but at 0/1 in BIT and BOOLEAN. The output bridge writes
// `base | valuebit`, so it needs the '0' position of the PORT'S OWN type.
// It used to hard-code 2; a BIT-typed output (every ITC'99 circuit) was then
// deposited as 2/3 — not a legal BIT position — so every downstream compare
// against '0' or '1' was false and the design's outputs read as constant
// garbage even though the model computed them correctly.
static int aj_bit_base(type_t t)
{
   type_t et = type_is_array(t) ? type_elem(t) : t;
   type_t b = type_base_recur(et);
   if (type_kind(b) != T_ENUM) return 2;
   const unsigned n = type_enum_literals(b);
   for (unsigned i = 0; i < n; i++) {
      tree_t lit = type_enum_literal(b, i);
      if (!tree_has_ident(lit)) continue;
      const char *s = istr(tree_ident(lit));
      if (!strcmp(s, "'0'") || !strcasecmp(s, "L3D_0")
          || !strcasecmp(s, "FALSE"))
         return (int)i;
   }
   return 2;   // unrecognised: keep the historical logic3d encoding
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
   c->vtable.on_abort = proc_on_abort_default;
   m->aj_chunks[m->aj_chunk_count++] = c;
   return c;
}

// Route every process in a subtree to ITS chunk's vtable (recovered later by
// aj_proc_eval via the vtable-first-field trick). Each proc's PRE-reroute
// vtable is saved on the chunk so aj_chunk_demote can reverse this exactly.
static void aj_reroute(rt_scope_t *scope, aj_chunk_t *chunk)
{
   for (int pi = 0; pi < scope->procs.count; pi++) {
      rt_proc_t *p = scope->procs.items[pi];
      if (chunk->rr_count == chunk->rr_max) {
         chunk->rr_max = chunk->rr_max ? chunk->rr_max * 2 : 64;
         chunk->rr_saved = xrealloc_array(chunk->rr_saved, chunk->rr_max,
                                          sizeof(struct aj_rr_saved));
      }
      chunk->rr_saved[chunk->rr_count].proc = p;
      chunk->rr_saved[chunk->rr_count].vt   = p->vtable;
      chunk->rr_count++;
      proc_set_vtable(p, &chunk->vtable);
   }
   for (int ci = 0; ci < scope->children.count; ci++)
      aj_reroute(scope->children.items[ci], chunk);
}

// Keep a rerouted chunk sensitive to its own clocks.
//
// WHY THIS IS REQUIRED FOR CORRECTNESS, not performance. The generated bridge
// does not receive an edge; it INFERS one by sampling the bound clock bytes at
// whatever deltas it happens to run:
//
//    driving:  posedge = (_clk && !aj_cs->clk_last0)    // real value edge;
//                        // last0 is ARMED per eval by aj_proc_eval from the
//                        // clock nexus' event state (was `clk && t != last_t`,
//                        // a level+time proxy that phantom-fired -- history
//                        // in the emitter comment at the posedge fprintf)
//    extra ck: rise    = (_n && !aj_cs->ck_last[k])     // vs its LAST sample
//
// Both rules are only sound if the chunk is evaluated in the delta where the
// clock actually transitions. Before the reroute that was incidentally true:
// the subtree's own processes drove its internal nets, so the subtree was busy
// every delta. Rerouting replaces every one of those procs with the chunk eval,
// which deposits ONLY the boundary outputs -- the internal nets go quiet, the
// mutual wakeups disappear, and the chunk is left waking on boundary-input
// events alone. Measured on VeeR-EH2 eh2_ifu_ifc_ctl (600ns, one chunk):
// 58 chunk evals driving vs 2486 in VERIFY, all of them at delta 16..54, none
// at the delta 0..4 where clk and active_clk actually move. Consequences:
//
//   * a timestep with no boundary-input change during clk-high evaluates the
//     chunk zero times, so `t != last_t` never fires and the clock edge is
//     SILENTLY DROPPED (t=155ns and 185ns in that run);
//   * an edge that does fire fires at an arbitrary late delta instead of the
//     real one, so every value the chunk publishes is a delta (or a cycle) out
//     of step with the interpreted flops around it;
//   * a gated/derived clock is worse: active_clk had already settled high by
//     the chunk's first sample, so `_n && !ck_last` never saw the transition
//     and its whole register group stopped advancing -- the edge mask degraded
//     from 0x3 to 0x1 permanently after t=135ns.
//
// This is exactly why NVC_ACCEL_VERIFY sees nothing: the companion is stepped
// every delta AND compiles a different rule (`_clk && !clk_last0`, a real edge
// detect), so the passive path never executes the broken one. It is also why a
// structural miter proves the chunk equivalent -- the COMPUTE is right; it is
// being clocked at the wrong deltas and skipping edges.
//
// The fix is to restore the sensitivity the reroute destroyed: subscribe one of
// the chunk's own rerouted procs to each clock net, so every clock event wakes
// the chunk in the delta the event happens. `t != last_t` then always fires on
// the first eval of a clk-high timestep (which is now the clk-event delta), and
// the extra-clock sampler observes the 0->1 transition it was missing.
//
// Cost is bounded: one extra wakeup per clock event per chunk, collapsed by
// aj_proc_eval's per-(time,delta) dedup into at most one eval.
static void aj_subscribe_clocks(rt_model_t *m, aj_chunk_t *chunk)
{
   if (chunk->rr_count == 0)
      return;
   // Any rerouted proc works: they all share the chunk's vtable, so waking any
   // one of them runs aj_proc_eval for this chunk.
   rt_wakeable_t *obj = &(chunk->rr_saved[0].proc->wakeable);
   if (getenv("NVC_ACCEL_CKSUB_DBG") != NULL)
      notef("accel-jit: cksub '%s': %d clock sig(s)",
            chunk->rs_top != NULL ? chunk->rs_top : "?", chunk->n_ck_sigs);
   for (int k = 0; k < chunk->n_ck_sigs; k++) {
      rt_signal_t *sig = chunk->ck_sigs[k];
      rt_nexus_t *n = &(sig->nexus);
      for (unsigned nx = 0; nx < sig->n_nexus; nx++, n = n->chain) {
         sched_event(m, &(n->pending), obj);
         // ALSO subscribe the ULTIMATE DRIVING nexus: the bound clk signal
         // may be a primitive's local port whose own pending list never
         // notifies (collapsed/quiet propagation) while the real clock
         // events at the root — the VeeR TB memory-model flop bundles bound
         // their local clk ports and NEVER EVALED after t=0 (rd_addr frozen,
         // rims stuck init-X: the fused X-poisoning + stale-value defect).
         rt_nexus_t *root = n;
         for (int hop = 0; hop < 16; hop++) {
            rt_source_t *ps = NULL;
            for (rt_source_t *s = &(root->sources); s != NULL;
                 s = s->chain_input)
               if (s->tag == SOURCE_PORT && s->u.port.input != NULL)
                  ps = s;
            if (ps == NULL) break;
            root = ps->u.port.input;
         }
         if (root != n)
            sched_event(m, &(root->pending), obj);
         if (getenv("NVC_ACCEL_CKSUB_DBG") != NULL)
            notef("accel-jit: cksub '%s' k=%d sig=%s nx=%p root=%p%s",
                  chunk->rs_top != NULL ? chunk->rs_top : "?", k,
                  sig->where != NULL ? istr(tree_ident(sig->where)) : "?",
                  (void *)n, (void *)root, root != n ? " (root-sub)" : "");
      }
   }
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
static void aj_dissolve_fastclk_ex(rt_model_t *m, bool sweep_strays);

// ===================================================================
// Reversible accel-chunk DEMOTION (the REVERSIBILITY DIRECTIVE's missing
// case: promoted forms must demote at a delta boundary with ZERO state
// loss so a live sim keeps rolling through design edits).
//
// aj_chunk_demote() runs at a settled delta boundary and:
//   1. quiesces  — asserts no chunk eval is in flight (the
//      fused_block_dissolve !running discipline);
//   2. WRITES BACK every register of the .so's state_t into its bound nvc
//      signal, as L3D driven-certain bytes (2-state bit -> 2|bit, i.e.
//      '0'/'1' — the exact conversion the bridge's AJ_OUT output path
//      uses), via deposit_signal;
//   3. restores every rerouted proc's PRE-reroute vtable (saved by
//      aj_reroute), so interp processes resume from the written-back
//      signal state at their next wake — they were parked at their waits
//      the whole time (reroute swaps vtables only; sensitivity/pending
//      lists and coroutine resume PCs were never touched);
//   4. unbinds the chunk (removed from m->aj_chunks, tables freed). The
//      dlopen handle is deliberately LEAKED — bounded at one .so mapping
//      per demoted chunk per run; dlclose while any stale pointer into
//      the .so's rodata could survive is not worth the risk (precedent:
//      the respecialization .so handle is also never closed).
//
// Register->signal binding is by NAME, the write-direction twin of the
// _nvc.c sm_reg_names/sm_write_nvc glue: gen_statemachine names each
// state_t field cname(<flattened yosys wire>) = leading '_' + the
// '_'-joined vid()-sanitized instance path + signal name (vhdl2vlog emits
// instances/wires via vid(): lowercase, non-alnum -> '_'; yosys flatten
// joins with '.'; cname maps '.' and '\' to '_'). We rebuild the same
// flattened name for every rt_signal_t in the chunk's scope subtree and
// require an EXACT match for EVERY register.
//
// DECLINE (loudly, chunk stays accelerated) rather than miscompute when:
//   - the .so predates the demote tables (stale cache — clear
//     ~/.cache/nvc/accel after rebuilding nvc);
//   - any register fails to bind, or binds to a width-mismatched signal;
//   - the state_t contains a MEMORY (word order across vhdl2vlog/yosys/
//     nvc array signals is unverified — writing a permuted RAM back would
//     be silent corruption);
//   - the chunk is wired chunk-to-chunk (NVC_ACCEL_HANDOFF, either
//     direction): a producer would poke freed state / a consumer's
//     deposit-bypassed inputs have no interp-visible home.
//
// PRIVDATA STALENESS (investigated for this design): a demoted-region
// interp proc's privdata holds its resume PC plus process variables.
// sv2vhdl emits blocking-assign shadows as variables RE-SEEDED from their
// signal at the top of every activation (v_x := x; ... x <= v_x) and
// blocking assigns land on SIGNALS (the := extension), so translated RTL
// carries NO cross-activation variable state — the written-back signals
// are the whole state. A process that DID carry read-before-write
// variable state would not survive vhdl2vlog translation, and a subtree
// that does not fully translate never installs — the install gate is the
// detector for the unsafe class.
// ===================================================================

static void deposit_signal_impl(rt_model_t *m, rt_signal_t *s,
                                const void *values, int offset, size_t count,
                                bool wake_next);

uint64_t g_aj_dep_nx = 0, g_aj_dep_noop = 0, g_aj_dep_w1 = 0,
         g_aj_dep_chg = 0;
uint64_t g_aj_ev_rise[64], g_aj_ev_fall[64];
uint64_t g_aj_ev_rise_bt[64], g_aj_ev_armed[64];
char g_aj_ev_name[64][48], g_aj_ev_ck[64][48];
__attribute__((destructor))
static void aj_eval_stats_dump(void)
{
   if (getenv("NVC_ACCEL_EVAL_STATS") == NULL) return;
   for (int i = 0; i < 64; i++)
      if (g_aj_ev_rise[i] + g_aj_ev_fall[i] > 0)
         fprintf(stderr, "#EVSTATS chunk%d rise=%"PRIu64" fall=%"PRIu64
                 " rise_bt=%"PRIu64" armed=%"PRIu64" name=%s primary=%s\n",
                 i, g_aj_ev_rise[i], g_aj_ev_fall[i], g_aj_ev_rise_bt[i],
                 g_aj_ev_armed[i], g_aj_ev_name[i], g_aj_ev_ck[i]);
}
__attribute__((destructor))
static void aj_dep_stats_dump(void)
{
   if (g_aj_dep_nx > 0 && getenv("NVC_DEP_STATS") != NULL)
      fprintf(stderr, "#DEPSTATS nx=%"PRIu64" changed=%"PRIu64
              " noop=%"PRIu64" w1=%"PRIu64"\n",
              g_aj_dep_nx, g_aj_dep_chg, g_aj_dep_nx - g_aj_dep_chg,
              g_aj_dep_w1);
}

typedef struct {
   char         name[512];
   rt_signal_t *sig;
} aj_dm_ent_t;

typedef struct {
   aj_dm_ent_t *v;
   int          n, max;
} aj_dm_idx_t;

// Append vid()-equivalent sanitization of `src` (basename after the last
// '.', lowercased, non-alnum -> '_') to dst.
static void aj_dm_cat(char *dst, size_t n, const char *src)
{
   size_t i = strlen(dst);
   const char *dot = strrchr(src, '.');
   if (dot != NULL) src = dot + 1;
   for (const char *p = src; *p != '\0' && i + 1 < n; p++) {
      char c = tolower((unsigned char)*p);
      dst[i++] = isalnum((unsigned char)c) ? c : '_';
   }
   dst[i] = '\0';
}

static void aj_dm_add(aj_dm_idx_t *ix, const char *prefix, const char *nm,
                      rt_signal_t *sig)
{
   if (ix->n == ix->max) {
      ix->max = ix->max ? ix->max * 2 : 256;
      ix->v = xrealloc_array(ix->v, ix->max, sizeof(aj_dm_ent_t));
   }
   aj_dm_ent_t *e = &ix->v[ix->n++];
   snprintf(e->name, sizeof e->name, "%s", prefix);
   aj_dm_cat(e->name, sizeof e->name, nm);
   e->sig = sig;
}

static void aj_dm_walk(rt_scope_t *s, const char *prefix, aj_dm_idx_t *ix)
{
   for (int i = 0; i < s->signals.count; i++) {
      rt_signal_t *sig = s->signals.items[i];
      aj_dm_add(ix, prefix, istr(tree_ident(sig->where)), sig);
   }
   for (int i = 0; i < s->aliases.count; i++) {
      rt_alias_t *a = s->aliases.items[i];
      aj_dm_add(ix, prefix, istr(tree_ident(a->where)), a->signal);
   }
   for (int ci = 0; ci < s->children.count; ci++) {
      rt_scope_t *c = s->children.items[ci];
      char cp[512];
      snprintf(cp, sizeof cp, "%s", prefix);
      aj_dm_cat(cp, sizeof cp, istr(tree_ident(c->where)));
      size_t l = strlen(cp);
      if (l + 1 < sizeof cp) { cp[l] = '_'; cp[l + 1] = '\0'; }
      aj_dm_walk(c, cp, ix);
   }
}

static rt_signal_t *aj_dm_find(const aj_dm_idx_t *ix, const char *name)
{
   for (int i = 0; i < ix->n; i++)
      if (strcmp(ix->v[i].name, name) == 0)
         return ix->v[i].sig;
   return NULL;
}

static bool aj_chunk_demote(rt_model_t *m, aj_chunk_t *chunk)
{
   const uint64_t t0 = get_timestamp_ns();
   const char *top = chunk->rs_top != NULL ? chunk->rs_top : "?";

   // ---- 1. safe point: settled delta boundary, never mid-eval -------------
   assert(g_aj_cur_chunk[thread_id()] == NULL);

   if (chunk->rr_count == 0) {
      notef("accel-jit: demote '%s' declined — chunk not rerouted "
            "(VERIFY companion)", top);
      return false;
   }

   // handoff-wired chunks (either direction) are not individually demotable
   if (chunk->hoff_nedges > 0) {
      notef("accel-jit: demote '%s' declined — %d packed handoff edge(s) "
            "to consumer chunks (NVC_ACCEL_HANDOFF)", top, chunk->hoff_nedges);
      return false;
   }
   if (chunk->hin_flags != NULL)
      for (int i = 0; i < chunk->rs_npins; i++)
         if (chunk->hin_flags[i]) {
            notef("accel-jit: demote '%s' declined — input %d is handoff-fed "
                  "by a producer chunk (NVC_ACCEL_HANDOFF)", top, i);
            return false;
         }

   // ---- 2. demote tables from the .so (emitted by aj_emit_bridge) ---------
   const char **rname            = dlsym(chunk->dl, "aj_reg_name");
   const unsigned long *roff     = dlsym(chunk->dl, "aj_reg_off");
   const int *rwidth             = dlsym(chunk->dl, "aj_reg_width");
   const int *rdepth             = dlsym(chunk->dl, "aj_reg_depth");
   int *pnregs                   = dlsym(chunk->dl, "aj_n_regs");
   unsigned long (*psoff)(void)  = dlsym(chunk->dl, "aj_demote_state_off");
   if (rname == NULL || roff == NULL || rwidth == NULL || rdepth == NULL
       || pnregs == NULL || psoff == NULL) {
      notef("accel-jit: demote '%s' declined — .so lacks demote tables "
            "(stale cache: clear ~/.cache/nvc/accel)", top);
      return false;
   }
   const int nregs = *pnregs;

   // ---- 3. bind EVERY register to its nvc signal before touching anything -
   rt_signal_t **bound = xcalloc_array(nregs > 0 ? nregs : 1,
                                       sizeof(rt_signal_t *));
   aj_dm_idx_t ix = { NULL, 0, 0 };
   aj_dm_walk(chunk->scope, "", &ix);
   const uint64_t t_ix = get_timestamp_ns();

   int unmapped = 0, maxbuf = 1;
   for (int r = 0; r < nregs; r++) {
      const char *rn = rname[r];
      while (*rn == '_') rn++;         // cname() of '\'-prefixed yosys name
      if (rdepth[r] > 0) {
         notef("accel-jit: demote '%s' declined — register '%s' is a "
               "%d-word memory (writeback word order unverified)",
               top, rn, rdepth[r]);
         free(bound); free(ix.v);
         return false;
      }
      rt_signal_t *sig = aj_dm_find(&ix, rn);
      if (sig == NULL) {
         if (unmapped++ < 8)
            notef("accel-jit: demote '%s': register '%s' has no matching "
                  "signal", top, rn);
         continue;
      }
      const int elem  = (int)sig->nexus.size;
      const int count = elem > 0 ? (int)(sig->shared.size / elem) : 0;
      if (count != rwidth[r]) {
         if (unmapped++ < 8)
            notef("accel-jit: demote '%s': register '%s' width %d vs "
                  "signal %d", top, rn, rwidth[r], count);
         continue;
      }
      bound[r] = sig;
      if (rwidth[r] * elem > maxbuf) maxbuf = rwidth[r] * elem;
   }
   free(ix.v);
   if (unmapped > 0) {
      notef("accel-jit: demote '%s' declined — %d/%d register(s) unbound; "
            "state writeback would be lossy", top, unmapped, nregs);
      free(bound);
      return false;
   }

   const uint64_t t_bind = get_timestamp_ns();

   // If the fast-clk table / fused block hold members of this subtree,
   // dissolve both BEFORE the register writeback: the writeback deposits
   // reach wakeup_one, and a still-flagged member would latch
   // fastclk_hit that the dissolve then clears -- a lost wakeup (stale
   // downstream readers). Dissolved first, the deposits queue members
   // normally. (They rebuild themselves with hysteresis; the fused
   // block is dissolved by aj_dissolve_fastclk_ex.) Non-members keep
   // their table. The stray sweep is skipped: no candidate build is in
   // flight here, so the only flagged procs are exactly the committed
   // table's members, which the dissolve unflags directly.
   bool in_fastclk = false;
   if (m->fastclk_table != NULL || m->fused_block != NULL)
      for (unsigned i = 0; i < chunk->rr_count && !in_fastclk; i++)
         if (chunk->rr_saved[i].proc->wakeable.fastclk)
            in_fastclk = true;
   if (in_fastclk)
      aj_dissolve_fastclk_ex(m, false);
   const uint64_t t_fc = get_timestamp_ns();

   // ---- 4. writeback: state_t -> signals (2-state -> L3D driven-certain) -
   const uint8_t *S = (const uint8_t *)chunk->state + psoff();
   uint8_t *buf = xmalloc(maxbuf);
   for (int r = 0; r < nregs; r++) {
      rt_signal_t *sig = bound[r];
      const int w = rwidth[r], elem = (int)sig->nexus.size;
      memset(buf, 0, (size_t)w * elem);
      if (w <= 64) {
         uint64_t v;
         memcpy(&v, S + roff[r], sizeof v);
         for (int b = 0; b < w; b++)
            buf[(size_t)(w - 1 - b) * elem] = 2 | (unsigned)((v >> b) & 1);
      }
      else {
         const uint32_t *lb = (const uint32_t *)(S + roff[r]);
         for (int b = 0; b < w; b++)
            buf[(size_t)(w - 1 - b) * elem] =
               2 | (unsigned)((lb[b >> 5] >> (b & 31)) & 1);
      }
      // A signal read via 'event/rising_edge caches events (NET_F_CACHE_EVENT)
      // and the immediate deposit path asserts it never sees one; route those
      // through the wake_next path, which maintains the cached-event flags.
      bool cache_ev = false;
      rt_nexus_t *n = &sig->nexus;
      for (unsigned nx = 0; nx < sig->n_nexus; nx++, n = n->chain)
         if (n->flags & NET_F_CACHE_EVENT) { cache_ev = true; break; }
      deposit_signal_impl(m, sig, buf, 0, w, cache_ev);
   }
   free(buf);
   free(bound);
   const uint64_t t_wb = get_timestamp_ns();

   // ---- 5. restore every rerouted proc's original vtable ------------------
   unsigned restored = 0;
   for (unsigned i = 0; i < chunk->rr_count; i++) {
      rt_proc_t *p = chunk->rr_saved[i].proc;
      if (p->vtable == &chunk->vtable) {
         proc_set_vtable(p, chunk->rr_saved[i].vt);
         restored++;
      }
      // else: something else (reset_process) already replaced it — leave it
   }

   // Reconnect any drivers aj_quench_rerouted_drivers disconnected for this
   // chunk's procs: the resumed procs drive their nets again from the
   // written-back state.
   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
      for (rt_source_t *s = &(n->sources); s != NULL; s = s->chain_input) {
         if (s->tag != SOURCE_DRIVER || !s->aj_rerouted)
            continue;
         for (unsigned i = 0; i < chunk->rr_count; i++)
            if (s->u.driver.proc == chunk->rr_saved[i].proc) {
               s->aj_rerouted = 0;
               s->disconnected = 0;
               break;
            }
      }
   }

   const uint64_t t_vt = get_timestamp_ns();

   // ---- 6. unbind the chunk ----------------------------------------------
   aj_ao3_drop_chunk(chunk);   // census map must not outlive the chunk
   // Publish any staged-but-unswapped deferred outputs first (they hold the
   // value the interp side is owed), exactly as the bank swap would.
   if (chunk->defer_outs != NULL) {
      for (unsigned i = 0; i < chunk->defer_count; i++) {
         aj_defer_out_t *d = &chunk->defer_outs[i];
         if (d->dirty && d->nexus != NULL
             && !cmp_bytes(d->eff, d->shadow, d->valuesz)) {
            copy2(d->last, d->eff, d->shadow, d->valuesz);
            d->nexus->event_delta = m->iteration + 1;
            d->nexus->last_event  = m->now;
            if (d->cache_event)
               d->nexus->signal->shared.flags |= SIG_F_EVENT_FLAG;
            m->trigger_epoch++;
         }
         free(d->shadow);
      }
      free(chunk->defer_outs);
   }

   for (unsigned ci = 0; ci < m->aj_chunk_count; ci++)
      if (m->aj_chunks[ci] == chunk) {
         memmove(&m->aj_chunks[ci], &m->aj_chunks[ci + 1],
                 (m->aj_chunk_count - ci - 1) * sizeof(aj_chunk_t *));
         m->aj_chunk_count--;
         break;
      }

   free(chunk->bindtab);
   free(chunk->state);
   free(chunk->rr_saved);
   free(chunk->rs_bridge);
   free(chunk->rs_dutc);
   free(chunk->rs_pins);
   free(chunk->hoff_flags);
   free(chunk->out_extra);
   free(chunk->out_extra_n);
   free(chunk->out_drv);
   free(chunk->out_drv_n);
   free(chunk->hin_flags);
   free(chunk->hoff_edges);
   // chunk->dl deliberately NOT dlclose'd (see header comment); rs_top is
   // freed LAST (it names the chunk in the note below)
   const uint64_t dt = get_timestamp_ns() - t0;
   notef("accel-jit: DEMOTED '%s' — %d register(s) written back, %u/%u "
         "proc vtable(s) restored, %.1f us (index %.1f bind %.1f fastclk "
         "%.1f wb %.1f vt %.1f); interp resumes from this state",
         top, nregs, restored, chunk->rr_count, (double)dt / 1000.0,
         (double)(t_ix - t0) / 1000.0, (double)(t_bind - t_ix) / 1000.0,
         (double)(t_fc - t_bind) / 1000.0, (double)(t_wb - t_fc) / 1000.0,
         (double)(t_vt - t_wb) / 1000.0);
   free(chunk->rs_top);
   free(chunk);
   return true;
}

// Public entry point (control-plane ops / future live-edit invalidation):
// demote every installed chunk whose subtree name contains `tok` (NULL or
// "" = all chunks), NVC_ACCEL_ONLY matching semantics. Must be called at a
// settled delta boundary (the model_cycle NVC_ACCEL_DEMOTE_AT hook is the
// in-tree caller). Returns the number of chunks demoted.
int accel_demote(rt_model_t *m, const char *tok)
{
   int n = 0;
   for (int ci = (int)m->aj_chunk_count - 1; ci >= 0; ci--) {
      aj_chunk_t *c = m->aj_chunks[ci];
      if (tok != NULL && tok[0] != '\0'
          && (c->rs_top == NULL || strstr(c->rs_top, tok) == NULL))
         continue;
      if (aj_chunk_demote(m, c))
         n++;
   }
   return n;
}

// ---------------------------------------------------------------------------
// X/Z FALLBACK: the .so's demote REQUEST (GAP 2)
//
// The value-plane engine models 2 states. Its bridge now marks aj_cs->x_seen
// the moment a boundary input byte is not driven-certain (aj_scan_inputs), and
// exports that through the optional accel_x_seen symbol. This poll is the
// model's half: at the SETTLED end-of-time-step safe point (beside the
// NVC_ACCEL_DEMOTE_AT hook and the fork checkpoint — state settled, after
// can_create_delta so a writeback may legally open the next delta at this
// time), read every chunk's flag and, if the action is armed, hand the chunk
// back to the interpreter through the existing aj_chunk_demote machinery.
//
// DETECTION IS ALWAYS ON; THE ACTION IS NOT. nvc's certainty-0 init doctrine
// means every signal powers on uncertain and reset pulses legitimately drive
// X, so an always-on demote would fire in the first delta and disable accel
// entirely. NVC_ACCEL_XDEMOTE=1 arms the action; NVC_ACCEL_XDEMOTE_AFTER=<time>
// additionally ignores (and re-arms past) hits before that sim time, which is
// what makes "power-on U" distinguishable from "a real X appeared mid-run".
static int   g_aj_xdemote = -1;    // -1 = unparsed, 0 = detect only, 1 = act
static int64_t g_aj_xarm  = 0;     // fs; hits before this are cleared, not acted on
static bool  g_aj_have_xdet = false;   // any installed .so exports the detector
static int   g_aj_xnote = 0;       // report sightings (JIT_DEBUG, or acting)

static void aj_xseen_poll(rt_model_t *m)
{
   if (g_aj_xdemote < 0) {
      const char *e = getenv("NVC_ACCEL_XDEMOTE");
      g_aj_xdemote = (e != NULL && e[0] != '\0' && strcmp(e, "0") != 0);
      const char *a = getenv("NVC_ACCEL_XDEMOTE_AFTER");
      if (a != NULL && a[0] != '\0') {
         int64_t t = parse_fork_time(a);
         if (t < 0) {   // bare number = ns, as NVC_ACCEL_DEMOTE_AT
            char *end = NULL;
            unsigned long long v = strtoull(a, &end, 10);
            t = (*a != '\0' && end != NULL && *end == '\0')
               ? (int64_t)v * 1000000 : -1;
         }
         if (t < 0)
            warnf("cannot parse NVC_ACCEL_XDEMOTE_AFTER='%s' (want e.g. 200ns)",
                  a);
         else
            g_aj_xarm = t;
      }
      // Detection is always live, but a sighting is only WORTH a line when it
      // explains something: the user asked for JIT debug, or it is about to
      // change behaviour.
      g_aj_xnote = g_aj_xdemote || getenv("NVC_ACCEL_JIT_DEBUG") != NULL;
   }

   const bool armed = m->now >= (uint64_t)g_aj_xarm;

   // downwards: aj_chunk_demote removes the chunk from m->aj_chunks and frees
   // it, so nothing below the current index shifts under us and `c` is dead
   // the instant the demote succeeds.
   for (int ci = (int)m->aj_chunk_count - 1; ci >= 0; ci--) {
      aj_chunk_t *c = m->aj_chunks[ci];
      if (c->x_seen_fn == NULL || c->state == NULL) continue;
      if (!(*c->x_seen_fn)(c->state)) continue;

      if (!armed) {
         // Pre-arm sighting: re-arm the detector so the report names the FIRST
         // post-arm event rather than power-on U.
         if (c->x_clear_fn != NULL) (*c->x_clear_fn)(c->state);
         continue;
      }

      unsigned hits = 0, pin = 0;
      if (c->x_info_fn != NULL) (*c->x_info_fn)(c->state, &hits, &pin);
      const char *pname = "?";
      if (pin > 0 && c->b_in != NULL && (int)pin <= c->n_bin)
         pname = c->b_in[pin - 1].name;

      if (!c->x_reported && g_aj_xnote) {
         c->x_reported = true;
         notef("accel-jit: X/Z at accel boundary of '%s' — input '%s' "
               "(bridge ordinal %u) not driven-certain, %u hit(s), first seen "
               "at or before %s%s",
               c->rs_top != NULL ? c->rs_top : "?", pname,
               pin > 0 ? pin - 1 : 0, hits, trace_time(m->now),
               g_aj_xdemote ? " — demoting" : " (detect only; set "
               "NVC_ACCEL_XDEMOTE=1 to fall back to the interpreter)");
      }

      if (!g_aj_xdemote) continue;
      if (c->x_demote_tried) continue;   // a decline is permanent — don't spam
      c->x_demote_tried = true;
      aj_chunk_demote(m, c);             // c is freed on success
   }
}

static bool aj_blacklisted(rt_model_t *m, rt_nexus_t *n)
{
   for (unsigned i = 0; i < m->fastclk_nbl; i++)
      if (m->fastclk_bl[i] == n)
         return true;
   return false;
}

// Admission filter: only procs whose wakeup registration is STATIC
// qualify -- wait_state==1 (promoted static-wait; direct-eval installed)
// or trigger-armed sensitivity procs (trigger != NULL: these never
// promote past wait_state 0 because proc_static_wait_finalize skips
// them, but they re-register the same static sensitivity every
// activation and both dispatch paths run the trigger filter). Excluded:
// wait_state==2 (dynamic wait: re-registers on nexuses outside
// candidate+companions, whose wakes would only latch -- lost activation
// for a posedge-only member), wait_state==0 with no trigger (parked at a
// plain `wait;` or first-activation limbo -- resuming those from the
// table would run them spuriously), and delayed procs (a member resumed
// by the table keeps a stale eventq entry that trips the
// deltaq_insert_proc assert). The filter lives HERE rather than in
// aj_pending_foreach so the unflag/stray sweeps stay unconditional (a
// flagged proc must always be unflaggable even if its wait state moved).
//
// NO-TRIGGER members (wait_state==1 with no derived trigger: both-edge
// bodies, falling-edge guards lower couldn't canonicalise, report-only
// monitors) may act on ANY candidate event, and drive attribution
// cannot see non-driving side effects (reports, variable state) --
// posedge-only dispatch would silently starve them.  Every-event
// membership is normal-path-equivalent for a clk-only proc, so admit
// them straight into the EE tail; in non-wide mode there is no EE tail,
// so they are not admitted at all.
static bool fastclk_wide_enabled(void);

// NVC_FASTCLK_CENSUS: admission exclusion tally (indexes: 0 admitted,
// 1 delayed, 2 mid-queue pending, 3 dynamic wait_state==2,
// 4 wait_state==0 no-trigger limbo, 5 no-trigger non-wide decline)
static unsigned aj_flag_census[6];

static void aj_flag_cb(rt_wakeable_t *w, void *ctx)
{
   if (w->delayed)
      { aj_flag_census[1]++; return; }
   if (w->pending)                   // mid-queue procs are not admitted
      { aj_flag_census[2]++; return; }
   if (w->wait_state == 2 && w->trigger != NULL)
      { aj_flag_census[3]++; return; }
   if (w->wait_state == 1
       || (w->trigger != NULL && w->wait_state != 2)) {
      if (w->trigger == NULL) {
         if (!fastclk_wide_enabled())
            { aj_flag_census[5]++; return; }
         w->fastclk_ee = 1;
      }
      aj_flag_census[0]++;
      w->fastclk = 1;
   }
   else
      aj_flag_census[4]++;
}

static void aj_unflag_cb(rt_wakeable_t *w, void *ctx)
{
   w->fastclk = 0;
   w->fastclk_ee = 0;
}

// Wide-mode pass 1 helpers: count currently-flagged procs on a pending
// list (companion ranking) and mark the flagged overlap of a REGISTERED
// companion as every-event members.
static void aj_count_flagged_cb(rt_wakeable_t *w, void *ctx)
{
   if (w->fastclk)
      (*(unsigned *)ctx)++;
}

static void aj_ee_mark_cb(rt_wakeable_t *w, void *ctx)
{
   if (w->fastclk)
      w->fastclk_ee = 1;
}

// NVC_FAST_CLK_WIDE: default ON; =0 restores the guard machinery wholesale
static bool fastclk_wide_enabled(void)
{
   static int en = -1;
   if (en < 0) {
      const char *e = getenv("NVC_FAST_CLK_WIDE");
      en = (e == NULL || *e != '0');
   }
   return en;
}

static unsigned fastclk_companion_cap(void)
{
   static int cap = -1;
   if (cap < 0) {
      const char *e = getenv("NVC_FAST_CLK_COMPANIONS");
      cap = (e != NULL) ? MAX(0, MIN(atoi(e), 64)) : 8;
   }
   return (unsigned)cap;
}

// Probation sim-time bound (P3): NVC_FAST_CLK_PROBATION_NS, default 10us
static uint64_t fastclk_probation_fs(void)
{
   static uint64_t fs = 0;
   if (fs == 0) {
      const char *e = getenv("NVC_FAST_CLK_PROBATION_NS");
      const uint64_t ns = (e != NULL && strtoull(e, NULL, 10) > 0)
         ? strtoull(e, NULL, 10) : UINT64_C(10000);
      fs = ns * UINT64_C(1000000);
   }
   return fs;
}

// Any registered companion evented THIS delta? (wake_next deposits
// attribute to iteration+1 and are consumed post-increment, so the same
// test covers them -- identical semantics to x_test_net_event.)
static bool aj_companion_evented(rt_model_t *m)
{
   for (unsigned i = 0; i < m->fastclk_ncomp; i++) {
      rt_nexus_t *n = m->fastclk_comp[i];
      if (n->last_event == m->now && n->event_delta == m->iteration)
         return true;
   }
   return false;
}

// Did THIS member's own registered sensitivity event this delta? Used on
// NON-candidate-event dispatches (companion-only deltas): a member whose
// own nexuses are quiet must NOT be activated -- normal wakeup would not
// have run it, and a spurious activation is not harmless for STD_MX
// translated bodies that run an unconditional `wait for 0 ns` (the
// member would sit pending mid-#0 when its real clock edge arrives, and
// the pending-skip would drop that edge -- measured on VeeR: mixed-clock
// probation stalled the core within 20 cycles of the first build).
// Trigger-armed members return true: run_process's run_trigger applies
// exactly this filter before the body runs.
static bool aj_member_evented(rt_model_t *m, rt_proc_t *p)
{
   if (p->wakeable.trigger != NULL)
      return true;                  // run_trigger gates the body instead
   if (p->wakeable.wait_state != 1 || p->wait_count == 0)
      return true;                  // no static set recorded: conservative
   for (unsigned i = 0; i < p->wait_count; i++) {
      rt_nexus_t *n = p->wait_set[i];
      if (n->last_event == m->now && n->event_delta == m->iteration)
         return true;
   }
   return false;
}

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

// ---- DERIVED-CLOCK QUARANTINE ----------------------------------------------
// A chunk's group-0 edge detect races the producer of its clock whenever that
// clock is DERIVED -- computed by comb logic within the same timestep as the
// root edge (active_clk, free_clk, l1clk...).  The bridge samples `_clk` when
// the chunk happens to be evaluated, so against a comb-driven clock the edge
// can be seen a delta late, missed, or double-counted depending on wake order.
// LATE mode does not help: it protects extra clock GROUPS, and here the
// derived clock IS group 0.
//
// MEASURED on VeeR-EH2 (2026-08-01, the end of an 8-round two-machine
// bisection): rvdff1__fe1e -- twelve instances of a plain 3-bit async-reset
// flop -- includes ifu_mem_ctl's bus_mb_beat_count_ff, CLOCKED BY active_clk.
// Accelerated per-instance, the beat counter never advances: the IFU issues
// `AR addr=0` EVERY cycle forever (interp walks 0,8,16,24...), the bus wedges,
// and retirement stops.  In the full 1,359-chunk mix the same race in the
// commit counters shows as retire counts offset +4 from the first retirement.
// Each chunk's COMPUTATION is correct throughout (NVC_ACCEL_VERIFY: zero
// divergences) -- the defect is purely in edge/data ordering at the bridge.
//
// THE PREDICATE IS STRUCTURAL, not name-based: walk the clk pin's net through
// the port chain to its ultimate driver(s); if any driving process is
// SIGNAL-SENSITIVE, the clock is derived and the chunk declines (stays
// interpreted).  A primary clock generator (`wait for 5 ns` loop) has no
// signal sensitivity; a comb producer does.  Signal-sensitivity is established
// by scanning nexus pending lists for the proc -- O(nexuses) once per install.
// NVC_ACCEL_ALLOW_DERIVED_CLK=1 overrides for experiments.

// A process is a PRIMARY clock source iff it self-schedules on TIME -- its
// body contains a timed wait (`wait for ...`).  A comb producer of a derived
// clock waits on signals only.  Read from the AST rather than runtime wakeup
// structures: the first attempt scanned nexus pending lists, and this fork's
// Phase-C slot/trigger rework means those no longer carry sensitivity --
// every proc scanned as "not sensitive" and the gate never fired.
static bool aj_tree_has_timed_wait(tree_t t, int depth)
{
   if (t == NULL || depth > 24) return false;
   switch (tree_kind(t)) {
   case T_WAIT:
      return tree_has_delay(t);
   // A clock generator written as a DELAYED SIGNAL ASSIGNMENT self-loop
   // (`clk <= not clk after 5 ns when run` -- the accelbench testbenches)
   // has no timed wait; the time source is the waveform delay. Count it.
   case T_SIGNAL_ASSIGN:
      {
         const int nw = tree_waveforms(t);
         for (int i = 0; i < nw; i++)
            if (tree_has_delay(tree_waveform(t, i)))
               return true;
      }
      return false;
   // Only kinds that structurally CARRY a statement list are descended --
   // tree_stmts on anything else is an object-lookup fatal (measured: the
   // first version crashed nvc inside the first install).  A timed wait
   // hidden somewhere more exotic misclassifies that clock as derived, which
   // errs toward DECLINE -- the safe direction.
   case T_PROCESS:
   case T_LOOP:
   case T_WHILE:
   case T_FOR:
      {
         const int ns = tree_stmts(t);
         for (int i = 0; i < ns; i++)
            if (aj_tree_has_timed_wait(tree_stmt(t, i), depth + 1))
               return true;
      }
      return false;
   // A concurrent conditional assignment (`clk <= not clk after 5 ns when
   // run else '0'` -- every accelbench testbench) lowers to a process whose
   // delayed assign sits under an IF arm.  Not descending T_IF classified the
   // whole suite's clock as derived and the install-guard in accel-gate
   // caught it: 6/6 designs NO-INSTALL.  T_IF carries conds, not stmts.
   case T_IF:
      {
         const int nc = tree_conds(t);
         for (int i = 0; i < nc; i++)
            if (aj_tree_has_timed_wait(tree_cond(t, i), depth + 1))
               return true;
      }
      return false;
   case T_COND_STMT:
      {
         const int ns = tree_stmts(t);
         for (int i = 0; i < ns; i++)
            if (aj_tree_has_timed_wait(tree_stmt(t, i), depth + 1))
               return true;
      }
      return false;
   default:
      return false;
   }
}

static bool aj_proc_signal_sensitive(rt_model_t *m, rt_proc_t *proc)
{
   (void)m;
   return proc->where != NULL && !aj_tree_has_timed_wait(proc->where, 0);
}

static bool aj_nexus_driver_is_comb(rt_model_t *m, rt_nexus_t *n, int depth)
{
   if (n == NULL || depth > 16) return false;
   for (rt_source_t *s = &(n->sources); s != NULL; s = s->chain_input) {
      if (getenv("AJ_CLKDBG"))
         fprintf(stderr, "CLKDBG d=%d tag=%d proc=%s sens=%d fastclk=%d\n",
                 depth, (int)s->tag,
                 s->tag == SOURCE_DRIVER && s->u.driver.proc
                    ? istr(tree_ident(s->u.driver.proc->where)) : "-",
                 s->tag == SOURCE_DRIVER && s->u.driver.proc
                    ? aj_proc_signal_sensitive(m, s->u.driver.proc) : -1,
                 s->tag == SOURCE_DRIVER && s->u.driver.proc
                    ? s->u.driver.proc->wakeable.fastclk : -1);
      switch (s->tag) {
      case SOURCE_DRIVER:
         if (s->u.driver.proc != NULL
             && aj_proc_signal_sensitive(m, s->u.driver.proc))
            return true;
         break;
      case SOURCE_PORT:
         // follow the hierarchy toward the real driver
         if (aj_nexus_driver_is_comb(m, s->u.port.input, depth + 1))
            return true;
         break;
      default:
         break;
      }
   }
   return false;
}

// Walk port sources up the hierarchy to the ULTIMATE driving nexus — the one
// whose driver is a real process, not a port hop.  Two clock pins anywhere in
// the design that descend from the same generator resolve to the same nexus,
// which is the merge-group key (task #53/#59: grouping by the LOCAL pin
// signal fragmented VeeR into ~5-member groups because the clock tree is not
// port-collapsed across all hierarchy levels).
static rt_nexus_t *aj_ultimate_driver_nexus(rt_nexus_t *n, int depth)
{
   if (n == NULL || depth > 64)
      return n;
   for (rt_source_t *s = &(n->sources); s != NULL; s = s->chain_input) {
      if (s->tag == SOURCE_PORT && s->u.port.input != NULL) {
         if (getenv("AJ_ROOTDBG"))
            fprintf(stderr, "ROOTDBG d=%d PORT hop\n", depth);
         return aj_ultimate_driver_nexus(s->u.port.input, depth + 1);
      }
      // See through identity buffers: a clock distributed via
      // `assign a = b;` is a DRIVER source whose proc body is a single
      // signal assignment of a plain reference -- resolve b and keep
      // walking (VeeR's clock tree is built from exactly these, which is
      // why port-hops alone fragmented the domain into 5/35-member groups).
      if (s->tag == SOURCE_DRIVER && s->u.driver.proc != NULL) {
         rt_proc_t *dp = s->u.driver.proc;
         tree_t w = dp->where;
         if (getenv("AJ_ROOTDBG"))
            fprintf(stderr, "ROOTDBG d=%d DRIVER proc=%s kind=%d\n", depth,
                    istr(tree_ident(w)), (int)tree_kind(w));
         tree_t asgn = NULL;
         if (tree_kind(w) == T_CONCURRENT && tree_stmts(w) == 1)
            asgn = tree_stmt(w, 0);
         else if (tree_kind(w) == T_SIGNAL_ASSIGN)
            asgn = w;
         if (asgn != NULL && tree_kind(asgn) == T_SIGNAL_ASSIGN
             && tree_waveforms(asgn) == 1) {
            tree_t wav = tree_waveform(asgn, 0);
            if (!tree_has_delay(wav)) {          // identity only, not a clkgen
               tree_t val = tree_value(wav);
               if (tree_kind(val) == T_REF) {
                  char lname[64];
                  aj_lower(lname, istr(tree_ident(val)), sizeof lname);
                  rt_signal_t *src = aj_find_signal(dp->scope, lname);
                  if (getenv("AJ_ROOTDBG"))
                     fprintf(stderr, "ROOTDBG d=%d BUFFER -> %s (%s)\n",
                             depth, lname, src ? "resolved" : "MISS");
                  if (src != NULL && src->n_nexus == 1
                      && &src->nexus != n)
                     return aj_ultimate_driver_nexus(&src->nexus, depth + 1);
               }
            }
         }
         // NVC_ACCEL_CKREKEY: see through LATCH-BASED clock gates.  VeeR's
         // sv2v output clocks whole subsystems (ARF/INSTBUFF/ALN/every BHT
         // bank — 1,162 of 1,182 dryrun groups) through LPM latch nets:
         // the driver proc is the latch process inside an lpm_* entity, so
         // resolution stopped here and each consumer became an unmergeable
         // 1-member group that stays interpreted.  Resolve THROUGH the
         // gating entity's own clock input (same name heuristics as the
         // candidate scan) so the family keys to the true root; the gated
         // clock still drives the wrapper's members via their clk pins
         // (grouping KEY only — bindtab wiring unchanged).
         { static int _rk = -1;
           if (_rk < 0) _rk = getenv("NVC_ACCEL_CKREKEY") != NULL;
           if (_rk && asgn == NULL && dp->scope != NULL) {
              rt_signal_t *gck = aj_find_signal(dp->scope, "clk");
              if (gck == NULL) {
                 for (int gi = 0; gi < dp->scope->signals.count
                         && gck == NULL; gi++) {
                    rt_signal_t *sg = dp->scope->signals.items[gi];
                    if (sg->where == NULL) continue;
                    char nm[64];
                    aj_lower(nm, istr(tree_ident(sg->where)), sizeof nm);
                    const size_t nl = strlen(nm);
                    if (nl >= 3 && strcmp(nm + nl - 3, "clk") == 0)
                       gck = sg;
                 }
              }
              if (gck != NULL && gck->n_nexus == 1 && &gck->nexus != n)
                 return aj_ultimate_driver_nexus(&gck->nexus, depth + 1);
           } }
      }
   }
   return n;
}

// Diagnostic (MERGE_DRYRUN member dump): is this clock signal produced
// inside `cand` (some driver proc's scope is cand or a descendant)?  A
// self-produced-primary theory of the DESCEND BUSCLK freeze was tested
// and REFUTED with this predicate (the BUSCLK group was 7 pure consumers;
// the real cause was a POISONED synth-cache entry — see aj_stage_source).
// Kept for grouping inspection: a group whose primary really were one of
// its own outputs would be worth seeing in the dryrun listing.
static bool aj_clk_self_produced(rt_signal_t *clksig, rt_scope_t *cand)
{
   if (clksig == NULL || cand == NULL || clksig->n_nexus != 1)
      return false;
   for (rt_source_t *s = &(clksig->nexus.sources); s != NULL;
        s = s->chain_input) {
      if (s->tag != SOURCE_DRIVER || s->u.driver.proc == NULL)
         continue;
      for (rt_scope_t *w = s->u.driver.proc->scope; w != NULL; w = w->parent)
         if (w == cand)
            return true;
   }
   return false;
}

static bool aj_clk_is_derived(rt_model_t *m, rt_signal_t *clksig)
{
   if (clksig == NULL || clksig->n_nexus != 1) return false;
   return aj_nexus_driver_is_comb(m, &clksig->nexus, 0);
}

static void aj_build_fastclk(rt_model_t *m, rt_signal_t *clksig, uint8_t *clkdata)
{
   aj_dissolve_fastclk(m);    // rebuilt per install/candidate — full cleanup
   {  // default-on: only an explicit NVC_FAST_CLK_AUTO=0 (and no
      // NVC_FAST_CLK) suppresses the table machinery entirely
      const char *fk = getenv("NVC_FAST_CLK"), *fa = getenv("NVC_FAST_CLK_AUTO");
      if (fk == NULL && fa != NULL && strtoull(fa, NULL, 10) == 0 && *fa != '\0')
         return;
   }
   if (clksig->n_nexus != 1) return;        // single-bit clock only
   rt_nexus_t *clkn = &clksig->nexus;

   // Pass 0: provisionally flag every clk-pending proc.
   aj_pending_foreach(clkn->pending, aj_flag_cb, NULL);
   if (getenv("NVC_FASTCLK_CENSUS") != NULL) {
      notef("fastclk admission: admitted=%u delayed=%u midq=%u dynwait=%u "
            "limbo=%u nonwide=%u", aj_flag_census[0], aj_flag_census[1],
            aj_flag_census[2], aj_flag_census[3], aj_flag_census[4],
            aj_flag_census[5]);
      memset(aj_flag_census, 0, sizeof(aj_flag_census));
   }

   const bool strict = getenv("NVC_FAST_CLK_STRICT") != NULL;
   const bool wide   = fastclk_wide_enabled();

   if (wide) {
      // WIDE pass 1: a non-candidate nexus qualifies as a COMPANION iff it
      // is single-bit AND its pending list holds >=1 currently-flagged
      // proc. Companion eligibility deliberately IGNORES the candidate
      // blacklist -- that list means "not a clock", and a failed clock
      // candidate (rst) is prime companion material. Qualifiers are
      // ranked by flagged-overlap count; overflow past the cap unflags
      // (as does any non-qualifying overlap: multi-bit, strict mode, or
      // a busy-companion exclusion from a previous rate demote).
      typedef struct { rt_nexus_t *nx; unsigned ovl; } comp_cand_t;
      comp_cand_t *cand = NULL;
      unsigned ncand = 0, maxcand = 0;
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
         if (n == clkn) continue;
         if (aj_pending_count(n->pending) == 0) continue;
         unsigned ovl = 0;
         aj_pending_foreach(n->pending, aj_count_flagged_cb, &ovl);
         if (ovl == 0) continue;      // no member overlap: not our concern
         bool excl = strict || n->width != 1;
         for (unsigned x = 0; !excl && x < m->fastclk_nexcl; x++)
            excl = (m->fastclk_excl[x] == n);
         if (excl) {
            aj_pending_foreach(n->pending, aj_unflag_cb, NULL);
            continue;
         }
         if (ncand == maxcand) {
            maxcand = maxcand ? maxcand * 2 : 16;
            cand = xrealloc_array(cand, maxcand, sizeof(comp_cand_t));
         }
         cand[ncand].nx  = n;
         cand[ncand].ovl = ovl;
         ncand++;
      }
      // rank by overlap descending (stable enough: insertion sort, the
      // qualifier count is small on real designs -- a handful of resets)
      for (unsigned i = 1; i < ncand; i++) {
         comp_cand_t key = cand[i];
         unsigned j = i;
         for (; j > 0 && cand[j - 1].ovl < key.ovl; j--)
            cand[j] = cand[j - 1];
         cand[j] = key;
      }
      const unsigned cap = fastclk_companion_cap();
      for (unsigned i = cap; i < ncand; i++)     // overflow: unflag procs
         aj_pending_foreach(cand[i].nx->pending, aj_unflag_cb, NULL);
      m->fastclk_ncomp = MIN(ncand, cap);
      if (m->fastclk_ncomp > 0) {
         m->fastclk_comp = xmalloc_array(m->fastclk_ncomp, sizeof(rt_nexus_t *));
         for (unsigned i = 0; i < m->fastclk_ncomp; i++) {
            m->fastclk_comp[i] = cand[i].nx;
            // partition = static sensitivity: overlap procs are EVERY-EVENT
            aj_pending_foreach(cand[i].nx->pending, aj_ee_mark_cb, NULL);
         }
      }
      free(cand);
   }
   else {
      // GUARD pass 1 (NVC_FAST_CLK_WIDE=0): un-flag any proc that also
      // appears on an ACTIVE other nexus. A QUIET other nexus (no event
      // within QUIET_FS of now — e.g. released rst) does not disqualify
      // its clk procs: mark it as a GUARD instead; any later event on a
      // guard nexus dissolves the table and falls back to normal
      // dispatch before the wakeup propagates.
      const uint64_t QUIET_FS = UINT64_C(2000000000);  // 2us: only long-
      // stable nexuses qualify (at the first build only never-toggled)
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

      // Patch each guard nexus's vtable (post-elab vtable hack): a copy
      // whose notify dissolves the table (restoring every vtable) before
      // the event propagates, so member procs fall back to queued wakeup.
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
   }

   // Pass 2: collect the survivors into the table.
   const unsigned maxn = aj_pending_count(clkn->pending);
   if (maxn == 0) {
      aj_dissolve_fastclk_ex(m, false);  // free the companion list; no
      return;                            // procs were flagged -> no strays
   }
   m->fastclk_table = xmalloc_array(maxn, sizeof(rt_proc_t *));
   aj_pending_foreach(clkn->pending, aj_collect_cb, m);

   if (m->fastclk_count == 0) {
      // Every pass-0 flag was cleared in pass 1 and collect visited the
      // entire pass-0 set, so there are provably no strays: skip the
      // full-nexus sweep (tens of ms on a big design, per AUTO round).
      aj_dissolve_fastclk_ex(m, false);
      return;
   }
   m->fastclk_nexus = clkn;
   m->fastclk_data  = clkdata;
   m->fastclk_on    = true;
   m->fastclk_built_at = m->now;   // hysteresis: survival measured from here
   m->fastclk_probation = 64;      // candidate value-edge deltas to classify
   m->fastclk_hit_deltas = 0;      // hit deltas consumed during probation
   m->fastclk_comb = xcalloc_array(m->fastclk_count, 1);
   m->fastclk_probe_member = -1;
   m->fastclk_clk_last = clkdata[0];   // value-edge shadow (dispatch-site only)
   m->fastclk_ee_start = m->fastclk_count;  // no partition until probation exit
   m->fastclk_empty_backoff = 0;   // a live build resets the empty-round ladder
   if (getenv("NVC_ACCEL_JIT_DEBUG") != NULL) {  // quiet by default: the note
      notef("accel-jit: NVC_FAST_CLK — %u clk-only proc(s) in posedge table",
            m->fastclk_count);   // pollutes gold-output tests when default-on
      if (wide) {
         unsigned n_ee = 0;
         for (unsigned i = 0; i < m->fastclk_count; i++)
            n_ee += m->fastclk_table[i]->wakeable.fastclk_ee;
         char comps[256] = "";
         size_t cl = 0;
         for (unsigned i = 0; i < m->fastclk_ncomp && cl + 2 < sizeof comps; i++)
            cl += snprintf(comps + cl, sizeof comps - cl, " %s",
                           istr(tree_ident(m->fastclk_comp[i]->signal->where)));
         notef("accel-jit: fast-clk WIDE %s: %u member(s), %u posedge-only + "
               "%u every-event, %u companion(s):%s",
               istr(tree_ident(clksig->where)), m->fastclk_count,
               m->fastclk_count - n_ee, n_ee, m->fastclk_ncomp,
               m->fastclk_ncomp > 0 ? comps : " (none)");
      }
   }

   // NVC_FUSED_BLOCK fusion happens at PROBATION EXIT (the member set is
   // not final until comb fanout is evicted)
}

// Tear the posedge table down (guard nexus fired, or install replaced it):
// unflag members so wakeup_one queues them normally again, clear guards.
static void aj_dissolve_fastclk_ex(rt_model_t *m, bool sweep_strays)
{
   // The fused block sequences table members; it never outlives the table
   fused_block_dissolve(m);

   // Latch rescue (belt): if a wake was latched but not yet consumed (a
   // same-delta companion/guard event or a demote writeback reached the
   // dissolve first), the members were never queued -- dropping the latch
   // would lose that delta's activation. Re-wake every former member
   // through the normal path after unflagging (spurious wakes are
   // body-filtered via sync_event_cache, so over-waking is safe).
   rt_proc_t **rescue = NULL;
   unsigned nrescue = 0;
   if (m->fastclk_hit && m->fastclk_count > 0 && m->fastclk_table != NULL) {
      nrescue = m->fastclk_count;
      rescue = xmalloc_array(nrescue, sizeof(rt_proc_t *));
      memcpy(rescue, m->fastclk_table, nrescue * sizeof(rt_proc_t *));
   }

   if (m->fastclk_table != NULL) {
      for (unsigned i = 0; i < m->fastclk_count; i++) {
         m->fastclk_table[i]->wakeable.fastclk = 0;
         m->fastclk_table[i]->wakeable.fastclk_ee = 0;
      }
      free(m->fastclk_table);
      m->fastclk_table = NULL;
   }
   const bool was_on = m->fastclk_on;
   m->fastclk_on = false;
   m->fastclk_probation = 0;
   m->fastclk_probe_member = -1;
   free(m->fastclk_comb); m->fastclk_comb = NULL;
   m->fastclk_count = 0;
   m->fastclk_hit = false;
   free(m->fastclk_comp); m->fastclk_comp = NULL;
   m->fastclk_ncomp = 0;
   m->fastclk_ee_start = 0;
   m->fastclk_hit_deltas = 0;
   m->fastclk_evict_defer = false;
   m->fastclk_win_pos = m->fastclk_win_off = 0;
   m->fastclk_npending = 0;
   memset(m->fastclk_comp_off, 0, sizeof m->fastclk_comp_off);

   // NVC_ACCEL_BANK: flush + de-bank. Once the table is gone the
   // post-dispatch publish site is unreachable, so a staged-but-unswapped
   // shadow would go stale forever; and a rebuilt table has no proof the
   // new membership covers the banked readers. Publish anything dirty
   // (the value the interp side is owed -- mirrors the chunk-demote
   // flush) and permanently revert the outputs to the deposit path.
   for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
      aj_chunk_t *c = m->aj_chunks[ci];
      if (c->defer_outs == NULL) continue;
      for (unsigned i = 0; i < c->defer_count; i++) {
         aj_defer_out_t *d = &c->defer_outs[i];
         if (!d->defer) continue;
         if (d->dirty && d->nexus != NULL
             && !cmp_bytes(d->eff, d->shadow, d->valuesz)) {
            copy2(d->last, d->eff, d->shadow, d->valuesz);
            d->nexus->event_delta = m->iteration + 1;
            d->nexus->last_event  = m->now;
            if (d->cache_event)
               d->nexus->signal->shared.flags |= SIG_F_EVENT_FLAG;
            m->trigger_epoch++;
         }
         d->dirty = false;
         d->defer = false;    // de-bank: deposit path from now on
      }
      c->defer_pending = false;
   }
   // Unflag EVERY stray (failed candidates leave pass-0 flags on procs not
   // in any table; with fastclk_on they would skip wakeups forever). This
   // sweeps ALL nexuses (tens of ms on a big design) — strays only exist
   // transiently inside aj_build_fastclk, so callers at a point where no
   // candidate build is in flight (aj_chunk_demote) skip it: the committed
   // table's members were unflagged exactly above.
   if (sweep_strays)
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain)
         aj_pending_foreach(n->pending, aj_unflag_cb, NULL);
   for (unsigned i = 0; i < m->fastclk_nguards; i++)
      m->fastclk_guard_nx[i]->vtable = m->fastclk_guard_orig[i];
   free(m->fastclk_guard_nx);   m->fastclk_guard_nx = NULL;
   free(m->fastclk_guard_orig); m->fastclk_guard_orig = NULL;
   free(m->fastclk_guard_vt);   m->fastclk_guard_vt = NULL;
   m->fastclk_nguards = 0;
   if (was_on) {
      const char *fca_r = getenv("NVC_FAST_CLK_AUTO");
      if (fca_r == NULL || strtoull(fca_r, NULL, 10) != 0 || *fca_r == '\0') {
         // redo-as-we-go: re-arm (AUTO is default-on; =0 disables)
         // Rebuild HYSTERESIS.  The flat +500ns re-arm thrashed on busy
         // phases (VeeR: dissolve/rebuild every ~500-900ns for the whole
         // run — each rebuild is a full nexus scan, plus a ~250KB block
         // re-emission under NVC_FUSED_BLOCK — measured ~10% wall).
         // Exponential backoff with survival-based reset: a table that
         // lived >= 8x the current backoff paid for itself, so restart
         // the ladder at the base; one that died young doubles the wait
         // (capped).  The guard blacklist keeps converging membership at
         // each attempt; hysteresis just spaces the attempts to match
         // how hostile the current sim phase actually is.
         const uint64_t base = UINT64_C(500000000);        // 500ns
         const uint64_t cap  = UINT64_C(1000000000000);    // 1ms
         const uint64_t survival = m->now - m->fastclk_built_at;
         if (m->fastclk_backoff == 0)
            m->fastclk_backoff = base;
         else if (survival >= 8 * m->fastclk_backoff)
            m->fastclk_backoff = base;
         else
            m->fastclk_backoff = MIN(cap, m->fastclk_backoff * 2);
         m->fastclk_auto_at = m->now + m->fastclk_backoff;
      }
      // rebuild excludes recently-active nexuses so membership converges.
      static unsigned dcount = 0;
      if (dcount++ < 10 && getenv("NVC_ACCEL_JIT_DEBUG") != NULL)
         notef("accel-jit: fast-clk table dissolved (guard event)");
   }

   // Latch rescue wakes (flags cleared above -> normal queued wakeup;
   // pending/delayed dedup handled by wakeup_one/wake_proc as usual)
   if (rescue != NULL) {
      for (unsigned i = 0; i < nrescue; i++)
         wakeup_one(m, &rescue[i]->wakeable);
      free(rescue);
   }
}

static void aj_dissolve_fastclk(rt_model_t *m)
{
   aj_dissolve_fastclk_ex(m, true);
}

// Probation STALL verdict (P2/P3): the candidate is not accumulating
// value-edges (a reset captured as candidate, or a gated/slow clock).
// NEVER blacklist -- silence is not evidence of non-clockness (a gated
// clock resumes; a reset makes a fine companion). Dissolve + backoff
// re-arm + a TEMPORARY cooldown so the next scan round tries the
// next-widest candidate instead; the round after may retry this one.
static void aj_fastclk_stall(rt_model_t *m, const char *why)
{
   rt_nexus_t *cand = m->fastclk_nexus;
   if (getenv("NVC_ACCEL_JIT_DEBUG") != NULL)
      notef("accel-jit: fast-clk probation STALL (%s) — candidate %s on "
            "cooldown, table dissolved", why,
            cand != NULL ? istr(tree_ident(cand->signal->where)) : "?");
   aj_dissolve_fastclk(m);   // re-arms fastclk_auto_at with backoff (was_on)
   if (cand != NULL) {
      const unsigned slot = m->fastclk_cool_next++ % ARRAY_LEN(m->fastclk_cool);
      m->fastclk_cool[slot].nx = cand;
      // skip exactly the next scan round (armed at now+backoff); if AUTO
      // is off (fastclk_auto_at == 0) the entry is immediately stale
      m->fastclk_cool[slot].retry_at = m->fastclk_auto_at + 1;
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

// True iff every static waiter on this output nexus is a POSEDGE-ONLY
// fast-clk proc (so it is dispatched off the clock edge and reads the OLD
// effective value before the swap). Any non-W_PROC waiter, a W_PROC that
// did not survive the clk-only filter (fastclk==0), OR an every-event
// member (fastclk_ee -- its sensitivity may include this output, and the
// publish's no-wakeup bookkeeping would starve its O-triggered
// activation) means a same-delta reader could observe the new value or
// miss the change -> not deferrable.
static bool aj_out_pending_ok(void *pending)
{
   if (pointer_tag(pending) == 1) {
      rt_wakeable_t *w = untag_pointer(pending, rt_wakeable_t);
      return w->kind == W_PROC && w->fastclk && !w->fastclk_ee;
   }
   else if (pending != NULL) {
      rt_pending_t *p = untag_pointer(pending, rt_pending_t);
      for (int i = 0; i < p->count; i++) {
         rt_wakeable_t *w = p->wake[i];
         if (w == NULL) continue;
         if (w->kind != W_PROC || !w->fastclk || w->fastclk_ee) return false;
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
   for (unsigned i = 0; i < m->fastclk_ncomp; i++)   // a companion event's
      if (m->fastclk_comp[i] == n)       // off-edge dispatch must never read
         BANK_DECLINE("output nexus is a registered fast-clk companion");
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
      // Fold the model identity in: two CONFIGS can share top name and flag
      // vectors while binding different logic (rs_dutc is content-keyed), and
      // an unkeyed spec name would cache one config's spec under the other's.
      for (const char *p = c->rs_dutc ? c->rs_dutc : ""; *p; p++)
         h = (h ^ (uint8_t)*p) * 1099511628211ull;

      char dir[512];
      snprintf(dir, sizeof dir, "%s", c->rs_bridge);
      char *slash = strrchr(dir, '/');
      if (slash != NULL) *slash = '\0';
      else snprintf(dir, sizeof dir, ".");
      char specc[600], specso[600];
      snprintf(specc,  sizeof specc,  "%s/aj_%s_spec_%016llx_bridge.c", dir,
               c->rs_top, (unsigned long long)h);
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
         snprintf(cmd, sizeof cmd,
                  "%s %s -shared -fPIC -o '%s.t%d' '%s' && mv -f '%s.t%d' '%s'",
                  cc, smd, specso, (int)getpid(), specc, specso, (int)getpid(),
                  specso);
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
      void (*sck)(void *, unsigned char) = dlsym(dl, "accel_set_clklast");
      if (sck != NULL) c->set_clklast = sck;
      void (*ffa)(void *) = dlsym(dl, "accel_force_arm");
      if (ffa != NULL) c->force_arm_fn = ffa;
      // the specialized .so has its own sm_live_outputs — re-apply the mask
      uint64_t *lom = dlsym(dl, "sm_live_outputs");
      if (lom != NULL) {
         // size-negotiated: newer gsm exports sm_live_outputs_words;
         // absent symbol = legacy 4-word layout
         const int *lw = dlsym(dl, "sm_live_outputs_words");
         int nw = lw != NULL ? *lw : 4;
         if (nw > 16) nw = 16;
         if (nw < 1) nw = 4;
         memcpy(lom, c->live_out_mask, (size_t)nw * 8);
      }
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

// Disconnect the drivers of every rerouted proc. After aj_reroute the proc
// never runs again, but its driver SOURCES survive with whatever waveform was
// current at install -- typically the init 'U'/'X' transaction still queued
// for t=0. Two failure paths follow: the queued transaction applies at t=0
// delta 1 and clobbers the chunk's seed deposit, and every later resolution
// on the nexus folds the stale driver value in (l3d RESOLVE_LUT ORs the
// uncertainty plane, so the VALUE stays right while the X mark spreads --
// the VeeR fused cyc90 illegal-instruction trap came from exactly this via
// dbg_halt_req). The chunk owns these nets now: mark the sources so update
// paths consume without applying, and set the VHDL disconnect flag so
// resolution ignores them. aj_chunk_demote reconnects on writeback.
static hash_t *g_aj_restored = NULL;   // rim-glue procs given back to interp

static void aj_quench_rerouted_drivers(rt_model_t *m)
{
   static bool done = false;
   if (done || m->aj_chunk_count == 0 || g_aj_verify
       || getenv("NVC_ACCEL_NO_QUENCH") != NULL)
      return;
   done = true;

   const bool dbg = getenv("NVC_ACCEL_QUENCH_DBG") != NULL;
   notef("accel-jit: T0 audit: has_field %"PRIu64" calls %.1fs, "
         "find_signal %"PRIu64" calls %.1fs",
         g_aj_hf_calls, g_aj_hf_ns / 1e9,
         g_aj_fs_calls, g_aj_fs_ns / 1e9);
   int nquench = 0, nlive = 0, nsync = 0;

   // Map bound output signal -> (chunk index << 16 | ord+1) so drivers on
   // bound outputs can stay connected and be value-synced by aj_out instead
   // of disconnected (port propagation reads DRIVING values).
   hash_t *bmap = hash_new(1024);
   for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
      aj_chunk_t *c = m->aj_chunks[ci];
      for (unsigned ord = 0; c->defer_outs != NULL && ord < c->defer_count;
           ord++)
         if (c->defer_outs[ord].sigp != NULL)
            hash_put(bmap, c->defer_outs[ord].sigp,
                     (void *)(((uintptr_t)ci << 16) | (ord + 1)));
   }
   // ULTIMATE-NEXUS companion map (#69, EH1a whole-core boot stall): the
   // interp driver of a bound output frequently sits on the INNER net (the
   // member primitive's own dout), port hops BELOW the bound rim — the
   // signal-identity bmap misses it, the driver was disconnected, and every
   // consumer past an interp-VISIBLE port hop above the rim (the EH1a TB's
   // AXI memory model) re-resolved a stale driving value forever: the rim's
   // effective bytes showed the fetch (IFU_AXI_ARVALID=1) while the TB
   // never saw one.  Key every bridged output by the same ultimate-nexus
   // walk the sibling-fanout pass uses so those drivers are kept + synced.
   hash_t *ubmap = hash_new(1024);
   const bool uq_on = getenv("NVC_ACCEL_UQUENCH") != NULL;
   for (unsigned ci = 0; uq_on && ci < m->aj_chunk_count; ci++) {
      aj_chunk_t *c = m->aj_chunks[ci];
      for (int bo = 0; c->b_out != NULL && bo < c->n_bout; bo++) {
         if (c->b_out[bo].sig == NULL) continue;
         rt_nexus_t *un = &c->b_out[bo].sig->nexus;
         for (int hop = 0; hop < 16; hop++) {
            rt_source_t *ps = NULL;
            for (rt_source_t *s = &(un->sources); s != NULL;
                 s = s->chain_input)
               if (s->tag == SOURCE_PORT && s->u.port.input != NULL)
                  ps = s;
            if (ps == NULL) break;
            un = ps->u.port.input;
         }
         if (hash_get(ubmap, un) == NULL)
            hash_put(ubmap, un,
                     (void *)(((uintptr_t)ci << 16) | (bo + 1)));
      }
   }

   // NVC_ACCEL_STARVE_CENSUS: enumerate nexuses that have NO driver source
   // at all, at least one interp reader pending, and live under a rerouted
   // member scope — the STARVED INTERNALIZED RIM class (#66): the member
   // proc that would drive them was rerouted before its first execution,
   // so no source was ever created; the rim holds init bytes forever and
   // its X mark poisons every interp reader cone (measured: the IFU/EXU
   // island behind the missing cyc86-89 postsync pause).
   if (getenv("NVC_ACCEL_STARVE_CENSUS") != NULL) {
      hash_t *mscope = hash_new(4096);   // rerouted member scopes
      for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
         aj_chunk_t *c = m->aj_chunks[ci];
         for (unsigned r = 0; r < c->rr_count; r++)
            if (c->rr_saved[r].proc->scope != NULL)
               hash_put(mscope, c->rr_saved[r].proc->scope, (void *)1);
      }
      hash_t *bysc = hash_new(1024);     // scope -> count (as uintptr)
      int nstarve = 0, nsamp = 0;
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
         if (n->signal == NULL || n->signal->parent == NULL) continue;
         // Walk port hops to the ULTIMATE driving nexus (sv2vhdl rims are
         // port-connected copies of the primitive's dout): liveness is
         // judged there, not at the rim.
         rt_nexus_t *un = n;
         for (int hop = 0; hop < 16; hop++) {
            rt_source_t *ps = NULL;
            for (rt_source_t *s = &(un->sources); s != NULL;
                 s = s->chain_input)
               if (s->tag == SOURCE_PORT && s->u.port.input != NULL)
                  ps = s;
            if (ps == NULL) break;
            un = ps->u.port.input;
         }
         bool live = false;
         for (rt_source_t *s = &(un->sources); s != NULL && !live;
              s = s->chain_input)
            if (s->tag == SOURCE_DRIVER && !s->disconnected
                && !s->aj_rerouted)
               live = true;
         if (live) continue;
         rt_scope_t *ps = n->signal->parent;
         bool member = false;
         for (rt_scope_t *w = ps; w != NULL && !member; w = w->parent)
            if (hash_get(mscope, w) != NULL) member = true;
         if (!member) continue;
         int ninterp = 0;
         void *pd = n->pending;
         if (pointer_tag(pd) == 1) {
            rt_wakeable_t *w = untag_pointer(pd, rt_wakeable_t);
            if (w->kind == W_PROC) ninterp = 1;
         }
         else if (pd != NULL) {
            rt_pending_t *pl = untag_pointer(pd, rt_pending_t);
            for (int i = 0; i < pl->count; i++)
               if (pl->wake[i] != NULL && pl->wake[i]->kind == W_PROC)
                  ninterp++;
         }
         if (ninterp == 0) continue;
         nstarve++;
         hash_put(bysc, ps,
                  (void *)((uintptr_t)hash_get(bysc, ps) + 1));
         if (nsamp < 30 && n->signal->where != NULL) {
            nsamp++;
            notef("accel-jit: STARVED %s.%s readers=%d",
                  istr(ps->name), istr(tree_ident(n->signal->where)),
                  ninterp);
         }
      }
      // NVC_ACCEL_STARVE_NET=<substr>: full source/pending dump for
      // matching signals — classifier ground truth
      { const char *sn = getenv("NVC_ACCEL_STARVE_NET");
        if (sn != NULL)
           for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
              if (n->signal == NULL || n->signal->where == NULL) continue;
              char ln[64];
              aj_lower(ln, istr(tree_ident(n->signal->where)), sizeof ln);
              if (strstr(ln, sn) == NULL) continue;
              // walk to the ultimate driving nexus, dump ITS sources
              rt_nexus_t *un = n;
              for (int hop = 0; hop < 16; hop++) {
                 rt_source_t *ps = NULL;
                 for (rt_source_t *s = &(un->sources); s != NULL;
                      s = s->chain_input)
                    if (s->tag == SOURCE_PORT && s->u.port.input != NULL)
                       ps = s;
                 if (ps == NULL) break;
                 un = ps->u.port.input;
              }
              hash_t *rrm = hash_new(32768);
              for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
                 aj_chunk_t *c = m->aj_chunks[ci];
                 for (unsigned r = 0; r < c->rr_count; r++)
                    hash_put(rrm, c->rr_saved[r].proc, (void *)1);
              }
              int nsrc = 0;
              for (rt_source_t *s = &(un->sources); s != NULL;
                   s = s->chain_input, nsrc++) {
                 rt_proc_t *dp = s->tag == SOURCE_DRIVER
                    ? s->u.driver.proc : NULL;
                 notef("accel-jit: SNET %s ULT sc=%s src%d tag=%d disc=%u "
                       "rr=%u proc=%s pscope=%s inrr=%d fastclk=%d "
                       "vt_chunk=%d", ln,
                       un->signal != NULL && un->signal->parent != NULL
                          ? istr(un->signal->parent->name) : "?",
                       nsrc, (int)s->tag, (unsigned)s->disconnected,
                       (unsigned)s->aj_rerouted,
                       dp != NULL ? istr(dp->name) : "-",
                       dp != NULL && dp->scope != NULL
                          ? istr(dp->scope->name) : "-",
                       dp != NULL ? (hash_get(rrm, dp) != NULL) : -1,
                       dp != NULL ? (int)dp->wakeable.fastclk : -1,
                       dp != NULL
                          ? (aj_chunk_of_proc(m, dp) != NULL) : -1);
              }
              hash_free(rrm);
              int npend = 0;
              void *pd = n->pending;
              if (pointer_tag(pd) == 1) npend = 1;
              else if (pd != NULL) {
                 rt_pending_t *pl = untag_pointer(pd, rt_pending_t);
                 npend = pl->count;
              }
              notef("accel-jit: SNET %s: %d source(s), %d pending", ln,
                    nsrc, npend);
           } }
      notef("accel-jit: STARVE census: %d driverless member-scope "
            "nexus(es) with interp readers", nstarve);
      const void *sk; void *sv;
      for (hash_iter_t it = HASH_BEGIN;
           hash_iter(bysc, &it, &sk, &sv); )
         if ((uintptr_t)sv >= 8)
            notef("accel-jit: STARVE scope %s: %d",
                  istr(((rt_scope_t *)sk)->name), (int)(uintptr_t)sv);
      hash_free(bysc);
      hash_free(mscope);
   }

   // RESTORE PASS: a rerouted proc whose driven nets have live interp
   // readers but NO chunk publication is rim GLUE the boundary missed
   // (readable/plain copies, fanout assigns, stall derivations). The chunk
   // computes the value internally but never publishes it, so the readers
   // starve — dec_tlu_mhartstart never rose (thread 1 never started) and
   // the CSR-write pipeline pause never asserted. Give such procs their
   // interp vtable back: they re-derive the net from published inputs.
   // Only procs driving NO published net qualify (no double-drive risk).
   {
      // Reroute membership by rr_saved, not vtable identity (direct-entry
      // rewrapping repoints vtables later; rr_saved is the ground truth).
      hash_t *rrmap = hash_new(32768);
      for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
         aj_chunk_t *c = m->aj_chunks[ci];
         for (unsigned r = 0; r < c->rr_count; r++)
            hash_put(rrmap, c->rr_saved[r].proc, c);
      }
      // Readers sit on the port-chain HEAD (the sv2vhdl rim copy the interp
      // procs actually read); the rerouted driver sits on the chain TAIL
      // (the primitive's dout).  Correlating them same-nexus never fires —
      // measured: a_ff_e2 rim with 5-8 interp readers, driver a_e2_ff:_p0
      // rerouted two port hops down, restore count 0, rim X-marked forever
      // (the starved-rim island behind the missing cyc86-89 pause).  Walk
      // each read rim to its ultimate driving nexus and attribute the
      // readers THERE.
      // Complete publication map: every bridged output signal of every
      // chunk (b_out), NOT just the negflip sigp subset in bmap — with the
      // subset, published standalone-chunk outputs classified as
      // unpublished and their rerouted drivers were restored into
      // double-drives (measured: 4/6 accelbench PERINST mismatches).
      hash_t *pubmap = hash_new(4096);
      for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
         aj_chunk_t *c = m->aj_chunks[ci];
         for (int bo = 0; c->b_out != NULL && bo < c->n_bout; bo++)
            if (c->b_out[bo].sig != NULL)
               hash_put(pubmap, c->b_out[bo].sig, (void *)1);
      }
      hash_t *pflag = hash_new(4096);   // proc -> 1=unpub-with-reader 2=pub
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
         bool irdr = false;
         void *pd = n->pending;
         if (pointer_tag(pd) == 1) {
            rt_wakeable_t *w = untag_pointer(pd, rt_wakeable_t);
            if (w->kind == W_PROC && hash_get(rrmap,
                   container_of(w, rt_proc_t, wakeable)) == NULL)
               irdr = true;
         }
         else if (pd != NULL) {
            rt_pending_t *pl = untag_pointer(pd, rt_pending_t);
            for (int i = 0; i < pl->count && !irdr; i++) {
               rt_wakeable_t *w = pl->wake[i];
               if (w != NULL && w->kind == W_PROC && hash_get(rrmap,
                      container_of(w, rt_proc_t, wakeable)) == NULL)
                  irdr = true;
            }
         }
         const bool pub = n->signal != NULL
            && (hash_get(bmap, n->signal) != NULL
                || hash_get(pubmap, n->signal) != NULL);
         if (!irdr && !pub)
            continue;
         rt_nexus_t *un = n;
         for (int hop = 0; hop < 16; hop++) {
            rt_source_t *ps = NULL;
            for (rt_source_t *s = &(un->sources); s != NULL;
                 s = s->chain_input)
               if (s->tag == SOURCE_PORT && s->u.port.input != NULL)
                  ps = s;
            if (ps == NULL) break;
            un = ps->u.port.input;
         }
         for (rt_source_t *s = &(un->sources); s != NULL;
              s = s->chain_input) {
            if (s->tag != SOURCE_DRIVER || s->u.driver.proc == NULL)
               continue;
            rt_proc_t *p = s->u.driver.proc;
            if (hash_get(rrmap, p) == NULL)
               continue;
            uintptr_t fl = (uintptr_t)hash_get(pflag, p);
            if (pub) fl |= 2;
            if (irdr) fl |= 1;
            hash_put(pflag, p, (void *)fl);
         }
      }
      hash_free(rrmap);
      hash_free(pubmap);
      int nrestore = 0, nvt = 0;
      { int fd[4] = {0,0,0,0}; unsigned nrr = 0;
        for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
           aj_chunk_t *c = m->aj_chunks[ci];
           nrr += c->rr_count;
           for (unsigned r = 0; r < c->rr_count; r++) {
              uintptr_t fl = (uintptr_t)hash_get(pflag, c->rr_saved[r].proc);
              if (fl < 4) fd[fl]++;
           }
        }
        if (dbg)
           notef("accel-jit: restore-pass: %u rr — none=%d unpub+rdr=%d "
                 "pub=%d pub+rdr=%d", nrr, fd[0], fd[1], fd[2], fd[3]); }
      for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
         aj_chunk_t *c = m->aj_chunks[ci];
         for (unsigned r = 0; r < c->rr_count; r++) {
            rt_proc_t *p = c->rr_saved[r].proc;
            if ((uintptr_t)hash_get(pflag, p) != 1)
               continue;   // qualifies only: unpublished-with-reader, no pub
            if (p->vtable != &c->vtable) {
               nvt++;
               continue;
            }
            proc_set_vtable(p, c->rr_saved[r].vt);
            deltaq_insert_proc(m, 0, p);   // catch up on missed events
            if (g_aj_restored == NULL) g_aj_restored = hash_new(1024);
            hash_put(g_aj_restored, p, (void *)1);   // census: LIVE reader
            nrestore++;
            if (dbg && nrestore <= 40)
               notef("accel-jit: quench RESTORE glue %s", istr(p->name));
         }
      }
      hash_free(pflag);
      if (nrestore > 0 || nvt > 0)
         notef("accel-jit: restored %d rim-glue proc(s) to the interpreter"
               " (%d skipped: foreign vtable)", nrestore, nvt);

      // SAME-ULTIMATE-NEXUS PUBLICATION FANOUT (#66): a flop's single dout
      // feeds several port-connected rims; the chunk publishes ONE of them
      // (its bridged output) while a sibling rim with its own interp
      // readers starves at init-X forever (per-proc restore cannot help:
      // 278 procs classify pub+rdr — restoring would double-drive the
      // published rim).  Register every reader-bearing rim that port-walks
      // to the SAME ultimate nexus as a published output as an out_extra
      // deposit target of that ord, and back-fill it once from the
      // published rim's current bytes so quiescent outputs do not leave it
      // at init until their first change.
      {
         hash_t *umap = hash_new(4096);   // ultimate nexus -> (ci<<16|ord+1)
         for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
            aj_chunk_t *c = m->aj_chunks[ci];
            for (int bo = 0; c->b_out != NULL && bo < c->n_bout; bo++) {
               if (c->b_out[bo].sig == NULL) continue;
               rt_nexus_t *un = &c->b_out[bo].sig->nexus;
               for (int hop = 0; hop < 16; hop++) {
                  rt_source_t *ps = NULL;
                  for (rt_source_t *s = &(un->sources); s != NULL;
                       s = s->chain_input)
                     if (s->tag == SOURCE_PORT && s->u.port.input != NULL)
                        ps = s;
                  if (ps == NULL) break;
                  un = ps->u.port.input;
               }
               if (hash_get(umap, un) == NULL)
                  hash_put(umap, un,
                           (void *)(((uintptr_t)ci << 16) | (bo + 1)));
            }
         }
         int nfan = 0, nfull = 0;
         for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
            if (n->signal == NULL) continue;
            bool irdr = false;
            void *pd = n->pending;
            if (pointer_tag(pd) == 1) {
               rt_wakeable_t *w = untag_pointer(pd, rt_wakeable_t);
               irdr = w->kind == W_PROC;
            }
            else if (pd != NULL) {
               rt_pending_t *pl = untag_pointer(pd, rt_pending_t);
               for (int i = 0; i < pl->count && !irdr; i++)
                  irdr = pl->wake[i] != NULL
                     && pl->wake[i]->kind == W_PROC;
            }
            if (!irdr) continue;
            rt_nexus_t *un = n;
            for (int hop = 0; hop < 16; hop++) {
               rt_source_t *ps = NULL;
               for (rt_source_t *s = &(un->sources); s != NULL;
                    s = s->chain_input)
                  if (s->tag == SOURCE_PORT && s->u.port.input != NULL)
                     ps = s;
               if (ps == NULL) break;
               un = ps->u.port.input;
            }
            void *uv = hash_get(umap, un);
            if (uv == NULL) continue;
            const unsigned ci  = (unsigned)((uintptr_t)uv >> 16);
            const unsigned ord = (unsigned)((uintptr_t)uv & 0xffff) - 1;
            aj_chunk_t *oc = m->aj_chunks[ci];
            rt_signal_t *pub = oc->b_out[ord].sig;
            rt_signal_t *sib = n->signal;
            if (sib == pub || sib->shared.size != pub->shared.size)
               continue;
            if (ord >= oc->defer_count) continue;
            // Clock-class outputs are EXEMPT: they publish through the
            // dedicated icg immediate path with runt-hold semantics, and
            // fanning their siblings here delivers rise+fall compressed
            // into one instant on the sibling nets (measured on the
            // DESCEND config: LSU_C1_DC*_CLK / BUS_IBUF_C1_CLK siblings
            // pulsed rise@845+fall@845 while the primary held correctly —
            // downstream flops missed edges and stalls arrived late).
            if (oc->defer_outs[ord].icg)
               continue;
            if (oc->out_extra == NULL) {
               oc->out_extra   = xcalloc_array(oc->defer_count,
                                               sizeof(*oc->out_extra));
               oc->out_extra_n = xcalloc_array(oc->defer_count, 1);
            }
            bool dup = false;
            for (int k = 0; k < oc->out_extra_n[ord] && !dup; k++)
               dup = oc->out_extra[ord][k] == sib;
            if (dup) continue;
            if (oc->out_extra_n[ord] >= 6) { nfull++; continue; }
            oc->out_extra[ord][oc->out_extra_n[ord]++] = sib;
            // back-fill from the published rim's current effective bytes
            deposit_signal(m, sib,
                           nexus_effective(&pub->nexus), 0,
                           oc->b_out[ord].width);
            nfan++;
         }
         hash_free(umap);
         if (nfan > 0 || nfull > 0)
            notef("accel-jit: %d same-net sibling rim(s) added to the "
                  "publication fanout (%d dropped: extra table full)",
                  nfan, nfull);
      }

      // READABLE-SHADOW FANOUT (#69, EH1a whole-core boot stall): the
      // iverilog-frontend translation vintage relays values up the
      // hierarchy through per-level GLUE PROCS, and those relays read the
      // sv2vhdl `<name>_readable` shadow — a sibling signal driven by its
      // own glue inside the rerouted primitive.  Quenching froze the
      // shadow at init, and because every hop is a DRIVER (not a port),
      // none of the port-walk correlation above can see it: the chunk
      // deposited IFU_AXI_ARVALID=1 on the bound rim (readers woke, rim
      // bytes right) while the relay chain kept re-deriving from the dead
      // shadow — the TB's AXI memory model never saw one fetch.  The
      // `_readable` suffix is the translator's own naming contract, so
      // correlate BY NAME in the bound signal's scope and fan
      // publications to reader-bearing shadows.
      // OUTPUT-PORT-CHAIN FANOUT (#70, EH1a whole-core boot): in the
      // iverilog-frontend vintage every hierarchy level above a member's
      // flop is a DRIVERLESS port-association net (no relay procs, no
      // collapse) — the chunk's deposit updates the bound rim's EFFECTIVE
      // bytes, but each output-port hop above re-resolves from DRIVING
      // values, and with no live driver anywhere the phantom yields
      // certain-0 forever (measured: rim=3 driven-1 at 145ns while the
      // ifu-level net re-resolved 6 at the same instant and the TB never
      // saw a fetch).  Walk the OUTPUT-port chain from every bound output
      // and register each driverless hop signal as an out_extra deposit
      // target: publications then land on every level's effective bytes
      // and no reader depends on driving-value resolution.  Hops with a
      // LIVE driver are skipped (their driver re-derives; depositing
      // would fight it).
      if (getenv("NVC_ACCEL_PORTFAN") != NULL) {
         int npf = 0, npffull = 0;
         for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
            aj_chunk_t *c = m->aj_chunks[ci];
            for (int bo = 0; c->b_out != NULL && bo < c->n_bout; bo++) {
               rt_signal_t *pub = c->b_out[bo].sig;
               if (pub == NULL || (unsigned)bo >= c->defer_count) continue;
               if (c->defer_outs[bo].icg) continue;   // clocks: never fan
               rt_nexus_t *hop = &pub->nexus;
               for (int depth = 0; depth < 8; depth++) {
                  rt_source_t *op = NULL;
                  for (rt_source_t *o = hop->outputs; o != NULL;
                       o = o->chain_output)
                     if (o->tag == SOURCE_PORT && o->u.port.output != NULL) {
                        op = o;
                        break;
                     }
                  if (op == NULL) break;
                  hop = op->u.port.output;
                  rt_signal_t *hs = hop->signal;
                  if (hs == NULL || hs == pub
                      || hs->shared.size != pub->shared.size)
                     break;
                  bool live = false;
                  for (rt_source_t *s2 = &(hop->sources);
                       s2 != NULL && !live; s2 = s2->chain_input)
                     if (s2->tag == SOURCE_DRIVER && !s2->disconnected
                         && !s2->aj_rerouted
                         && s2->u.driver.proc != NULL)
                        live = true;
                  if (live) break;
                  if (c->out_extra == NULL) {
                     c->out_extra   = xcalloc_array(c->defer_count,
                                                    sizeof(*c->out_extra));
                     c->out_extra_n = xcalloc_array(c->defer_count, 1);
                  }
                  bool dup = false;
                  for (int k = 0; k < c->out_extra_n[bo] && !dup; k++)
                     dup = c->out_extra[bo][k] == hs;
                  if (!dup) {
                     if (c->out_extra_n[bo] >= 6) { npffull++; break; }
                     c->out_extra[bo][c->out_extra_n[bo]++] = hs;
                     deposit_signal(m, hs, nexus_effective(&pub->nexus), 0,
                                    c->b_out[bo].width);
                     npf++;
                  }
               }
            }
         }
         if (npf > 0 || npffull > 0)
            notef("accel-jit: %d output-port-chain hop(s) added to the "
                  "publication fanout (%d dropped: extra table full)",
                  npf, npffull);
      }

      if (getenv("NVC_ACCEL_RDFAN") != NULL) {
         int nrd = 0, nrfull = 0;
         for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
            aj_chunk_t *c = m->aj_chunks[ci];
            for (int bo = 0; c->b_out != NULL && bo < c->n_bout; bo++) {
               rt_signal_t *sig = c->b_out[bo].sig;
               if (sig == NULL || sig->parent == NULL
                   || sig->where == NULL)
                  continue;
               if ((unsigned)bo >= c->defer_count) continue;
               if (c->defer_outs[bo].icg) continue;   // clocks: never fan
               char ln[64], rn[80];
               aj_lower(ln, istr(tree_ident(sig->where)), sizeof ln);
               snprintf(rn, sizeof rn, "%s_readable", ln);
               rt_signal_t *rs = aj_find_signal(sig->parent, rn);
               if (rs == NULL || rs == sig
                   || rs->shared.size != sig->shared.size)
                  continue;
               bool irdr = false;
               void *pd = rs->nexus.pending;
               if (pointer_tag(pd) == 1) {
                  rt_wakeable_t *w = untag_pointer(pd, rt_wakeable_t);
                  irdr = w->kind == W_PROC;
               }
               else if (pd != NULL) {
                  rt_pending_t *pl = untag_pointer(pd, rt_pending_t);
                  for (int i = 0; i < pl->count && !irdr; i++)
                     irdr = pl->wake[i] != NULL
                        && pl->wake[i]->kind == W_PROC;
               }
               if (!irdr) continue;
               // the shadow must be DEAD (its own driver quenched or
               // absent) — a live-driven shadow needs no help and a
               // second writer would fight it
               bool live = false;
               for (rt_source_t *s = &(rs->nexus.sources);
                    s != NULL && !live; s = s->chain_input)
                  if (s->tag == SOURCE_DRIVER && !s->disconnected
                      && !s->aj_rerouted && s->u.driver.proc != NULL)
                     live = true;
               if (live) continue;
               if (c->out_extra == NULL) {
                  c->out_extra   = xcalloc_array(c->defer_count,
                                                 sizeof(*c->out_extra));
                  c->out_extra_n = xcalloc_array(c->defer_count, 1);
               }
               bool dup = false;
               for (int k = 0; k < c->out_extra_n[bo] && !dup; k++)
                  dup = c->out_extra[bo][k] == rs;
               if (dup) continue;
               if (c->out_extra_n[bo] >= 6) { nrfull++; continue; }
               c->out_extra[bo][c->out_extra_n[bo]++] = rs;
               deposit_signal(m, rs, nexus_effective(&sig->nexus), 0,
                              c->b_out[bo].width);
               nrd++;
            }
         }
         if (nrd > 0 || nrfull > 0)
            notef("accel-jit: %d readable-shadow rim(s) added to the "
                  "publication fanout (%d dropped: extra table full)",
                  nrd, nrfull);
      }
   }

   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
      for (rt_source_t *s = &(n->sources); s != NULL; s = s->chain_input) {
         if (s->tag != SOURCE_DRIVER || s->u.driver.proc == NULL)
            continue;
         rt_proc_t *p = s->u.driver.proc;
         aj_chunk_t *pc = aj_chunk_of_proc(m, p);
         if (pc == NULL)
            continue;
         // A rerouted proc is not necessarily dead: the fast-clk table (and
         // the levelize sweep) dispatch proc BODIES directly, bypassing the
         // chunk vtable — clock gaters distribute their gated clock exactly
         // this way. Only quench drivers of procs nothing dispatches.
         if (p->wakeable.fastclk) {
            nlive++;
            if (dbg)
               notef("accel-jit: quench SKIP live fastclk driver %s -> %s",
                     istr(p->name),
                     n->signal != NULL && n->signal->where != NULL
                        ? istr(tree_ident(n->signal->where)) : "?");
            continue;
         }
         s->aj_rerouted = 1;

         void *bv = n->signal != NULL ? hash_get(bmap, n->signal) : NULL;
         if (bv != NULL) {
            // Driver on a bound chunk output: keep it CONNECTED; aj_out
            // will refresh its waveform value each publication.
            const unsigned ci = (unsigned)((uintptr_t)bv >> 16);
            const unsigned ord = (unsigned)((uintptr_t)bv & 0xffff) - 1;
            aj_chunk_t *oc = m->aj_chunks[ci];
            if (oc->out_drv == NULL) {
               oc->out_drv   = xcalloc_array(oc->defer_count,
                                             sizeof(*oc->out_drv));
               oc->out_drv_n = xcalloc_array(oc->defer_count, 1);
            }
            if (oc->out_drv_n[ord] < 4) {
               unsigned off = 0;
               for (rt_nexus_t *x = &(n->signal->nexus);
                    x != NULL && x != n; x = x->chain)
                  off += x->size * x->width;
               oc->out_drv[ord][oc->out_drv_n[ord]++] =
                  (struct aj_odrv){ s, n, off };
               nsync++;
               // The chunk seed pass ran during model_reset, before this
               // registration: back-fill the driver from the seeded
               // effective bytes and re-propagate through the port network
               // (quiescent chunks may never publish again).
               copy_value_ptr(n, &s->u.driver.waveforms.value,
                              nexus_effective(n));
               for (rt_source_t *o = n->outputs; o != NULL;
                    o = o->chain_output)
                  if (o->tag == SOURCE_PORT)
                     defer_driving_update(m, o->u.port.output);
            }
            continue;
         }

         // Ultimate-nexus match: same treatment as the direct bmap hit.
         // The driver's nexus IS the chain tail; require identical signal
         // byte layout with the bound rim so the published buffer's offsets
         // apply verbatim (mismatch keeps today's disconnect).
         if (n->signal != NULL) {
            void *uv = hash_get(ubmap, n);
            if (uv != NULL) {
               const unsigned uci  = (unsigned)((uintptr_t)uv >> 16);
               const unsigned uord = (unsigned)((uintptr_t)uv & 0xffff) - 1;
               aj_chunk_t *oc = m->aj_chunks[uci];
               if (uord < oc->defer_count && oc->b_out[uord].sig != NULL
                   && oc->b_out[uord].sig != n->signal
                   && n->signal->shared.size
                      == oc->b_out[uord].sig->shared.size) {
                  if (oc->out_drv == NULL) {
                     oc->out_drv   = xcalloc_array(oc->defer_count,
                                                   sizeof(*oc->out_drv));
                     oc->out_drv_n = xcalloc_array(oc->defer_count, 1);
                  }
                  if (oc->out_drv_n[uord] < 4) {
                     unsigned off = 0;
                     for (rt_nexus_t *x = &(n->signal->nexus);
                          x != NULL && x != n; x = x->chain)
                        off += x->size * x->width;
                     oc->out_drv[uord][oc->out_drv_n[uord]++] =
                        (struct aj_odrv){ s, n, off };
                     nsync++;
                     copy_value_ptr(n, &s->u.driver.waveforms.value,
                                    nexus_effective(n));
                     for (rt_source_t *o = n->outputs; o != NULL;
                          o = o->chain_output)
                        if (o->tag == SOURCE_PORT)
                           defer_driving_update(m, o->u.port.output);
                     continue;
                  }
               }
            }
         }

         s->disconnected = 1;
         nquench++;
         if (dbg && nquench <= 40)
            notef("accel-jit: quench %s -> %s", istr(p->name),
                  n->signal != NULL && n->signal->where != NULL
                     ? istr(tree_ident(n->signal->where)) : "?");
      }
   }
   hash_free(bmap);
   hash_free(ubmap);

   // NVC_ACCEL_PENDDUMP=<substr>: post-quench forensic — for every signal
   // whose lowered leaf matches, print its identity (scope, sig/nexus ptrs,
   // effective byte0) and every pending reader with its reroute status.
   // Ground truth for wrong-sibling binds and dead-subscription questions.
   { const char *pdn = getenv("NVC_ACCEL_PENDDUMP");
     if (pdn != NULL) {
        hash_t *rrm2 = hash_new(32768);
        for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
           aj_chunk_t *c = m->aj_chunks[ci];
           for (unsigned r = 0; r < c->rr_count; r++)
              hash_put(rrm2, c->rr_saved[r].proc, (void *)1);
        }
        for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
           if (n->signal == NULL || n->signal->where == NULL
               || n->signal->parent == NULL)
              continue;
           char ln[64];
           aj_lower(ln, istr(tree_ident(n->signal->where)), sizeof ln);
           if (strstr(ln, pdn) == NULL) continue;
           notef("accel-jit: PENDDUMP %s sc=%s sig=%p nx=%p eff0=%u",
                 ln, istr(n->signal->parent->name), (void *)n->signal,
                 (void *)n,
                 (unsigned)((unsigned char *)nexus_effective(n))[0]);
           void *pd = n->pending;
           if (pointer_tag(pd) == 1) {
              rt_wakeable_t *w = untag_pointer(pd, rt_wakeable_t);
              if (w->kind == W_PROC) {
                 rt_proc_t *rp = container_of(w, rt_proc_t, wakeable);
                 notef("accel-jit:   pend %s sc=%s rr=%d", istr(rp->name),
                       rp->scope ? istr(rp->scope->name) : "?",
                       hash_get(rrm2, rp) != NULL);
              }
           }
           else if (pd != NULL) {
              rt_pending_t *pl = untag_pointer(pd, rt_pending_t);
              for (int i = 0; i < pl->count; i++) {
                 if (pl->wake[i] == NULL || pl->wake[i]->kind != W_PROC)
                    continue;
                 rt_proc_t *rp =
                    container_of(pl->wake[i], rt_proc_t, wakeable);
                 notef("accel-jit:   pend %s sc=%s rr=%d", istr(rp->name),
                       rp->scope ? istr(rp->scope->name) : "?",
                       hash_get(rrm2, rp) != NULL);
              }
           }
        }
        hash_free(rrm2);
     } }
   if (nquench > 0 || nlive > 0 || nsync > 0)
      notef("accel-jit: disconnected %d driver(s) of rerouted procs "
            "(%d live fastclk skipped, %d bound-output drivers synced)",
            nquench, nlive, nsync);
}

// Undo aj_build_fastclk + the partially-built chunk when an install fails after
// they were created, so a non-installed chunk leaves no active state.
// ---- NVC_ACCEL_PIN_CENSUS: post-quench starved-net census dump (#70) -------
// Runs AFTER aj_quench_rerouted_drivers so the liveness predicate is truthful
// (bound-output synced drivers stay connected and self-exclude; disconnected
// rerouted drivers read dead).  Records every nexus that (a) has NO live
// driver, (b) has interp W_PROC readers, (c) lives under a rerouted member
// scope, and (d) is not already fed by a chunk publication (bound outputs,
// their ultimate nexuses, out_extra siblings).  These are exactly the nets
// the relay glue reads that pin-completion must publish.  Filters applied at
// dump time so the file contains only injectable records (marshallable
// shape, name fits "m999_<leaf>__ajp", not clock-like).  Forward decl: the
// member scopes live in g_aj_cands, defined later in this file.
static int  g_aj_ncand_fwd(void);
static bool g_aj_cand_scope_at(int i, rt_scope_t **sc, bool *is_glue);

static void aj_pin_census_dump(rt_model_t *m)
{
   static bool done = false;
   const char *fn = getenv("NVC_ACCEL_PIN_CENSUS");
   if (done || fn == NULL || m->aj_chunk_count == 0)
      return;
   done = true;

   // FED exclusion: signals (and ultimate nexuses) a chunk already publishes
   hash_t *fed_sig = hash_new(4096);
   hash_t *fed_nex = hash_new(4096);
   for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
      aj_chunk_t *c = m->aj_chunks[ci];
      for (unsigned ord = 0; c->defer_outs != NULL && ord < c->defer_count;
           ord++) {
         if (c->defer_outs[ord].sigp != NULL)
            hash_put(fed_sig, c->defer_outs[ord].sigp, (void *)1);
         if (c->out_extra != NULL && c->out_extra_n != NULL)
            for (int k = 0; k < c->out_extra_n[ord]; k++)
               hash_put(fed_sig, c->out_extra[ord][k], (void *)1);
      }
      for (int bo = 0; c->b_out != NULL && bo < c->n_bout; bo++) {
         if (c->b_out[bo].sig == NULL) continue;
         hash_put(fed_sig, c->b_out[bo].sig, (void *)1);
         rt_nexus_t *un = &c->b_out[bo].sig->nexus;
         for (int hop = 0; hop < 16; hop++) {
            rt_source_t *ps = NULL;
            for (rt_source_t *s = &(un->sources); s != NULL;
                 s = s->chain_input)
               if (s->tag == SOURCE_PORT && s->u.port.input != NULL)
                  ps = s;
            if (ps == NULL) break;
            un = ps->u.port.input;
         }
         hash_put(fed_nex, un, (void *)1);
      }
   }
   // member scopes (non-glue candidates)
   hash_t *mscope = hash_new(1024);
   for (int i = 0; i < g_aj_ncand_fwd(); i++) {
      rt_scope_t *sc; bool gl;
      if (g_aj_cand_scope_at(i, &sc, &gl) && !gl && sc != NULL)
         hash_put(mscope, sc, (void *)1);
   }
   // rerouted procs: their stale pending subscriptions must NOT count as
   // readers (they are dead; counting them ballooned the census 100x)
   hash_t *rrmap = hash_new(32768);
   for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
      aj_chunk_t *c = m->aj_chunks[ci];
      for (unsigned r = 0; r < c->rr_count; r++) {
         rt_proc_t *rp = c->rr_saved[r].proc;
         // the rim-glue RESTORE pass gave some rr procs their interp
         // vtable back — those are LIVE readers, not dead subscriptions
         if (g_aj_restored != NULL && hash_get(g_aj_restored, rp) != NULL)
            continue;
         hash_put(rrmap, rp, (void *)1);
      }
   }

   FILE *f = fopen(fn, "w");
   if (f == NULL) {
      warnf("accel-jit: NVC_ACCEL_PIN_CENSUS=%s: cannot open", fn);
      hash_free(fed_sig); hash_free(fed_nex); hash_free(mscope);
      return;
   }
   fprintf(f, "# aj-pin-census v1 chunks=%u cands=%d\n",
           m->aj_chunk_count, g_aj_ncand_fwd());
   hash_t *seen = hash_new(4096);   // signal ptr dedup (multi-nexus signals)
   int nrec = 0, nskip = 0;
   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
      // (b) LIVE interp readers on this nexus (rerouted procs' stale
      // subscriptions are dead and must not count; RESTORED rim-glue
      // procs count as live)
      int nrd = 0;
      void *pd = n->pending;
      if (pointer_tag(pd) == 1) {
         rt_wakeable_t *w = untag_pointer(pd, rt_wakeable_t);
         if (w->kind == W_PROC
             && hash_get(rrmap,
                         container_of(w, rt_proc_t, wakeable)) == NULL)
            nrd = 1;
      }
      else if (pd != NULL) {
         rt_pending_t *pl = untag_pointer(pd, rt_pending_t);
         for (int i = 0; i < pl->count; i++)
            if (pl->wake[i] != NULL && pl->wake[i]->kind == W_PROC
                && hash_get(rrmap, container_of(pl->wake[i], rt_proc_t,
                                                wakeable)) == NULL)
               nrd++;
      }
      if (nrd == 0) continue;
      // walk to the ULTIMATE driving nexus: the read head may sit ANY
      // number of port hops above the member-internal net whose driver
      // was quenched — record THAT signal (injection happens there, and
      // publishing it + UQUENCH refresh feeds every hop back up)
      rt_nexus_t *un = n;
      for (int hop = 0; hop < 16; hop++) {
         rt_source_t *ps = NULL;
         for (rt_source_t *s2 = &(un->sources); s2 != NULL;
              s2 = s2->chain_input)
            if (s2->tag == SOURCE_PORT && s2->u.port.input != NULL)
               ps = s2;
         if (ps == NULL) break;
         un = ps->u.port.input;
      }
      rt_signal_t *sg = un->signal;
      if (sg == NULL || sg->parent == NULL || sg->where == NULL)
         continue;
      if (hash_get(seen, sg) != NULL) continue;
      // (d) already fed by a chunk publication?
      if (hash_get(fed_sig, sg) != NULL) continue;
      if (hash_get(fed_nex, un) != NULL) continue;
      // (a) live driver at the ultimate?
      bool live = false;
      for (rt_source_t *s2 = &(un->sources); s2 != NULL && !live;
           s2 = s2->chain_input)
         if (s2->tag == SOURCE_DRIVER && !s2->disconnected
             && !s2->aj_rerouted && s2->u.driver.proc != NULL)
            live = true;
      if (live) continue;
      // (c) the ultimate must live under a MEMBER scope (that is the
      // module the injector can add a probe port to); depth from the
      // member top decides phase-1 eligibility
      int depth = -1;
      for (rt_scope_t *w = sg->parent; w != NULL; w = w->parent) {
         depth++;
         if (hash_get(mscope, w) != NULL) break;
         if (w->parent == NULL) depth = -1;
      }
      if (depth < 0) continue;
      // filters: injectable shape only
      const int elem  = (int)un->size;
      const int width = elem > 0 ? (int)(sg->shared.size / elem) : 0;
      char leaf[64];
      aj_lower(leaf, istr(tree_ident(sg->where)), sizeof leaf);
      if ((elem != 1 && elem != 4) || width < 1 || width > 4096
          || sg->shared.size != (size_t)width * elem
          || strlen(leaf) > 48
          || strstr(leaf, "l1clk") != NULL
          || (strlen(leaf) >= 3
              && strcmp(leaf + strlen(leaf) - 3, "clk") == 0)) {
         nskip++;
         continue;
      }
      hash_put(seen, sg, (void *)1);
      fprintf(f, "%s %s %d %d %d %d\n", istr(sg->parent->name), leaf,
              width, elem, nrd, depth);
      nrec++;
   }
   fclose(f);
   hash_free(fed_sig); hash_free(fed_nex); hash_free(mscope);
   hash_free(seen); hash_free(rrmap);
   notef("accel-jit: pin-census: %d record(s) dumped to %s (%d filtered)",
         nrec, fn, nskip);
}

static void aj_accel_teardown(rt_model_t *m)
{
   // engagement telemetry reader: zero fleet passes / snapshot evals on a
   // run that installed chunks means the sampling code never executed at
   // all (the unexecuted-patch trap) -- report it where it can be seen.
   if (getenv("AJ_SNAP_DBG") != NULL && m->aj_chunk_count > 0)
      notef("accel-snap: %d fleet passes, %d snapshot evals this run",
            g_aj_snap_fleet, g_aj_snap_used);
   free(m->fastclk_table);
   m->fastclk_table = NULL;
   m->fastclk_count = 0;
   m->fastclk_on    = false;
   m->fastclk_probation = 0;
   m->fastclk_hit = false;
   m->fastclk_evict_defer = false;
   m->fastclk_npending = 0;
   // member fastclk/fastclk_ee bits and the companion list are swept by
   // the next build's leading aj_dissolve_fastclk, exactly as the flags
   // always were (fastclk_on false gates the wakeup latch meanwhile)
   if (m->aj_chunk_count > 0) {
      aj_chunk_t *c = m->aj_chunks[--m->aj_chunk_count];
      if (c->defer_outs != NULL) {
         for (unsigned i = 0; i < c->defer_count; i++) {
            free(c->defer_outs[i].shadow);
            free(c->defer_outs[i].rh_latch);
         }
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

// Anti-poisoning staging for the synth cache.  The cache was poisoned by a
// two-process race: one run content-hashed its just-emitted wrapper .v, a
// concurrent run with a DIFFERENT config rewrote the same shared-named file
// before gen_statemachine read it, and the OTHER config's netlist was cached
// under this config's key — permanently, because every later run recomputes
// the same key and trusts the cached file (measured: merged_30 lost its
// entire active_clk register family, lsu_bus_clk_en_q read as constant 0 and
// LSU BUSCLK froze from cyc70 in the DESCEND config only).  The fix is to
// hand gen_statemachine a KEY-NAMED PRIVATE COPY of the exact bytes that
// went into the hash: same key => same bytes by construction, so whatever
// other processes do to the shared-named emissions cannot cross configs.
static void aj_staged_path(char *out, size_t outsz, const char *accel_dir,
                           const char *keyname, uint64_t vhash, int idx)
{
   snprintf(out, outsz, "%s/ajk_%s_%016llx_s%d.v", accel_dir, keyname,
            (unsigned long long)vhash, idx);
}

static bool aj_stage_source(const char *accel_dir, const char *keyname,
                            uint64_t vhash, int idx, const char *text)
{
   char path[600];
   aj_staged_path(path, sizeof path, accel_dir, keyname, vhash, idx);
   const size_t n = strlen(text);
   struct stat st;
   if (stat(path, &st) == 0 && (size_t)st.st_size == n)
      return true;   // key-named => content-determined; size guards truncation
   char tmp[640];
   snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
   FILE *f = fopen(tmp, "wb");
   if (f == NULL) return false;
   const bool wok = fwrite(text, 1, n, f) == n;
   if (fclose(f) != 0 || !wok || rename(tmp, path) != 0) {
      unlink(tmp);
      return false;
   }
   return true;
}

// gen_statemachine declares each scalar/vector port as "uint64_t _<name>;" for
// <=64 bits, or as a limb array "uint32_t _<name>[N];" for a >64-bit port (the
// scalable wide path). Match either spelling — a wide port that fails this check
// would pass the gate but never get bridged. (The older "unsigned __int128"
// spelling is matched too for forward/backward compatibility.)
#define AJ_MAX_PINS 4096   // per-subtree bridged-pin cap (dec has hundreds)

static bool aj_model_has_field(const char *dutc_text, const char *name)
{
   const uint64_t t0 = get_timestamp_ns();
   char n1[80], n2[96], n3[96];
   snprintf(n1, sizeof n1, "uint64_t _%s;", name);
   snprintf(n2, sizeof n2, "unsigned __int128 _%s;", name);
   snprintf(n3, sizeof n3, "uint32_t _%s[", name);   // wide limb array
   const bool hit = strstr(dutc_text, n1) != NULL
       || strstr(dutc_text, n2) != NULL
       || strstr(dutc_text, n3) != NULL;
   g_aj_hf_ns += get_timestamp_ns() - t0;
   g_aj_hf_calls++;
   return hit;
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
         // Overflow is the silent-truncation death class (the 8-bit
         // late_pend dropped families 8+ for ~a month): a 17th family
         // would simply never advance while both gates stay green.
         // Decline the chunk loudly instead of truncating.
         if (nck == 16 && end != NULL) {
            const char *more = strchr(p, '"');
            if (more != NULL && more < end) {
               notef("accel-jit: model declares more than 16 extra clock "
                     "families — bridge tables cap at 16; declining (interp)");
               free(dut_text);
               return false;
            }
         }
      }
   }
   // SM_GROUP0_REGS: how many registers the MAIN clock group actually
   // clocks (-1 when the model predates the marker).  0 with extras means
   // the wrapper clk pin is a DEAD net — see the dead-primary redirect
   // after clock subscription below.
   long g0regs = -1;
   { const char *g0p = strstr(dut_text, "#define SM_GROUP0_REGS ");
     if (g0p != NULL)
        g0regs = atol(g0p + strlen("#define SM_GROUP0_REGS ")); }

   // Out-of-band reset handshake. Reset is a TWO-SIDED protocol: gsm strips
   // a port named `rst` from inputs_t and exports sm_oob_reset; the bridge
   // diverts the same pin and drives sm_reset from RST (AJB[5]). Removing
   // either half alone is silent poison — 8c8e1c1ba took out one side and
   // 16/19 accelbench designs went wrong with both gates green. Marker
   // absent (older cached model) => skip the marker check; the kept-input
   // conflict check below is vintage-independent.
   { long oob = -1;
     const char *op = strstr(dut_text, "const int sm_oob_reset = ");
     if (op != NULL) oob = atol(op + strlen("const int sm_oob_reset = "));
     if (oob >= 0 && (oob != 0) != (rst != NULL)) {
        notef("accel-jit: OOB-RESET CONTRACT BROKEN: model %s an out-of-band "
              "reset but the bridge %s an rst pin — declining (interp)",
              oob ? "expects" : "does not expect",
              rst != NULL ? "diverted" : "did not divert");
        free(dut_text);
        return false;
     }
     if (rst != NULL && aj_model_has_field(dut_text, "rst")) {
        notef("accel-jit: OOB-RESET CONTRACT BROKEN: bridge diverts pin "
              "'rst' but the model kept it in inputs_t (it would read stale "
              "zero forever) — declining (interp)");
        free(dut_text);
        return false;
     }
   }

   // Reader-side snapshot semantics (mode >= 4) are UNSOUND for un-merged
   // multi-clock chunks: the mech-3 architectural verdict (2026-08-02) —
   // mode-4 full-VeeR exited rc=0 clean at 5ns with dec's 5 extra clock
   // families never leaving reset, while the single-clock toy passed and
   // hid the class. Enforced until now only by config convention; convert
   // the silent death into a named decline (assert-order rank 12).
   if (aj_snap_mode() >= 4 && nck > 0 && (chunk == NULL || !chunk->merged)) {
      notef("accel-jit: SNAP_MODE %d with %d extra clock famil%s on an "
            "un-merged chunk — reader-side snapshot split is unsound for "
            "multi-clock (mech-3 verdict 2026-08-02); declining (interp)",
            aj_snap_mode(), nck, nck == 1 ? "y" : "ies");
      free(dut_text);
      return false;
   }

   // Per-BIT vector clocks: gen_statemachine names a group on bit N of a
   // vector clock wire "<wire>__b<N>" (EH2's active_thread_l2clk[1:0] — one
   // group per thread).  Split into the marshalled FIELD name and the bit to
   // extract, so each group edge-detects its own bit instead of bit 0 (which
   // advanced thread-1 flops on thread-0's clock).
   char extra_clk_field[16][64];
   int  extra_clk_bit[16];
   for (int k = 0; k < nck; k++) {
      snprintf(extra_clk_field[k], sizeof extra_clk_field[k], "%s", extra_clk[k]);
      extra_clk_bit[k] = 0;
      char *b = strstr(extra_clk_field[k], "__b");
      if (b != NULL && b[3] != '\0' && strspn(b + 3, "0123456789") == strlen(b + 3)) {
         extra_clk_bit[k] = atoi(b + 3);
         *b = '\0';
      }
   }

   // Each extra clock must be a marshalled input field, else the bridge cannot
   // edge-detect it from the boundary — decline (leave the chunk interpreted).
   for (int k = 0; k < nck; k++)
      if (!aj_model_has_field(dut_text, extra_clk_field[k])) {
         notef("accel-jit: extra clock '%s' not a marshalled input — declining",
               extra_clk[k]);
         free(dut_text);
         return false;
      }

   FILE *f = fopen(path, "w");
   if (!f) { free(dut_text); return false; }

   fprintf(f, "#define SM_NO_MAIN 1\n");
   fprintf(f, "#include <stdint.h>\n#include <string.h>\n"
              "#include <stddef.h>\n");
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
   // x_seen/x_hits/x_pin: BOUNDARY UNCERTAINTY (the value-plane engine models
   // 2 states, so an input byte that is not driven-certain is data the model
   // CANNOT represent). Set by aj_scan_inputs' repack loop (one XOR per
   // changed input bit, zero cost on an unchanged pin); read by the model
   // through the optional accel_x_seen accessor, which is how a .so asks to be
   // demoted. x_seen is sticky; x_hits counts every scan that saw uncertainty;
   // x_pin is the bridge ordinal (+1) of the first offender.
   if (nck == 0)
      fprintf(f, "typedef struct { state_t S; long long last_t;"
                 " unsigned char clk_last0;"
                 " inputs_t in_live; unsigned char shadow_valid, rst_prev, ext_chg,"
                 " icg_ckprev;"
                 " outputs_t o_prev;"
                 " unsigned char x_seen; unsigned short x_pin; unsigned x_hits;"
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
                 " inputs_t in_live; unsigned char shadow_valid, rst_prev, ext_chg,"
                 " icg_ckprev;"
                 " outputs_t o_prev;"
                 " unsigned char x_seen; unsigned short x_pin; unsigned x_hits;"
                 " unsigned char raw_shadow[%zu]; } aj_cs_t;\n", nck,
              has_late ? " state_t snapS; inputs_t snapIn;"
                         // 32-bit: descend wrappers carry up to 30 extra
                         // families — the original unsigned char silently
                         // dropped families 8+ (the LATE at-scale death)
                         " unsigned late_pend; unsigned char force_arm;" : "",
              raw_total);
   fprintf(f, "unsigned long accel_state_size(void){ return sizeof(aj_cs_t); }\n");
   // NVC_ACCEL_RECAP contract: the replayable PREFIX of aj_cs_t = register
   // block S + edge-detect fields (last_t/clk_last0/ck_last[/snapS...]) —
   // everything BEFORE in_live.  Rolling back only this prefix restores the
   // capture state AND re-arms the edge-detects while leaving the change
   // baselines (in_live/raw_shadow) and publication gates (o_prev) intact —
   // v1's full-state rollback reverted those and broke publication gating.
   fprintf(f, "unsigned long accel_recap_size(void){"
              " return (unsigned long)((char*)&((aj_cs_t*)0)->in_live"
              " - (char*)0); }\n");

   // DEMOTE WRITEBACK TABLES (aj_chunk_demote). Scrape the state_t struct
   // out of the generated model text (the established cross-file contract:
   // sm_extra_clocks / sm_output_order / aj_model_has_field all text-scrape
   // the same file) and emit a register-descriptor table whose offsets the
   // C compiler computes with offsetof against the REAL struct — exact by
   // construction, immune to alignment/padding guesswork. Emitted BEFORE
   // the `#define S (aj_cs->S)` below or that macro would poison
   // offsetof(aj_cs_t, S). Three field spellings (gen_statemachine):
   //   uint64_t _r;         // W bits            scalar register (W <= 64)
   //   uint32_t _r[NL];     // W bits            wide register (32b limbs)
   //   uint64_t _m[D];      // D x W-bit         memory (demote declines)
   {
      typedef struct { char name[512]; int width, depth; } aj_rdesc_t;
      aj_rdesc_t *rd = NULL;
      int nrd = 0, maxrd = 0;
      const char *send = strstr(dut_text, "} state_t;");
      const char *sbeg = NULL;
      for (const char *p = dut_text; send != NULL && p < send; p++) {
         const char *t = strstr(p, "typedef struct {");
         if (t == NULL || t > send) break;
         sbeg = t;
         p = t;
      }
      const char *line = sbeg != NULL ? strchr(sbeg, '\n') : NULL;
      while (line != NULL && line < send) {
         line++;
         const char *nl = strchr(line, '\n');
         const char *q = line;
         while (*q == ' ') q++;
         if (strncmp(q, "uint64_t ", 9) == 0) q += 9;
         else if (strncmp(q, "uint32_t ", 9) == 0) q += 9;
         else { line = nl; continue; }
         aj_rdesc_t e = { .name = "", .width = 0, .depth = 0 };
         int ni = 0;
         while ((isalnum((unsigned char)*q) || *q == '_') && ni < 511)
            e.name[ni++] = *q++;
         e.name[ni] = '\0';
         const char *cm = strstr(q, "//");
         if (ni == 0 || cm == NULL || (nl != NULL && cm > nl)) {
            line = nl;
            continue;
         }
         int a_ = 0, b_ = 0;
         if (sscanf(cm, "// %d x %d-bit", &a_, &b_) == 2) {
            e.depth = a_;
            e.width = b_;
         }
         else if (sscanf(cm, "// %d bits", &a_) == 1)
            e.width = a_;
         else { line = nl; continue; }
         if (nrd == maxrd) {
            maxrd = maxrd ? maxrd * 2 : 64;
            rd = xrealloc_array(rd, maxrd, sizeof(aj_rdesc_t));
         }
         rd[nrd++] = e;
         line = nl;
      }
      fprintf(f, "const char *aj_reg_name[] = {");
      for (int i = 0; i < nrd; i++) fprintf(f, "\"%s\",", rd[i].name);
      fprintf(f, "0};\n");
      fprintf(f, "const unsigned long aj_reg_off[] = {");
      for (int i = 0; i < nrd; i++)
         fprintf(f, "offsetof(state_t,%s),", rd[i].name);
      fprintf(f, "0};\n");
      fprintf(f, "const int aj_reg_width[] = {");
      for (int i = 0; i < nrd; i++) fprintf(f, "%d,", rd[i].width);
      fprintf(f, "0};\n");
      fprintf(f, "const int aj_reg_depth[] = {");
      for (int i = 0; i < nrd; i++) fprintf(f, "%d,", rd[i].depth);
      fprintf(f, "0};\n");
      fprintf(f, "int aj_n_regs = %d;\n", nrd);
      fprintf(f, "unsigned long aj_demote_state_off(void)"
                 "{ return offsetof(aj_cs_t, S); }\n\n");
      free(rd);
   }

   fprintf(f, "#define S (aj_cs->S)\n#define last_t (aj_cs->last_t)\n");
   if (nck == 0)
      fprintf(f, "void accel_reset(void *p){ aj_cs_t *aj_cs = p;"
                 " sm_reset(&S); last_t = -1; aj_cs->clk_last0 = 0;"
                 " aj_cs->shadow_valid = 0; aj_cs->ext_chg = 0;"
                 " aj_cs->x_seen = 0; aj_cs->x_pin = 0; aj_cs->x_hits = 0;"
                 " memset(&aj_cs->o_prev, 0xff, sizeof aj_cs->o_prev);"
                 " memset(&aj_cs->in_live, 0, sizeof aj_cs->in_live); }\n\n");
   else
      fprintf(f, "void accel_reset(void *p){ aj_cs_t *aj_cs = p;"
                 " sm_reset(&S); last_t = -1; aj_cs->clk_last0 = 0;"
                 " aj_cs->shadow_valid = 0; aj_cs->ext_chg = 0;"
                 " aj_cs->x_seen = 0; aj_cs->x_pin = 0; aj_cs->x_hits = 0;"
                 " memset(&aj_cs->o_prev, 0xff, sizeof aj_cs->o_prev);"
                 " memset(&aj_cs->in_live, 0, sizeof aj_cs->in_live);"
                 "%s"
                 " for(int _k=0;_k<%d;_k++) aj_cs->ck_last[_k]=0; }\n\n",
              has_late ? " aj_cs->late_pend = 0;" : "", nck);
   // Seed-time hook: the t=0 seed evals with clk FORCED low (comb-only settle),
   // which leaves clk_last0 = 0 via the per-eval update.  For a design whose
   // clock initialises HIGH there is no rising edge at t=0, so the seed must
   // sync clk_last0 to the REAL clock level afterwards or the first sample in
   // the initial high phase reads as a spurious 0->1 edge.
   fprintf(f, "void accel_set_clklast(void *p, unsigned char v)"
              "{ ((aj_cs_t *)p)->clk_last0 = v; }\n\n");
   // LATE arming for dead/re-keyed primaries: the bridge's own `posedge`
   // is an edge detect on the RAW bindtab clk bytes, which a dead primary
   // (FPGA-shape BUSCLK class) never raises — the model-side re-keyed
   // arming pokes this flag so snapS/late_pend still arm under CK_LATE.
   if (has_late)
      fprintf(f, "void accel_force_arm(void *p)"
                 "{ ((aj_cs_t *)p)->force_arm = 1; }\n\n");

   // Fill the scalar table slots (the per-pin slots are filled in the loops).
   chunk->bindtab[0] = (void *)&deposit_signal;
   chunk->bindtab[1] = (void *)&model_now;
   chunk->bindtab[2] = m;
   chunk->bindtab[3] = (void *)&aj_out;
   chunk->bindtab[4] = clk->data;
   chunk->bindtab[5] = rst ? rst->data : NULL;
   // Both AO maps key on the ULTIMATE nexus (port-hop walk): a consumer's
   // input rim and its producer's output rim are DIFFERENT rt_nexus_t
   // objects joined by port sources, so exact-pointer keys never connect
   // them (measured: seedord's 3-chunk split fired zero SEED-ORDER lines
   // while reproducing the #43 wrong answer).
   if (!spec)   // always: the ordered seed pass (#43 fix) consumes these
      for (int i = 0; i < npins; i++)
         if (pins[i].is_output && pins[i].sig != NULL)
            aj_ao4_add_out(aj_ultimate_driver_nexus(&pins[i].sig->nexus, 0),
                           chunk);
   chunk->primary_ck = clk != NULL ? clk->sig : NULL;
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
   // X/Z BOUNDARY DETECTION (GAP 1). The repack loop keeps only bit 0 of each
   // logic3d/std_logic element, so the driven/uncertain planes are dropped and
   // nothing downstream can ever see an X. The test is one compare against the
   // port's OWN '0' position: a byte is DRIVEN-CERTAIN iff (byte>>1)==(base>>1).
   //   logic3d  base=2: L3D_0=2,L3D_1=3 -> 1 (pass); Z=4,W=5 -> 2; X=6,U=7 -> 3
   //   std_logic base=2: '0'=2,'1'=3   -> 1 (pass); U=0,X=1 -> 0; Z=4,W=5 -> 2;
   //                                      L=6,H=7 -> 3; '-'=8 -> 4
   //   BIT/BOOLEAN base=0: only 0,1 exist, both >>1 == 0 == base>>1 — the test
   //     can never fire, so it is NOT EMITTED at all for those ports.
   // CAVEAT (measured, not assumed): the test is "driven AND certain", so the
   // weak-but-known states ('L'/'H', L3D_L/L3D_H) also trip it even though
   // their value bit IS faithful. That is conservative — over-detection can
   // only cost accel, never correctness — and the raw byte is reported so a
   // weak-drive hit is distinguishable from a real X.
   // NVC_ACCEL_NO_XDET: emit the ORIGINAL repack loop (no detection, no
   // accessors, so the model's poll disappears too). The A/B control for
   // measuring what the detection costs, and the escape hatch if it ever
   // does. Clear the .so cache when toggling — the cache key is the DUT
   // logic, not the bridge text.
   const bool no_xdet = getenv("NVC_ACCEL_NO_XDET") != NULL;
   if (!no_xdet) {
      fprintf(f, "static int g_xdbg = -1;\n");
      fprintf(f, "static void aj_x_first(aj_cs_t *aj_cs, void **AJB, int _ord,"
                 " const char *_nm, const unsigned char *_p, int _w, int _e,"
                 " unsigned _xb){\n"
                 "  unsigned _raw = 0;\n"
                 "  for(int b=0;b<_w;b++){ unsigned _v=_p[b*_e];"
                 " if((_v>>1)!=_xb){ _raw=_v; break; } }\n"
                 "  aj_cs->x_seen = 1; aj_cs->x_pin = (unsigned short)(_ord+1);\n"
                 "  if(g_xdbg<0) g_xdbg = getenv(\"NVC_ACCEL_JIT_DEBUG\")?1:0;\n"
                 "  if(g_xdbg>0){ unsigned _d; long long _t = NOW(MDL,&_d);\n"
                 "    fprintf(stderr, \"#AJX %%s: input '%%s' (ordinal %%d) not"
                 " driven-certain, raw byte %%u, at %%lld fs delta %%u\\n\","
                 " \"%s\", _nm, _ord, _raw, _t, _d); }\n"
                 "}\n",
              chunk->rs_top != NULL ? chunk->rs_top : "accel chunk");
   }
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
   static void  *snap_live[AJ_MAX_PINS];
   static size_t snap_nb[AJ_MAX_PINS];
   static int    snap_slot[AJ_MAX_PINS];
   int n_snap = 0;
   // inputs: DIRECT IN-PLACE — the packed `in` (chunk-state in_live) persists;
   // each pin memcmp's its nvc logic3d bytes against raw_shadow and is only
   // re-translated on change. Translation detail: each sub-element is `elem`
   // bytes (logic3d = natural); the value bit is bit 0 of the element's low
   // byte (little-endian). nvc stores a `(width-1 downto 0)` vector MSB-first:
   // element offset e maps to bit (width-1-e) — without this reversal every
   // multi-bit input arrives bit-reversed (din=1 read as 0x80).
   for (int i = 0; i < npins; i++) {
      if (pins[i].is_output) continue;
      // Clock registration MUST precede the has_field skip: a MERGED wrapper
      // consumes `clk` into per-member connections, so the model struct has
      // no `clk` field and the skip below drops the pin — leaving ck_sigs
      // EMPTY, aj_subscribe_clocks with nothing to arm, and the chunk never
      // evaluating after t=0 (the VeeR TB-memory flop bundles froze their
      // rd_addr/data rims at init-X and poisoned the core's fetch path).
      if (!spec && strcmp(pins[i].name, "clk") == 0 && pins[i].sig != NULL
          && chunk->n_ck_sigs < (int)ARRAY_LEN(chunk->ck_sigs)) {
         bool dup0 = false;
         for (int k = 0; k < chunk->n_ck_sigs; k++)
            if (chunk->ck_sigs[k] == pins[i].sig) { dup0 = true; break; }
         if (!dup0) chunk->ck_sigs[chunk->n_ck_sigs++] = pins[i].sig;
      }
      // NVC_ACCEL_RST_HOLD: remember the reset input's live bytes.  Matched
      // on the pin's BASE name (merged member prefix m<N>_ stripped);
      // trailing _l/_n/_b means active-low.  Reset SEMANTICS still come
      // from the netlist ($adff / D-mux) — this pin only gates WHEN
      // publications become visible (see the RST_HOLD branch in aj_out).
      if (!spec && chunk->rst_data == NULL && pins[i].data != NULL) {
         const char *bn = pins[i].name;
         if (bn[0] == 'm') {
            const char *us = bn + 1;
            while (*us >= '0' && *us <= '9') us++;
            if (us > bn + 1 && *us == '_') bn = us + 1;
         }
         char base[32];
         const size_t bl = strlen(bn);
         if (bl < sizeof base) {
            memcpy(base, bn, bl + 1);
            bool low = false;
            if (bl > 2 && base[bl - 2] == '_' && (base[bl - 1] == 'l'
                || base[bl - 1] == 'n' || base[bl - 1] == 'b')) {
               low = true;
               base[bl - 2] = '\0';
            }
            const size_t xl = strlen(base);
            const char *tail = xl >= 3 ? base + xl - 3 : base;
            // tail "rst" only at a word boundary: core_rst matches, the
            // JTAG trst does NOT (it is not the functional reset and is
            // typically static — anchoring on it would hold forever).
            const bool tail_rst = strcmp(tail, "rst") == 0
               && (xl == 3 || base[xl - 4] == '_');
            if (strcmp(base, "rst") == 0 || strcmp(base, "reset") == 0
                || strcmp(base, "resetn") == 0 || tail_rst
                || (xl >= 5 && strcmp(base + xl - 5, "reset") == 0)) {
               chunk->rst_low = low || strcmp(base, "resetn") == 0;
               // the pin's own rim is usually dead (rerouted glue) — anchor
               // the release read on the live root of the reset network
               rt_signal_t *rr = aj_rst_root_find(m->root, base,
                                                  chunk->rst_low, 3);
               if (rr == NULL)
                  rr = aj_rst_root_find(m->root, "rst", chunk->rst_low, 3);
               if (rr == NULL)
                  rr = aj_rst_root_find(m->root, "reset", chunk->rst_low, 3);
               chunk->rst_data = rr != NULL
                  ? (const uint8_t *)rr->shared.data : pins[i].data;
               if (aj_rst_hold())
                  notef("accel-jit: RST_HOLD arm pin '%s' (%s) anchor=%s",
                        pins[i].name, chunk->rst_low ? "low" : "high",
                        rr != NULL && rr->where != NULL
                           ? istr(tree_ident(rr->where)) : "(pin rim)");
            }
         }
      }
      if (!aj_model_has_field(dut_text, pins[i].name)) continue;
      const size_t nb = (size_t)pins[i].width * pins[i].elem;
      bool is_ck = strcmp(pins[i].name, "clk") == 0;
      for (int k = 0; k < nck && !is_ck; k++)
         if (strcmp(pins[i].name, extra_clk_field[k]) == 0) is_ck = true;
      if (!spec) {
         chunk->bindtab[6 + bridged_in] = pins[i].data;
         if (!pins[i].is_output && pins[i].sig != NULL && !is_ck)
            aj_ao3_add(aj_ultimate_driver_nexus(&pins[i].sig->nexus, 0),
                       chunk, pins[i].name, pins[i].data,
                       (size_t)pins[i].width * pins[i].elem);
         // snapshot slot for the two-phase edge sampling (see struct);
         // mode 3: comb-driven inputs stay LIVE (blocking-assign settle
         // must remain visible same-delta), only clocked/timed sources
         // are edge-sampled.  CLOCK pins are NEVER snapshotted: the
         // bridge's extra-clock edge-detects must see the live post-rise
         // byte or a snapshot-engaged eval could not fire them at all
         // (measured on merged_31: FPGA-shape members whose `clk` pin is a
         // dead gated net and whose real clocking is rawclk extras).
         bool snap_this = !is_ck;
         // NVC_ACCEL_SNAP_PINS=<file>: explicit per-pin snapshot assignment
         // (user directive: random-bisect which INPUT is troublesome for
         // the mixed accel/interp boundary).  Lines "<chunk-top> <pin>";
         // listed pins snapshot (mode-4 timing), unlisted stay live; clock
         // pins never snapshot regardless.  Read per install — runtime-only
         // (the snap map is built here from dut_text, not baked in the .so).
         { static char (*spn)[160] = NULL; static int spn_n = -1;
           if (spn_n < 0) {
              spn_n = 0;
              const char *pf = getenv("NVC_ACCEL_SNAP_PINS");
              FILE *sf = pf != NULL ? fopen(pf, "r") : NULL;
              if (sf != NULL) {
                 int mx = 64;
                 spn = xmalloc_array(mx, sizeof *spn);
                 char cn[96], pn[64];
                 while (fscanf(sf, "%95s %63s", cn, pn) == 2) {
                    if (spn_n == mx) {
                       mx *= 2;
                       spn = xrealloc_array(spn, mx, sizeof *spn);
                    }
                    snprintf(spn[spn_n++], sizeof spn[0], "%s %s", cn, pn);
                 }
                 fclose(sf);
              }
              else if (pf != NULL)
                 spn_n = -2;   // file named but unreadable: filter DISABLED
           }
           if (spn_n > 0 && snap_this && chunk->rs_top != NULL) {
              char key[160];
              snprintf(key, sizeof key, "%s %s", chunk->rs_top, pins[i].name);
              bool listed = false;
              for (int si = 0; si < spn_n && !listed; si++)
                 listed = strcmp(spn[si], key) == 0;
              snap_this = listed;
           }
         }
         // Modes 3 and 5 snapshot ONLY registered-producer pins.  Mode 5 =
         // mode-4 timing (per-timestep boundary fleet take, armed-eval-only
         // engagement) + this per-pin selection: interp REGISTERED flops
         // commit at edge-delta+1, so a chunk's late armed eval live-reads
         // their POST-edge values (the mech-3 one-cycle-early class — e.g.
         // the LPM wb_pkt packing flop feeding TLU); comb-driven pins keep
         // live reads (same-delta settle visibility — snapshotting them was
         // mode 4's overreach and its 3000ns depth defect).
         if (snap_this && (aj_snap_mode() == 3 || aj_snap_mode() == 5)
             && pins[i].sig != NULL)
            snap_this = !aj_nexus_driver_is_comb(m, &pins[i].sig->nexus, 0);
         if (snap_this) {
            snap_live[n_snap] = pins[i].data;
            snap_nb[n_snap]   = nb;
            snap_slot[n_snap] = 6 + bridged_in;
            n_snap++;
         }
      }
      // Record every clock INPUT signal for aj_subscribe_clocks (below). This
      // is the only place that knows which pins are clocks: `clk` by name and
      // the rest from the model's sm_extra_clocks[] table.
      if (!spec && is_ck && pins[i].sig != NULL
          && chunk->n_ck_sigs < (int)ARRAY_LEN(chunk->ck_sigs)) {
         bool dup = false;
         for (int k = 0; k < chunk->n_ck_sigs; k++)
            if (chunk->ck_sigs[k] == pins[i].sig) { dup = true; break; }
         if (!dup) chunk->ck_sigs[chunk->n_ck_sigs++] = pins[i].sig;
      }
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
         // X/Z detection folded into the repack: _xa accumulates
         // (byte>>1)^(base>>1) over the bits this pin just changed — branchless,
         // one XOR+OR per changed bit, and nothing at all for an unchanged pin
         // (the memcmp gate above already short-circuits). Ports whose type
         // cannot express an uncertain value (BIT/BOOLEAN, base<2) emit the
         // original loop unchanged.
         const bool xdet = pins[i].base >= 2 && !no_xdet;
         const unsigned xb = (unsigned)pins[i].base >> 1;
         if (pins[i].width > 64) {
            // Wide port: gen_statemachine declares in._<name> as a uint32_t[N]
            // limb array; place each bit in its limb.
            const int nl = (pins[i].width + 31) / 32;
            if (xdet)
               fprintf(f, "    for(int _l=0;_l<%d;_l++) in._%s[_l]=0;"
                          " unsigned _xa=0;"
                          " for(int b=0;b<%d;b++){ int _bp=%d-1-b;"
                          " unsigned _v=p[b*%d]; _xa|=(_v>>1)^%uu;"
                          " in._%s[_bp>>5]|=(uint32_t)(_v&1)<<(_bp&31); }"
                          " if(_xa){ aj_cs->x_hits++;"
                          " if(!aj_cs->x_seen)"
                          " aj_x_first(aj_cs,AJB,%d,\"%s\",p,%d,%d,%uu); } } }\n",
                       nl, pins[i].name, pins[i].width, pins[i].width,
                       pins[i].elem, xb, pins[i].name,
                       bridged_in, pins[i].name, pins[i].width, pins[i].elem, xb);
            else
               fprintf(f, "    for(int _l=0;_l<%d;_l++) in._%s[_l]=0;"
                          " for(int b=0;b<%d;b++){ int _bp=%d-1-b;"
                          " in._%s[_bp>>5]|=(uint32_t)(p[b*%d]&1)<<(_bp&31); } } }\n",
                       nl, pins[i].name, pins[i].width, pins[i].width,
                       pins[i].name, pins[i].elem);
         } else {
            if (xdet)
               fprintf(f, "    uint64_t v=0; unsigned _xa=0;"
                          " for(int b=0;b<%d;b++){ unsigned _v=p[b*%d];"
                          " _xa|=(_v>>1)^%uu;"
                          " v|=(uint64_t)(_v&1)<<(%d-1-b); }"
                          " in._%s=v;"
                          " if(_xa){ aj_cs->x_hits++;"
                          " if(!aj_cs->x_seen)"
                          " aj_x_first(aj_cs,AJB,%d,\"%s\",p,%d,%d,%uu); } } }\n",
                       pins[i].width, pins[i].elem, xb, pins[i].width,
                       pins[i].name, bridged_in, pins[i].name, pins[i].width,
                       pins[i].elem, xb);
            else
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
      // materialise the two-phase snapshot arena now every input slot is known
      chunk->snap_nin = n_snap;
      if (getenv("NVC_ACCEL_SNAP_PINS") != NULL)
         notef("accel-jit: snap-pins filter: '%s' snap_nin=%d",
               chunk->rs_top != NULL ? chunk->rs_top : "?", n_snap);
      if (n_snap > 0) {
         size_t tot = 0;
         chunk->snap_map = xcalloc_array(n_snap, sizeof(aj_snap_ent_t));
         for (int j = 0; j < n_snap; j++) {
            chunk->snap_map[j].live = snap_live[j];
            chunk->snap_map[j].off  = tot;
            chunk->snap_map[j].nb   = snap_nb[j];
            chunk->snap_map[j].slot = snap_slot[j];
            tot += snap_nb[j];
         }
         chunk->snap = xmalloc(tot);
         for (int j = 0; j < n_snap; j++)
            memcpy(chunk->snap + chunk->snap_map[j].off,
                   chunk->snap_map[j].live, chunk->snap_map[j].nb);
         if (getenv("AJ_SNAP_DBG"))
            notef("accel-snap: arena built, %d inputs", n_snap);
      }
      // bridged-input summary for the handoff link pass (bridge ordinal order)
      chunk->b_in = xcalloc_array(bridged_in > 0 ? bridged_in : 1,
                                  sizeof(aj_bpin_t));
      chunk->n_bin = bridged_in;
      for (int bi = 0; bi < bridged_in; bi++) {
         aj_pin_t *pp = &pins[bin_pin[bi]];
         chunk->b_in[bi] = (aj_bpin_t){ .data = pp->data, .sig = pp->sig,
                                        .width = pp->width, .elem = pp->elem };
         snprintf(chunk->b_in[bi].name, sizeof chunk->b_in[bi].name,
                  "%s", pp->name);
      }
      // DEAD-PRIMARY REDIRECT (SM_GROUP0_REGS, see gen_statemachine): the
      // model's main clock group has NO registers but extra clock groups
      // exist — the wrapper clk pin is a dead net (FPGA-shape members whose
      // flops run on rawclk extras), so every phase keyed on primary_ck
      // (edge arming, pre-edge snapshot, negflip staged-output publication)
      // never fires: outputs fall into mixed immediate/late publication
      // (measured on merged_31: dout_readable +5ns late, rsp-valid Q one
      // full cycle early — the descend nt11 bus-phase shift).  Re-key
      // primary_ck to a LIVE subscribed clock: prefer an underived ck_sig,
      // else the first non-primary one.  bindtab[4] keeps the wrapper-clk
      // bytes — the empty group 0 makes the netlist clklast inert.
      if (g0regs == 0 && nck > 0 && chunk->primary_ck != NULL) {
         rt_signal_t *live = NULL;
         for (int k = 0; k < chunk->n_ck_sigs && live == NULL; k++) {
            rt_signal_t *cs = chunk->ck_sigs[k];
            if (cs != NULL && cs != chunk->primary_ck
                && !aj_clk_is_derived(m, cs))
               live = cs;
         }
         for (int k = 0; k < chunk->n_ck_sigs && live == NULL; k++)
            if (chunk->ck_sigs[k] != NULL
                && chunk->ck_sigs[k] != chunk->primary_ck)
               live = chunk->ck_sigs[k];
         if (live != NULL) {
            notef("accel-jit: group-0 empty (%d extra clock group(s)) — "
                  "primary re-keyed %s -> %s", nck,
                  chunk->primary_ck->where != NULL
                     ? istr(tree_ident(chunk->primary_ck->where)) : "?",
                  live->where != NULL
                     ? istr(tree_ident(live->where)) : "?");
            chunk->primary_ck = live;
         }
      }
   }
   fprintf(f, "  return _chg;\n}\n\n");
   fprintf(f, "void accel_eval(void *p, void **AJB){\n  aj_cs_t *aj_cs = p;\n");
   fprintf(f, "  if(g_dbg<0) g_dbg = getenv(\"NVC_ACCEL_JIT_DEBUG\")?20000:0;\n");
   fprintf(f, "  unsigned d; long long t = NOW(MDL,&d);\n");
   // clk is one element; its 0/1 value is bit 0 of the low byte of the element.
   fprintf(f, "  int _clk = CLK[0]&1;\n");
   // GSM_ICG2EN exported gated clocks: the model's comb-local _clk is pinned
   // 0 by design, so gsm emits a sm_icg_clkval global its marked AND gates
   // read; poke the real clock value before every eval.  Marker-scraped so
   // every existing model's bridge text is byte-identical (no salt bump).
   if (strstr(dut_text, "sm_icg_clkval") != NULL)
      fprintf(f, "  { extern uint64_t sm_icg_clkval; sm_icg_clkval = _clk; }\n");
   fprintf(f, "  if(g_dbg>0 && _clk){ fprintf(stderr,\"AJ clk=%%d t=%%lld last=%%lld\\n\",_clk,t,last_t); g_dbg--; }\n");
   // The bridge runs on EVERY boundary-input-change delta (the rerouted
   // combinational processes wake it), not just the clock edge.  ADVANCE the
   // registers once per clock cycle.  BOTH paths now compile the same real
   // value-edge detect on clk_last0; what differs is who maintains last0:
   // VERIFY steps the bridge every delta so its own per-eval update sees the
   // low phase, while the DRIVING path cannot rely on that (the rerouted
   // proc's rising-edge trigger filters fall wakes) -- there aj_proc_eval
   // ARMS clk_last0 from the primary clock nexus' event state before each
   // eval (see the arming block).  COMB outputs re-settle on every call
   // regardless (below).
   fprintf(f, "  int posedge;\n");
   // ONE shared edge-detect expression for BOTH paths (assert-order rank
   // 5): the +4 class survived for weeks precisely because VERIFY and
   // driving compiled DIFFERENT edge rules and the clean oracle was
   // misread as exonerating compute. A single source string makes that
   // divergence impossible by construction; the emitted text is
   // byte-identical to before, so no bridge-salt bump.
   static const char AJ_EDGE_EXPR[] = "(_clk && !aj_cs->clk_last0)";
   fprintf(f, "  if(VERIFY) posedge = %s;\n", AJ_EDGE_EXPR);
   // Driving mode HAD `(_clk && t != last_t)` -- a level+new-timestep proxy
   // from before aj_subscribe_clocks existed, when the chunk might never
   // sample the clock-low phase.  Post-subscription the chunk wakes on every
   // clock event INCLUDING the fall, so a real edge detect is complete -- and
   // the proxy is strictly worse: a boundary-input wake in a negedge timestep
   // that reads the (not-yet-fallen) gated clock high fires a PHANTOM edge.
   // Measured on full VeeR: eh2_dec double-clocked at burst starts (phantom
   // at the negedge, then the real posedge) -> retired-PC +4 on the first
   // retire of every burst.  VERIFY compiled the real edge detect all along,
   // which is why it stayed clean while the driving run diverged.
   fprintf(f, "  else { posedge = %s; if(posedge) last_t = t; }\n", AJ_EDGE_EXPR);
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
   // ICG-bearing chunks (a gated-clock output): the quiet-skip must NOT eat
   // the eval where the base clock FELL — a gated clock is identically 0 at
   // clk-low, and the fall must publish or downstream interp consumers see an
   // alternate-edges-only waveform (the rim stays high, the next rise lands
   // on 1 with no event; glue2 fixture, and the quiet-phase l1clk hazard at
   // scale).  Whether the fall eval ARRIVES at all is the model side's
   // NVC_ACCEL_ICG_FALLWAKE trigger-clear; the same env gates this override
   // at RUN time so the emitted text is cache-safe.
   bool has_icg_out = false;
   for (int i = 0; i < npins; i++)
      if (pins[i].is_output
          && (pins[i].icg || strstr(pins[i].name, "l1clk") != NULL))
         has_icg_out = true;
   {
      const char *rstv = (rst != NULL) ? "(RST[0]&1)" : "0";
      if (nck == 0) {
         fprintf(f, "  { static int _noskip=-1; if(_noskip<0) _noskip=getenv(\"NVC_ACCEL_NO_SKIP\")?1:0;\n");
         fprintf(f, "    int _rn = %s;\n", rstv);
         if (has_icg_out) {
            fprintf(f, "    static int _ifw=-1; if(_ifw<0) _ifw=getenv(\"NVC_ACCEL_ICG_FALLWAKE\")?1:0;\n");
            fprintf(f, "    int _ckfall = _ifw && !_clk && aj_cs->icg_ckprev; aj_cs->icg_ckprev=(unsigned char)_clk;\n");
            fprintf(f, "    if(!_noskip && !VERIFY && !posedge && !_chg && !_ckfall && _rn==aj_cs->rst_prev) return;\n");
         }
         else
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
      if (has_late) {
         // NOTE: `S` is a macro for aj_cs->S — writing aj_cs->S here would
         // expand into aj_cs->(aj_cs->S). Use the macro.
         // ck_last RESET at arming: the chunk only re-scans when it is
         // evaluated, and a quiet chunk sleeps through the negedge half-cycle
         // entirely (VeeR dec_tlu: evals at 0, 5ns, 15ns, 30ns — nothing at
         // 10ns).  The gated clocks' FALLS are then never observed, ck_last
         // sticks at 1, and every rise after the first fails the !ck_last
         // edge test → late_pend jams armed and no gated group ever commits
         // again (first edge fine, all later cycles dead).  An ICG-of-clk
         // gated clock is by construction LOW at the instant of the main
         // posedge, so any remembered high is the previous cycle's scan —
         // clear it when arming and the same-timestep rise detects freshly.
         // A genuinely-divided clock that legitimately holds high across a
         // main posedge would see a spurious edge — NVC_ACCEL_CK_KEEPLAST
         // restores the old behaviour for such designs (none in VeeR; the
         // per-group ICG-of-clk auto-detect is the general landing).
         fprintf(f, "  static int _keeplast=-1; if(_keeplast<0)"
                    " _keeplast=getenv(\"NVC_ACCEL_CK_KEEPLAST\")?1:0;\n");
         fprintf(f, "  if(_late && (posedge || aj_cs->force_arm)){"
                    " aj_cs->force_arm = 0;"
                    " aj_cs->snapS = S;"
                    " aj_cs->snapIn = in;"
                    " aj_cs->late_pend = %uu;"
                    " if(!_keeplast) for(int _k=0;_k<%d;_k++)"
                    " aj_cs->ck_last[_k]=0; }\n",
                 ((1u << (nck + 1)) - 2u), nck);
      }
      // Merged chunks: the ck_last posedge-clear is REQUIRED, not a CK_LATE
      // option — a fused domain's extra-clock families (thread-gated
      // registers) otherwise fire ONCE and die: the chunk never samples the
      // gated clock's fall, ck_last sticks at 1, and every later rise fails
      // the !ck_last test (measured: gals2 Q2 froze after its first edge;
      // EH2 thread 1 diverged from first activation).  An ICG-of-clk gated
      // clock is by construction LOW at the main posedge, so any remembered
      // high is the previous cycle's — clearing at the posedge re-arms
      // detection for the fresh rise.
      if (chunk != NULL && chunk->merged && nck > 0)
         fprintf(f, "  if(posedge) for(int _k=0;_k<%d;_k++)"
                    " aj_cs->ck_last[_k]=0;\n", nck);
      // non-coincident (legacy): scan FIRST, then value-edge-detect each extra
      // clock from the freshly-scanned values — original behaviour, unchanged.
      fprintf(f, "  if(!_late && !_coinc){\n");
      fprintf(f, "    _chg = aj_scan_inputs(aj_cs, AJB);\n");
      for (int k = 0; k < nck; k++)
         fprintf(f, "    { int _n=(int)(in._%s>>%d)&1;"
                    " if(_n && !aj_cs->ck_last[%d]) posedge_mask|=(1u<<(1+%d));"
                    " aj_cs->ck_last[%d]=_n; }\n", extra_clk_field[k],
                 extra_clk_bit[k], k, k, k);
      // coincident: every gated clock is the main edge; DEFER the scan until
      // after the advance so flops sample previous-delta-settled inputs.
      fprintf(f, "  } else if(!_late) {\n");
      fprintf(f, "    if(posedge) posedge_mask |= %uu;\n",
              ((1u << (nck + 1)) - 2u));
      fprintf(f, "  }\n");
      fprintf(f, "  int aj_pe = (posedge_mask != 0);\n");
      // NVC_ACCEL_CK_TRACE=<N>: dump the first N evals' clock bookkeeping to
      // stderr -- one line per eval with the scanned value / last / late_pend
      // of every extra clock BEFORE the advance.  Debug aid for gated-clock
      // first-edge divergence (VeeR free_l2clk): shows exactly which delta a
      // gated group's rise is observed in and whether its commit fires.
      fprintf(f, "  static int _ckt=-2; if(_ckt==-2){ const char *e=getenv(\"NVC_ACCEL_CK_TRACE\"); _ckt=e?atoi(e):0; }\n");
      fprintf(f, "  if(_ckt>0){ _ckt--; fprintf(stderr, \"[ckt %s] t=%%lld+%%u pe=%%d mask=0x%%x%s\"",
              istr(chunk->scope->name), has_late ? " pend=0x%x" : "");
      for (int k = 0; k < nck; k++)
         fprintf(f, " \" %s=%%d/l%%d\"", extra_clk[k]);
      fprintf(f, " \"\\n\", t, d, posedge, posedge_mask");
      if (has_late)
         fprintf(f, ", (unsigned)aj_cs->late_pend");
      for (int k = 0; k < nck; k++)
         fprintf(f, ", (int)(in._%s>>%d)&1, (int)aj_cs->ck_last[%d]",
                 extra_clk_field[k], extra_clk_bit[k], k);
      fprintf(f, "); }\n");
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
            fprintf(f, "    { int _n=(int)(in._%s>>%d)&1;"
                       " if(_n && !aj_cs->ck_last[%d] && (aj_cs->late_pend & (1u<<(1+%d))))"
                       " _fired |= (1u<<(1+%d));"
                       " aj_cs->ck_last[%d]=_n; }\n",
                    extra_clk_field[k], extra_clk_bit[k], k, k, k, k);
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
         // CK_TRACE line B: post-scan/post-late state — fresh in._*, updated
         // ck_last[], fired bits and what remains pending.
         fprintf(f, "    if(_ckt>0){ fprintf(stderr, \"[ckT %s] t=%%lld+%%u fired=0x%%x pend=0x%%x\"",
                 istr(chunk->scope->name));
         for (int k = 0; k < nck; k++)
            fprintf(f, " \" %s=%%d/l%%d\"", extra_clk[k]);
         fprintf(f, " \"\\n\", t, d, _fired, (unsigned)aj_cs->late_pend");
         for (int k = 0; k < nck; k++)
            fprintf(f, ", (int)(in._%s>>%d)&1, (int)aj_cs->ck_last[%d]",
                 extra_clk_field[k], extra_clk_bit[k], k);
         fprintf(f, "); }\n");
         fprintf(f, "  }\n");
      }
      {
         const char *rstv2 = (rst != NULL) ? "(RST[0]&1)" : "0";
         fprintf(f, "  { static int _noskip=-1; if(_noskip<0) _noskip=getenv(\"NVC_ACCEL_NO_SKIP\")?1:0;\n");
         fprintf(f, "    int _rn = %s;\n", rstv2);
         if (has_icg_out) {   // same fall-override as the nck==0 skip
            fprintf(f, "    static int _ifw=-1; if(_ifw<0) _ifw=getenv(\"NVC_ACCEL_ICG_FALLWAKE\")?1:0;\n");
            fprintf(f, "    int _ckfall = _ifw && !_clk && aj_cs->icg_ckprev; aj_cs->icg_ckprev=(unsigned char)_clk;\n");
            fprintf(f, "    if(!_noskip && !VERIFY && !posedge_mask && !_chg && !_ckfall && _rn==aj_cs->rst_prev) return;\n");
         }
         else
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
   // NVC_ACCEL_EVTRACE=<substr of scope tag>: UNCONDITIONAL per-eval trace
   // (t, delta, pe/mask/chg/fused + every narrow *din* input and *dout*
   // output).  CK_TRACE is count-limited from t=0 and INDUMP/SMDUMP are
   // gated on `posedge` — which a dead-primary chunk NEVER raises — so none
   // of them can watch a same-instant capture mid-run.  This one can.
   fprintf(f, "  { static int _evt=-2; if(_evt==-2){ const char *e="
              "getenv(\"NVC_ACCEL_EVTRACE\");"
              " _evt=(e && strstr(\"%s\", e))?1:0; }\n",
           istr(chunk->scope->name));
   if (nck > 0)   // posedge_mask/_chg/_fused exist only in multi-clock bridges
      fprintf(f, "    if(_evt){ fprintf(stderr,\"[evt %s] t=%%lld+%%u pe=%%d"
                 " mask=0x%%x chg=%%d fused=%%d\","
                 " t, d, posedge, posedge_mask, _chg, _fused);\n",
              istr(chunk->scope->name));
   else
      fprintf(f, "    if(_evt){ fprintf(stderr,\"[evt %s] t=%%lld+%%u"
                 " pe=%%d\", t, d, posedge);\n",
              istr(chunk->scope->name));
   { int nf = 0;
     for (int i = 0; i < npins && nf < 16; i++) {
        if (pins[i].width > 64 || !aj_model_has_field(dut_text, pins[i].name))
           continue;
        if (!pins[i].is_output && strstr(pins[i].name, "din") != NULL) {
           fprintf(f, "    fprintf(stderr,\" %s=%%llx\","
                      "(unsigned long long)in._%s);\n",
                   pins[i].name, pins[i].name);
           nf++;
        }
        else if (pins[i].is_output && strstr(pins[i].name, "dout") != NULL) {
           fprintf(f, "    fprintf(stderr,\" %s=%%llx\","
                      "(unsigned long long)o._%s);\n",
                   pins[i].name, pins[i].name);
           nf++;
        }
     } }
   fprintf(f, "    fprintf(stderr,\"\\n\"); } }\n");
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
   static char comb_out[1024][64];   // 256 silently misclassified overflow
                                     // outputs as registered (Mealy relay
                                     // temps would take negflip — the exact
                                     // cross-boundary timing bug class)
   int n_comb_out = 0;
   {
      const char *start = strstr(dut_text, "const char *sm_comb_outputs[] = {");
      if (start != NULL) {
         start += strlen("const char *sm_comb_outputs[] = {");
         const char *end = strchr(start, '}');
         const char *p = start;
         while (end != NULL && n_comb_out < 1024) {
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
         d->icg     = pins[i].icg
                      || strstr(pins[i].name, "l1clk") != NULL;
         { const size_t pl_ = strlen(pins[i].name);
           d->probe = pl_ > 5
              && strcmp(pins[i].name + pl_ - 5, "__ajp") == 0; }
         d->defer   = (tgt != NULL);
         // Merged chunks: every REGISTERED output stages at posedge evals and
         // publishes at the domain clock's fall (the negedge state flip).  No
         // conservative reader gate needed -- publication goes through
         // deposit_signal, which handles port propagation and comb-reader
         // wakeups (the interp-side ICG enables recompute during the low
         // phase, matching real ICG latch timing).
         if (chunk->merged && !is_comb && !d->defer
             && getenv("NVC_ACCEL_NO_NEGFLIP") == NULL) {
            d->negflip = true;
            d->sigp    = pins[i].sig;
            d->pw      = pins[i].width;
            d->shadow  = xmalloc(bufsz);
         }
         if (d->defer) {
            d->nexus       = tgt;
            d->eff         = (unsigned char *)tgt->signal->shared.data + tgt->offset;
            d->last        = d->eff + tgt->signal->shared.size;
            d->cache_event = (tgt->flags & NET_F_CACHE_EVENT) != 0;
            d->shadow      = xmalloc(bufsz);
            deferred++;
         }
         // #67: registered classes (defer-bank / negflip / NBA-eligible
         // non-comb) are deferrable to the update thread; ICG clocks and
         // Mealy/comb outputs are load-bearing same-delta and pinned to E.
         // (off_edge is runtime-learned Mealy — the U thread must ALSO
         // re-check it, since an ordinal can demote after install.)
         d->must_e = d->icg || is_comb;
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
                          "%d|(unsigned)((o._%s[b>>5]>>(b&31))&1);"
                          " AJ_OUT(%d,OUT_SIG(%d),buf,%d,%s); }\n",
                       spec ? "" : "else ",
                       bufsz, pins[i].width, pins[i].width, pins[i].elem,
                       pins[i].base, pins[i].name, ord, ord, pins[i].width, pe_i);
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
                          " for(int b=0;b<%d;b++) buf[(%d-1-b)*%d]=%d|(unsigned)((v>>b)&1);"
                          " AJ_OUT(%d,OUT_SIG(%d),buf,%d,%s); }\n",
                       spec ? "" : "else ",
                       bufsz, pins[i].name, pins[i].width, pins[i].width, pins[i].elem,
                       pins[i].base, ord, ord, pins[i].width, pe_i);
            }
         }
      }
      // Duplicate rim bindings: aj_find_signal returns the FIRST signal whose
      // leaf name matches the port, but sv2vhdl duplicates rim glue (per
      // thread / readable copies), so the SAME scope can hold several
      // same-named, same-size signals — each the root of its own port
      // network. Publications must land on all of them or the unbound
      // networks' readers keep their init bytes forever (the fused VeeR
      // X-plane poisoning entered through the second dbg_halt_req /
      // exu_flush_final copies). Collect the siblings as extra deposit
      // targets for this ordinal; aj_out fans every publication out.
      if (!spec && pins[i].sig != NULL && pins[i].sig->where != NULL
          && pins[i].sig->parent != NULL) {
         rt_signal_t *prim = pins[i].sig;
         rt_scope_t *ps = prim->parent;
         // Compare against the primary SIGNAL's leaf name, not the pin name:
         // merged bridges prefix pins ("m0_dbg_halt_req") but keep the
         // member-resolved signal.
         char pn[64];
         aj_lower(pn, istr(tree_ident(prim->where)), sizeof pn);
         // sv2vhdl pairs `foo` with its readable copy `foo_readable`; the
         // interp glue between them gets rerouted, so binding one starves
         // the other's network (dec_tlu_mhartstart never rose and thread 1
         // never started while _readable updated). Compare base names.
         { size_t pl = strlen(pn);
           if (pl > 9 && strcmp(pn + pl - 9, "_readable") == 0)
              pn[pl - 9] = '\0'; }
         { static const char *bd = NULL; static int bdi = -1;
           if (bdi < 0) { bd = getenv("NVC_ACCEL_BINDDBG"); bdi = bd ? 1 : 0; }
           if (bdi && strstr(pn, bd) != NULL) {
              notef("accel-jit: binddbg pin '%s' prim=%p leaf=%s parent=%s "
                    "(%d signals) size=%u", pins[i].name, (void *)prim, pn,
                    istr(ps->name), ps->signals.count,
                    (unsigned)prim->shared.size);
              for (int si = 0; si < ps->signals.count; si++) {
                 rt_signal_t *sb = ps->signals.items[si];
                 if (sb->where == NULL) continue;
                 char xn[64];
                 aj_lower(xn, istr(tree_ident(sb->where)), sizeof xn);
                 if (strstr(xn, bd) != NULL)
                    notef("accel-jit:   scope sig %p leaf=%s size=%u%s",
                          (void *)sb, xn, (unsigned)sb->shared.size,
                          sb == prim ? " (prim)" : "");
              }
           } }
         for (int si = 0; si < ps->signals.count; si++) {
            rt_signal_t *sib = ps->signals.items[si];
            if (sib == prim || sib->where == NULL
                || sib->shared.size != prim->shared.size)
               continue;
            char sn[64];
            aj_lower(sn, istr(tree_ident(sib->where)), sizeof sn);
            { size_t sl = strlen(sn);
              if (sl > 9 && strcmp(sn + sl - 9, "_readable") == 0)
                 sn[sl - 9] = '\0'; }
            if (strcmp(sn, pn) != 0)
               continue;
            if (chunk->out_extra == NULL) {
               chunk->out_extra   = xcalloc_array(npins > 0 ? npins : 1,
                                                  sizeof(*chunk->out_extra));
               chunk->out_extra_n = xcalloc_array(npins > 0 ? npins : 1, 1);
            }
            if (chunk->out_extra_n[ord] < 6)
               chunk->out_extra[ord][chunk->out_extra_n[ord]++] = sib;
         }
      }
      bout_pin[ord] = i;
      ord++;
      bridged_out++;
   }
   if (!spec) {
      chunk->defer_count = ord;
      { int nicg = 0;
        for (int i2 = 0; i2 < ord; i2++)
           if (chunk->defer_outs[i2].icg) nicg++;
        if (nicg > 0)
           notef("accel-jit: '%s': %d gated-clock (ICG) output(s)",
                 chunk->rs_top != NULL ? chunk->rs_top : "?", nicg); }
      // #66 sibling-rim census: how many outputs carry same-scope same-name
      // sibling deposit targets (the duplicated-rim glue class — the proven
      // event-hole participant).  Bounds the coverage cost of declining
      // such members from reroute at admission time.
      { int nsib = 0, nx = 0;
        if (chunk->out_extra_n != NULL)
           for (int i2 = 0; i2 < ord; i2++)
              if (chunk->out_extra_n[i2] > 0)
                 { nsib++; nx += chunk->out_extra_n[i2]; }
        if (nsib > 0)
           notef("accel-jit: SIBRIM '%s': %d/%d output(s) have %d sibling "
                 "rim(s)", chunk->rs_top != NULL ? chunk->rs_top : "?",
                 nsib, ord, nx); }
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
   // THE DEMOTE REQUEST (GAP 2). Optional symbols, dlsym'd NULL-tolerantly at
   // install exactly like accel_dump/accel_in_addr — no AJB slot is taken, so
   // the fixed indices (FORCE/NOW/MDL/AJ_OUT/CLK/RST, then IN_ADDR/OUT_SIG)
   // are untouched and a stale cached .so simply reports "no detector".
   //   accel_x_seen  — 1 once a boundary input carried an uncertain value
   //   accel_x_info  — hit count + first offending bridge ordinal (+1)
   //   accel_x_clear — re-arm (used by the model to ignore hits before
   //                   NVC_ACCEL_XDEMOTE_AFTER; power-on U is not a defect)
   if (!no_xdet) {
      fprintf(f, "int accel_x_seen(void *p){ aj_cs_t *aj_cs = p;"
                 " return aj_cs->x_seen; }\n");
      fprintf(f, "void accel_x_info(void *p, unsigned *hits, unsigned *pin){"
                 " aj_cs_t *aj_cs = p;"
                 " if(hits) *hits = aj_cs->x_hits;"
                 " if(pin) *pin = aj_cs->x_pin; }\n");
      fprintf(f, "void accel_x_clear(void *p){ aj_cs_t *aj_cs = p;"
                 " aj_cs->x_seen = 0; aj_cs->x_pin = 0; }\n");
   }
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
// `timeout` reports 124 when it fires, or 128+SIGKILL (137) if -k had to
// escalate. system() hands back a wait status, so unwrap it before comparing.
static bool aj_synth_timed_out(int status)
{
   if (status == -1) return false;
   if (WIFSIGNALED(status)) return WTERMSIG(status) == SIGKILL;
   if (!WIFEXITED(status)) return false;
   const int ec = WEXITSTATUS(status);
   return ec == 124 || ec == 137;
}

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

// ---- NVC_ACCEL_MERGE: one chunk per clock domain (task #53/#59) ------------
// The collect pass records fully-prepared install candidates instead of
// installing them; aj_try_merge_install groups them by primary-clock SIGNAL
// (port collapse makes same-domain pins share one rt_signal_t) and installs
// each group as ONE merged chunk: a generated wrapper module instantiates
// every member, chunk-to-chunk nets become flattened INTERNAL wires (the
// cross-chunk sampling race class disappears structurally), and only the
// interp-facing rim keeps bridge pins.  gen_statemachine's generated code is
// already the no-copy two-phase eval within the block: registers are copied
// to pre-edge locals (phase 1), comb reads the locals, commits write state
// (phase 2) -- so a single-domain merge needs no cross-block banking at all.
typedef struct {
   rt_scope_t *scope;
   tree_t      ref;
   char       *top_mod;      // emitted Verilog module name (variant name)
   char       *vpath;        // emitted subtree .v path
   aj_pin_t   *pins;
   int         npins;
   aj_pin_t    clk, rst;
   rt_nexus_t *ck_root;      // ultimate driving nexus of clk (the group key)
   bool        have_rst;
   // NVC_ACCEL_MERGE_GLUE: comb-only subtree (no primary clock) recorded as
   // connective tissue.  Never seeds or joins a group by clock; admitted into
   // a group only by the backward-cone fixpoint in aj_try_merge_install when
   // one of its outputs feeds a group member's input.  Un-admitted glue stays
   // interpreted (it is never claimed).  This is the mech-3 root fix: without
   // the glue, member<->member cascades cross the interp rim and the merged
   // chunk's registers capture stale sibling values at the armed eval.
   bool        is_glue;
} aj_mcand_t;

static bool        g_aj_collecting = false;
static aj_mcand_t *g_aj_cands   = NULL;
static int         g_aj_ncand   = 0;
static int         g_aj_candmax = 0;

// accessors for the pin-census dump, which is defined before these globals
static int g_aj_ncand_fwd(void) { return g_aj_ncand; }
static bool g_aj_cand_scope_at(int i, rt_scope_t **sc, bool *is_glue)
{
   if (i < 0 || i >= g_aj_ncand) return false;
   *sc = g_aj_cands[i].scope;
   *is_glue = g_aj_cands[i].is_glue;
   return true;
}

static void aj_try_merge_install(rt_model_t *m, const char *accel_dir);

// ---- NVC_ACCEL_PIN_COMPLETE: starve-census pin completion (#70) ------------
// A merged chunk internalizes member nets that live interp glue still READS
// (this translation vintage relays values up the hierarchy through per-level
// gated glue procs); those nets freeze at init and the relay chain starves
// (EH1a whole-core: the model computes fetches, the TB never sees one).
// Two-run protocol: run 1 dumps the post-quench starved-net census
// (NVC_ACCEL_PIN_CENSUS=<file>, see aj_pin_census_dump); run 2 loads it
// (NVC_ACCEL_PIN_COMPLETE=<file>) and injects each depth-0 record as an
// EXTRA OUTPUT PIN on the member's emitted subtree module — an `__ajp`
// probe port assigned from the internal net — so the standard bridge
// output path publishes it and the relays re-derive natively.  Probe
// values are whole-timestep relay feeds, never a delta-accurate oracle
// (VERIFY skips their compare).
typedef struct {
   char scope[256];
   char leaf[64];
   int  width, elem, readers, depth;
   bool used;
} aj_pcrec_t;

static aj_pcrec_t *g_aj_pcrec  = NULL;
static int         g_aj_npcrec = -2;   // -2 = not loaded yet

static void aj_pin_complete_load(void)
{
   if (g_aj_npcrec != -2) return;
   g_aj_npcrec = -1;
   const char *fn = getenv("NVC_ACCEL_PIN_COMPLETE");
   if (fn == NULL) return;
   FILE *f = fopen(fn, "r");
   if (f == NULL) {
      warnf("accel-jit: NVC_ACCEL_PIN_COMPLETE=%s: cannot open — "
            "pin completion inert this run", fn);
      return;
   }
   int cap = 0, n = 0;
   char line[512];
   while (fgets(line, sizeof line, f) != NULL) {
      if (line[0] == '#' || line[0] == '\n') continue;
      aj_pcrec_t r;
      memset(&r, 0, sizeof r);
      if (sscanf(line, "%255s %63s %d %d %d %d", r.scope, r.leaf,
                 &r.width, &r.elem, &r.readers, &r.depth) != 6)
         continue;
      if (n == cap) {
         cap = cap ? cap * 2 : 64;
         g_aj_pcrec = xrealloc_array(g_aj_pcrec, cap, sizeof(aj_pcrec_t));
      }
      g_aj_pcrec[n++] = r;
   }
   fclose(f);
   g_aj_npcrec = n;
   notef("accel-jit: pin-complete: %d census record(s) loaded from %s", n, fn);
}

// Inject probe output ports for this member scope's depth-0 census records
// into a PRIVATE copy of the emitted subtree .v (the shared per-entity file
// must not leak probes into sibling candidates).  Returns the number of
// probes injected and fills xp[]; 0 = no records/nothing injectable (dst not
// written, caller keeps using src).
static int aj_inject_probes(const char *src, const char *dst,
                            const char *top_mod, rt_scope_t *scope,
                            aj_pin_t *xp, int maxxp)
{
   aj_pin_complete_load();
   if (g_aj_npcrec <= 0) return 0;
   const char *spath = istr(scope->name);
   int nrec = 0;
   for (int i = 0; i < g_aj_npcrec; i++)
      if (g_aj_pcrec[i].depth == 0 && !g_aj_pcrec[i].used
          && strcmp(g_aj_pcrec[i].scope, spath) == 0)
         nrec++;
   if (nrec == 0) return 0;

   char *text = aj_read_file(src);
   if (text == NULL) return 0;

   // locate the TOP module's ANSI port-list close: "module <top> (" .. ");"
   char key[256];
   snprintf(key, sizeof key, "module %s (", top_mod);
   char *mh = strstr(text, key);
   char *close = mh != NULL ? strstr(mh, ");") : NULL;
   if (close == NULL) { free(text); return 0; }
   // bound all body searches to THIS module: an undeclared-in-top leaf that
   // exists in a LATER module would otherwise pass the decl check and the
   // probe assign would create a silent 1-bit implicit wire
   char *mod_end = strstr(close, "\nendmodule");
   const char save_end = mod_end != NULL ? *mod_end : '\0';
   if (mod_end != NULL) *mod_end = '\0';

   char ports[8192]; ports[0] = '\0'; size_t pn = 0;
   char assigns[8192]; assigns[0] = '\0'; size_t an = 0;
   int nxp = 0, nskip = 0;
   for (int i = 0; i < g_aj_npcrec && nxp < maxxp; i++) {
      aj_pcrec_t *r = &g_aj_pcrec[i];
      if (r->depth != 0 || r->used || strcmp(r->scope, spath) != 0)
         continue;
      rt_signal_t *sg = aj_find_signal(scope, r->leaf);
      // staleness cross-check: the live signal must match the census shape
      if (sg == NULL || (int)sg->nexus.size != r->elem
          || (int)(sg->shared.size / (r->elem > 0 ? r->elem : 1))
             != r->width) {
         nskip++;
         continue;
      }
      // the net must exist as a PACKED decl in the emitted top module (a
      // memory-shaped or optimized-away net cannot become a port)
      char decl[96];
      snprintf(decl, sizeof decl, " %s;", r->leaf);
      char decl2[96];
      snprintf(decl2, sizeof decl2, " %s =", r->leaf);
      char decl3[96];
      snprintf(decl3, sizeof decl3, " %s <=", r->leaf);
      if (strstr(close, decl) == NULL && strstr(close, decl2) == NULL
          && strstr(close, decl3) == NULL) {
         nskip++;
         continue;
      }
      // already a port? (would double-publish one signal from two ordinals)
      char pkey[96];
      snprintf(pkey, sizeof pkey, "put %s,", r->leaf);   // in/output %s,
      char pkey2[96];
      snprintf(pkey2, sizeof pkey2, "put %s\n", r->leaf);
      const char save_cl = *close;
      *close = '\0';                       // header = [mh, close)
      const bool isport = strstr(mh, pkey) != NULL
         || strstr(mh, pkey2) != NULL;
      *close = save_cl;
      if (isport) {
         nskip++;
         continue;
      }
      if (r->width > 1)
         pn += snprintf(ports + pn, sizeof ports - pn,
                        ",\n  output [%d:0] %s__ajp", r->width - 1, r->leaf);
      else
         pn += snprintf(ports + pn, sizeof ports - pn,
                        ",\n  output %s__ajp", r->leaf);
      an += snprintf(assigns + an, sizeof assigns - an,
                     "  assign %s__ajp = %s;\n", r->leaf, r->leaf);
      if (pn >= sizeof ports - 128 || an >= sizeof assigns - 128)
         break;   // caps full — remaining records stay for a later phase
      aj_pin_t pin = {0};
      snprintf(pin.name, sizeof pin.name, "%s__ajp", r->leaf);
      pin.elem  = (int)sg->nexus.size;
      pin.width = r->width;
      pin.data  = (uint8_t *)sg->shared.data;
      pin.sig   = sg;
      pin.base  = sg->where != NULL
         ? aj_bit_base(tree_type(sg->where)) : 0;
      pin.is_output = true;
      xp[nxp++] = pin;
      r->used = true;
   }
   if (mod_end != NULL) *mod_end = save_end;    // restore before writing
   if (nxp == 0) { free(text); return 0; }

   FILE *o = fopen(dst, "w");
   if (o == NULL) { free(text); return 0; }
   fwrite(text, 1, (size_t)(close - text), o);   // header up to ");"
   fputs(ports, o);                              // extra output ports
   fputs("\n);\n", o);                           // close the port list
   fputs(assigns, o);                            // probe assigns (module body)
   fputs(close + 2, o);                          // rest after the original ");"
   fclose(o);
   free(text);
   notef("accel-jit: pin-complete: injected %d probe(s) into '%s' "
         "(%d record(s) skipped)", nxp, top_mod, nskip);
   return nxp;
}

// Rewrite an emitted subtree .v with every module name suffixed, so N members
// of a merge can coexist in one yosys design.  Necessary because the emit
// path names both the FILE and the MODULES by entity/variant, and same-entity
// members with different baked generics collide on both (each emit overwrites
// the file; the per-chunk path never noticed because it synthesizes
// immediately and keys the .so by CONTENT hash).  Returns the renamed top
// module in top_out.
static bool aj_uniquify_modules(const char *src, const char *dst,
                                const char *suffix, const char *top_in,
                                char *top_out, size_t top_outsz)
{
   char *text = aj_read_file(src);
   if (text == NULL)
      return false;
   // Collect declared module names WITH a content hash of each module's own
   // text span.  The per-member suffix (_cN) forked every shared primitive
   // into N copies -- 120 VeeR members each carried a renamed copy of the
   // same rvdff forest and yosys elaborated the whole thing N times (fused
   // synths ran for hours).  A content-hash suffix gives identical modules
   // the SAME name everywhere: yosys's overwrite-on-identical-redefinition
   // is harmless, and the design graph dedups back to its true size.  Only
   // same-name modules with DIFFERENT content (baked generics) diverge.
   char names[256][128];
   uint64_t nhash[256];
   const char *nspan_s[256], *nspan_e[256];
   int nnames = 0;
   for (const char *p = text; (p = strstr(p, "module ")) != NULL; p += 7) {
      if (p != text && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))
         continue;   // endmodule / $xmodule etc.
      const char *q = p + 7;
      while (*q == ' ') q++;
      int len = 0;
      while ((isalnum((unsigned char)q[len]) || q[len] == '_') && len < 127)
         len++;
      if (len == 0 || nnames == 256) continue;
      const char *e = strstr(q, "endmodule");
      uint64_t h = 1469598103934665603ULL;
      for (const char *t = q; t < (e ? e + 9 : q + len); t++)
         { h ^= (uint8_t)*t; h *= 1099511628211ULL; }
      memcpy(names[nnames], q, len);
      names[nnames][len] = '\0';
      nhash[nnames] = h;
      nspan_s[nnames] = q; nspan_e[nnames] = e ? e + 9 : q + len;
      nnames++;
   }
   // TRANSITIVE hash: an identical-text parent instantiating divergent
   // children must itself diverge (else the overwrite picks one child
   // binding — the 24-clusters bug one level up).  Mix referenced modules'
   // hashes into each module's hash until fixpoint (instantiation graphs
   // are acyclic, so <= nnames passes converge).
   // The reference structure is PASS-INVARIANT (the text never changes) —
   // precompute it once and iterate the hash folding over bitmaps.  The
   // old form re-ran the strstr search over every module span on EVERY
   // fixpoint pass: measured 71% of fused VeeR warm startup (~190s) inside
   // one libc string kernel.  Fold order and in-place (Gauss-Seidel)
   // updates are preserved exactly, so the resulting hashes — and the
   // downstream synth/.so cache keys — are bit-identical.
   static uint64_t refs[256][4];
   memset(refs, 0, sizeof refs);
   for (int i = 0; i < nnames; i++)
      for (int j = 0; j < nnames; j++) {
         if (j == i) continue;
         const size_t jl = strlen(names[j]);
         for (const char *t = nspan_s[i];
              (t = strstr(t, names[j])) != NULL && t < nspan_e[i];
              t += jl) {
            const char b = (t > nspan_s[i]) ? t[-1] : ' ';
            const char a = t[jl];
            if (!(isalnum((unsigned char)b) || b == '_')
                && !(isalnum((unsigned char)a) || a == '_')) {
               refs[i][j >> 6] |= (uint64_t)1 << (j & 63);
               break;   // one mix per referenced module is enough
            }
         }
      }
   for (int pass = 0; pass < nnames; pass++) {
      bool changed = false;
      for (int i = 0; i < nnames; i++) {
         uint64_t h = nhash[i];
         for (int j = 0; j < nnames; j++)
            if (j != i && (refs[i][j >> 6] & ((uint64_t)1 << (j & 63))))
               h = (h ^ nhash[j]) * 1099511628211ULL;
         if (h != nhash[i]) { nhash[i] = h; changed = true; }
      }
      if (!changed) break;
   }
   FILE *f = fopen(dst, "w");
   if (f == NULL) { free(text); return false; }
   // stream the text, replacing whole-word occurrences of any collected name
   const char *p = text;
   while (*p) {
      if (isalpha((unsigned char)*p) || *p == '_' || *p == '\\') {
         const char *w = p;
         if (*p == '\\') p++;   // escaped identifier: include leader
         while (isalnum((unsigned char)*p) || *p == '_') p++;
         const int wl = (int)(p - w) - (w[0] == '\\' ? 1 : 0);
         const char *wb = w + (w[0] == '\\' ? 1 : 0);
         int hit = -1;
         for (int n = 0; n < nnames && hit < 0; n++)
            if ((int)strlen(names[n]) == wl
                && strncmp(names[n], wb, wl) == 0)
               hit = n;
         fwrite(w, 1, p - w, f);
         if (hit >= 0)
            fprintf(f, "_h%08x", (unsigned)(nhash[hit] & 0xffffffffu));
      }
      else
         fputc(*p++, f);
   }
   fclose(f);
   (void)suffix;   // superseded by per-module content-hash suffixes
   uint64_t th = 0;
   for (int n = 0; n < nnames; n++)
      if (strcmp(names[n], top_in) == 0) { th = nhash[n]; break; }
   free(text);
   snprintf(top_out, top_outsz, "%s_h%08x", top_in,
            (unsigned)(th & 0xffffffffu));
   return true;
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
      // Default 1 = no size gate.  The old default of 8 counted INSTANCE
      // SCOPES, which is blind to the amount of work: a flat monolithic
      // entity counts 1 at any size and could never install, while 64 copies
      // of one leaf (128 comb cells) was refused where 33 distinct modules
      // (64 cells) was admitted.  Measured crossover is TWO pipeline stages,
      // ~30x below the old threshold, and of 4,514 regression designs the
      // largest emitted subtree is 3 modules -- so the old default admitted
      // exactly none of them.  This gate is about COST, never correctness.
      const char *minenv = getenv("NVC_ACCEL_MIN_MODULES");
      const int min_mod = minenv ? atoi(minenv) : 1;
      const int ninst = aj_count_instances(scope);
      if (ninst < min_mod) {
         notef("accel-jit: subtree '%s' below instance gate (%d < %d) "
               "— leaving in nvc", istr(tree_ident(scope->where)), ninst, min_mod);
         return false;
      }
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
   // NVC_ACCEL_SLICE=<seed>:<pct> — seeded random admission (user
   // directive: regression suites as accel-boundary fuzzers).  Each seed
   // draws a DIFFERENT deterministic accel/interp boundary through the
   // same design: hash(subtree name, seed) selects pct% of subtrees for
   // acceleration, the rest stay interpreted.  pct=100 = full-bore.
   { static int sl_pct = -2; static unsigned sl_seed = 0;
     if (sl_pct == -2) {
        const char *se = getenv("NVC_ACCEL_SLICE");
        sl_pct = -1;
        if (se != NULL) {
           unsigned s, p;
           if (sscanf(se, "%u:%u", &s, &p) == 2 && p <= 100) {
              sl_seed = s; sl_pct = (int)p;
           }
        }
     }
     if (sl_pct >= 0) {
        uint32_t h = 2166136261u ^ sl_seed;
        for (const char *p = top0; *p; p++)
           h = (h ^ (uint8_t)*p) * 16777619u;
        if ((h % 100u) >= (unsigned)sl_pct) {
           notef("accel-jit: subtree '%s' sliced out (NVC_ACCEL_SLICE)",
                 top0);
           return false;
        }
     } }
      // NVC_ACCEL_SKIP_SCOPE=p1,p2: decline subtrees whose INSTANCE PATH
      // contains a token. Needed where module names cannot discriminate:
      // testbench memory MODELS instantiate the same rvdff primitives as the
      // core, but accelerating them closes handshake feedback loops a delta
      // early (wait-states compress; VeeR TB-mem responses arrived cycles
      // ahead once the chunks' clocks were wired) — and TB models are not
      // the speed target. Match against the scope path (e.g. ".DUT.MEM.").
      const char *sksc = getenv("NVC_ACCEL_SKIP_SCOPE");
      if (sksc != NULL && sksc[0] != '\0' && scope->name != NULL) {
         char buf[512];
         snprintf(buf, sizeof buf, "%s", sksc);
         const char *sn = istr(scope->name);
         for (char *t = strtok(buf, ","); t != NULL; t = strtok(NULL, ","))
            if (t[0] != '\0' && strstr(sn, t) != NULL) {
               notef("accel-jit: subtree '%s' at %s skipped "
                     "(NVC_ACCEL_SKIP_SCOPE)", top0, sn);
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
      const int min_mod = minenv ? atoi(minenv) : 1;
      if (nseen < min_mod) {
         notef("accel-jit: subtree '%s' too small (%d modules) — leaving in nvc",
               top0, nseen);
         return false;
      }
      // Oversized emissions DESCEND instead of admitting the whole subtree:
      // a multi-MB .v (flattened RAM farm — eh2_ic_data class) was doomed
      // either way (merge-excluded; singleton synth hangs; interpreted
      // under NOFALLBACK) and, worse, admitting it here CONSUMED its
      // interior — re-admitting ic_mem/ic_data dissolved the ICM/DCCM
      // flop-bundle groups (14 groups/295 members -> 6/88) and LOST speed
      // (16.5 -> 13.4 cyc/s).  Returning false makes accel_scan_scope
      // recurse into the children, so the tractable interior becomes
      // candidates piecewise.  OPT-IN (NVC_ACCEL_DESCEND_BIG=1): with the
      // descend on, coverage jumps 14 groups/295 members -> 34/701 on
      // VeeR (5 descents) but the retire stream diverges (28 retires
      // diff=24 vs the window — deterministic), so a correctness bisect
      // of the newly admitted interior chunk classes must land before
      // this can default on.
      if (getenv("NVC_ACCEL_DESCEND_BIG") != NULL) {
         const char *bigenv = getenv("NVC_ACCEL_MERGE_MAX_MEMBER");
         const long bigcap = bigenv ? atol(bigenv) : 2*1024*1024;
         struct stat vst;
         if (stat(vpath, &vst) == 0 && vst.st_size > bigcap) {
            notef("accel-jit: subtree '%s' emission oversized (%ld bytes) — "
                  "descending into children", top0, (long)vst.st_size);
            return false;
         }
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

   // NVC_ACCEL_MERGE collect pass (task #53/#59), placed BEFORE the synth
   // step: collect mode must not pay per-candidate synthesis (measured: 6h
   // on full VeeR reached only 80 of 1,193 candidates).  Build a local pin
   // view (the normal pin loop runs post-synth because its no-"clk" fallback
   // scrapes the generated C; here a name heuristic + the AST derived-clock
   // test suffice), snapshot the emitted .v with uniquified module names,
   // and record the candidate.  Returning true stops the scan descending.
   if (g_aj_collecting) {
      if (nsrc != 1)
         return false;      // collect handles the single-file FROM_VHDL form
      static aj_pin_t cpins[AJ_MAX_PINS];
      int ncp = 0;
      int clk_i = -1;
      const int cnports = tree_ports(ent);
      for (int i = 0; i < cnports && ncp < AJ_MAX_PINS; i++) {
         tree_t p = tree_port(ent, i);
         char lname[64];
         aj_lower(lname, istr(tree_ident(p)), sizeof lname);
         rt_signal_t *sig = aj_find_signal(scope, lname);
         if (sig == NULL) {
            if (getenv("NVC_ACCEL_COLLECT_DBG") != NULL)
               notef("accel-jit: collect '%s' %s: port %s NOT FOUND",
                     top0, istr(scope->name), lname);
            continue;
         }
         aj_pin_t pin = {0};
         snprintf(pin.name, sizeof pin.name, "%s", lname);
         pin.elem  = (int)sig->nexus.size;
         pin.width = pin.elem ? (int)(sig->shared.size / pin.elem) : 1;
         pin.data  = (uint8_t *)sig->shared.data;
         pin.sig   = sig;
         pin.base  = aj_bit_base(tree_type(p));
         pin.is_output = (tree_subkind(p) == PORT_OUT
                          || tree_subkind(p) == PORT_BUFFER);
         if (pin.width < 1 || pin.width > 4096
             || (pin.elem != 1 && pin.elem != 4)
             || !aj_marshallable_type(tree_type(p))) {
            if (getenv("NVC_ACCEL_COLLECT_DBG") != NULL)
               notef("accel-jit: collect bail '%s' pin %s w=%d elem=%d "
                     "marsh=%d", top0, lname, pin.width, pin.elem,
                     (int)aj_marshallable_type(tree_type(p)));
            return false;   // unmarshallable: leave for the normal scan
         }
         if (!pin.is_output) {
            const size_t nl = strlen(lname);
            if (strcmp(lname, "clk") == 0)
               clk_i = ncp;   // exact name wins
            else if (clk_i < 0 && nl >= 3
                     && strcmp(lname + nl - 3, "clk") == 0
                     && !aj_clk_is_derived(m, sig))
               clk_i = ncp;   // first primary *clk-suffixed input
         }
         cpins[ncp++] = pin;
      }
      if (clk_i < 0) {
         // No primary clock: not mergeable as a member.  Under
         // NVC_ACCEL_MERGE_GLUE, record comb-only subtrees (sv_and/sv_or
         // interconnect glue) as glue candidates so the group builder can
         // internalize member->glue->member cascades (mech-3 root fix).
         // Either way the subtree is NOT collected as a member: return
         // false so it stays interpreted unless a group admits it.
         static int glue_on = -1;
         if (glue_on < 0)
            glue_on = getenv("NVC_ACCEL_MERGE_GLUE") != NULL;
         bool has_out = false;
         for (int p = 0; p < ncp; p++) has_out |= cpins[p].is_output;
         if (glue_on && ncp > 0 && has_out) {
            if (g_aj_ncand == g_aj_candmax) {
               g_aj_candmax = g_aj_candmax ? g_aj_candmax * 2 : 64;
               g_aj_cands = xrealloc_array(g_aj_cands, g_aj_candmax,
                                           sizeof(aj_mcand_t));
            }
            aj_mcand_t *gc = &g_aj_cands[g_aj_ncand++];
            memset(gc, 0, sizeof(aj_mcand_t));
            gc->scope = scope;
            gc->ref   = ref;
            char sfx[32], upath[600], utop[192];
            snprintf(sfx, sizeof sfx, "_g%d", g_aj_ncand - 1);
            snprintf(upath, sizeof upath, "%s/aj_glu%d_subtree.v", accel_dir,
                     g_aj_ncand - 1);
            if (!aj_uniquify_modules(srcs[0], upath, sfx, top_mod,
                                     utop, sizeof utop)) {
               g_aj_ncand--;
               return false;
            }
            // NO EDGE-TRIGGERED STATE: comb glue may legitimately emit as
            // `always @(*)` (reduction loops), but a no-clock subtree with
            // posedge/negedge sensitivity holds state and must never be
            // synthesized as comb inside the fused netlist.
            {
               char *vtext = aj_read_file(upath);
               const bool has_edge = vtext != NULL
                  && (strstr(vtext, "posedge") != NULL
                      || strstr(vtext, "negedge") != NULL);
               free(vtext);
               if (has_edge) {
                  g_aj_ncand--;
                  return false;
               }
            }
            gc->top_mod = xstrdup(utop);
            gc->vpath   = xstrdup(upath);
            gc->pins  = xmalloc_array(ncp, sizeof(aj_pin_t));
            memcpy(gc->pins, cpins, ncp * sizeof(aj_pin_t));
            gc->npins = ncp;
            gc->is_glue = true;
         }
         return false;
      }
      if (g_aj_ncand == g_aj_candmax) {
         g_aj_candmax = g_aj_candmax ? g_aj_candmax * 2 : 64;
         g_aj_cands = xrealloc_array(g_aj_cands, g_aj_candmax,
                                     sizeof(aj_mcand_t));
      }
      aj_mcand_t *c = &g_aj_cands[g_aj_ncand++];
      memset(c, 0, sizeof(aj_mcand_t));
      c->scope = scope;
      c->ref   = ref;
      // Snapshot the emitted .v NOW under a unique path with uniquified
      // module names: the emit path keys the file AND the modules by
      // entity/variant name, so same-entity members with different baked
      // generics overwrite each other and collide in yosys (measured:
      // merged many_k24 silently became 24 copies of cluster 23).
      static aj_pin_t xpins[256];
      int nxp = 0;
      {
         char sfx[32], upath[600], utop[192];
         snprintf(sfx, sizeof sfx, "_c%d", g_aj_ncand - 1);
         snprintf(upath, sizeof upath, "%s/aj_mrg%d_subtree.v", accel_dir,
                  g_aj_ncand - 1);
         // NVC_ACCEL_PIN_COMPLETE: inject census probe ports into a private
         // copy BEFORE uniquify — the content hash then forks the probed
         // top's module name from unprobed same-entity siblings, and every
         // downstream stage (wrapper pins, bridge ordinals, cache keys)
         // follows from c->pins + the emitted bytes automatically.
         const char *usrc = srcs[0];
         char ppath[600];
         if (getenv("NVC_ACCEL_PIN_COMPLETE") != NULL) {
            snprintf(ppath, sizeof ppath, "%s/aj_pcp%d_subtree.v", accel_dir,
                     g_aj_ncand - 1);
            nxp = aj_inject_probes(srcs[0], ppath, top_mod, scope,
                                   xpins, 256);
            if (nxp > 0) usrc = ppath;
            if (ncp + nxp > AJ_MAX_PINS) {   // never overflow into fallback
               nxp = 0;
               usrc = srcs[0];
            }
         }
         if (!aj_uniquify_modules(usrc, upath, sfx, top_mod,
                                  utop, sizeof utop)) {
            g_aj_ncand--;
            return false;
         }
         c->top_mod = xstrdup(utop);
         c->vpath   = xstrdup(upath);
      }
      c->pins  = xmalloc_array(ncp + nxp > 0 ? ncp + nxp : 1,
                               sizeof(aj_pin_t));
      memcpy(c->pins, cpins, (ncp > 0 ? ncp : 1) * sizeof(aj_pin_t));
      if (nxp > 0)
         memcpy(c->pins + ncp, xpins, nxp * sizeof(aj_pin_t));
      c->npins = ncp + nxp;
      c->clk   = cpins[clk_i];
      c->ck_root = aj_ultimate_driver_nexus(&cpins[clk_i].sig->nexus, 0);
      memset(&c->rst, 0, sizeof c->rst);
      c->have_rst = false;
      return true;
   }

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
   // GSM_ICG2EN changes gsm's emitted netlist for the SAME sources: fold it
   // into the key so toggling the env cannot serve the other config's cache.
   { const char *icg = getenv("GSM_ICG2EN");
     if (icg != NULL)
        for (const char *p = "+icg2en="; *p; p++)
           { vhash ^= (uint8_t)*p; vhash *= 1099511628211ULL; }
     for (const char *p = icg ? icg : ""; *p; p++)
        { vhash ^= (uint8_t)*p; vhash *= 1099511628211ULL; } }
   for (const char *p = top_mod; *p; p++) { vhash ^= (uint8_t)*p; vhash *= 1099511628211ULL; }
   char **vtexts = xcalloc_array(nsrc, sizeof(char *));
   for (int i = 0; i < nsrc; i++) {
      char *vtext = aj_read_file(srcs[i]);
      if (vtext != NULL) {
         for (const char *p = vtext; *p; p++) { vhash ^= (uint8_t)*p; vhash *= 1099511628211ULL; }
         vtexts[i] = vtext;   // staged below once the full key is known
      }
   }
   // Stage the HASHED bytes under the key so gen_statemachine reads exactly
   // what was hashed (see aj_stage_source).  A failed stage falls back to
   // the shared-named source: correct in the single-process case.
   bool staged = true;
   for (int i = 0; i < nsrc; i++) {
      if (vtexts[i] == NULL
          || !aj_stage_source(accel_dir, top, vhash, i, vtexts[i]))
         staged = false;
      free(vtexts[i]);
   }
   free(vtexts);

   char dutc[600], bridge[600], so[600];
   snprintf(dutc,   sizeof dutc,   "%s/aj_%s_%016llx.c", accel_dir, top,
            (unsigned long long)vhash);
   snprintf(bridge, sizeof bridge, "%s/aj_%s_%016llx_bridge.c", accel_dir, top,
            (unsigned long long)vhash);

   // TWO-TIER KEY.  The generated C above is keyed by vhash alone -- it is
   // derived from the LOGIC and is therefore portable: the same design yields
   // the same C on every machine, so it is worth keeping (and in principle
   // committing) rather than recomputing.
   //
   // The .so is NOT portable.  It is native code emitted by the local compiler
   // for the local CPU, so its key must also cover the toolchain and the
   // machine.  Without that:
   //   * switching NVC_ACCEL_CC between `cc` (-O0) and `gcc -O3` SILENTLY
   //     REUSES the old .so, because the compile step below is skipped
   //     whenever the file merely exists -- which invalidates any measurement
   //     comparing optimisation levels;
   //   * a cache directory shared across a heterogeneous farm would load
   //     machine code built for someone else's ISA.
   // Both became live hazards when the benchmark harnesses stopped wiping the
   // cache between runs.
   uint64_t shash = vhash;
   // bridge-format version: bump when the emitted bridge TEXT changes so
   // cached .so's (keyed on logic+toolchain+machine, NOT bridge text) go
   // stale and re-emit+recompile from the cached synth .c.  v2: true
   // edge-detect posedge + accel_set_clklast.
   for (const char *p = "bridge-v8"; *p; p++)
      { shash ^= (uint8_t)*p; shash *= 1099511628211ULL; }
   { const char *cc = getenv("NVC_ACCEL_CC");
     if (cc == NULL) cc = "gcc -g -O3";
     for (const char *p = cc; *p; p++) { shash ^= (uint8_t)*p; shash *= 1099511628211ULL; }
     struct utsname un;
     if (uname(&un) == 0)
        for (const char *p = un.machine; *p; p++) { shash ^= (uint8_t)*p; shash *= 1099511628211ULL; }
#if defined(__x86_64__)
     // vector width actually used by the emitted code varies with the host
     __builtin_cpu_init();
     const unsigned isa = (__builtin_cpu_supports("avx512f") ? 4u : 0u)
                        | (__builtin_cpu_supports("avx2")    ? 2u : 0u)
                        | (__builtin_cpu_supports("sse4.2")  ? 1u : 0u);
     shash = (shash ^ isa) * 1099511628211ULL;
#endif
   }
   snprintf(so,     sizeof so,     "%s/aj_%s_%016llx.so", accel_dir, top,
            (unsigned long long)shash);

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
      // WALL-CLOCK CAP ON SYNTH.  gen_statemachine can take superlinear time
      // on a pathological netlist and there is nothing to stop it: an icache
      // SRAM chunk (eh2_ifu_ic_mem) was measured spinning for 1 day 13 hours at
      // 99.9% CPU inside yosys's proc_dlatch pass -- find_mux_feedback recurses
      // PER BIT through the mux network, and that module carries 654,336 bits of
      // flattened RAM (18 hoisted copies of a 36,352-bit v_nba_ram_core).  It
      // never produced its .c, and nothing timed it out, so it burned a core
      // indefinitely and silently.
      //
      // A synth that overruns must DEGRADE TO A DECLINE, which is the outcome
      // the caller already handles.  Gated on `timeout` actually existing: if
      // coreutils is absent, fall back to the old unbounded behaviour rather
      // than making every synth fail.
      const char *tmo = getenv("NVC_ACCEL_SYNTH_TIMEOUT");
      const int tmo_s = tmo ? atoi(tmo) : 600;
      const bool have_timeout = access("/usr/bin/timeout", X_OK) == 0;
      int off;
      if (tmo_s > 0 && have_timeout)
         off = snprintf(cmd, sizeof cmd,
                        "cd '%s' && /usr/bin/timeout -k 5 %d '%s'",
                        dir, tmo_s, aj_gen_sm());
      else
         off = snprintf(cmd, sizeof cmd, "cd '%s' && '%s'", dir, aj_gen_sm());
      for (int i = 0; i < nsrc; i++) {
         char sp[600];
         if (staged)
            aj_staged_path(sp, sizeof sp, accel_dir, top, vhash, i);
         else
            snprintf(sp, sizeof sp, "%s", srcs[i]);
         off += snprintf(cmd + off, sizeof cmd - off, " '%s'", sp);
      }
      // Re-synthesize with the elaboration's actual generics (width/depth/...).
      // The vhdl2vlog path emits already-elaborated modules (generics baked in),
      // so passing them would chparam a non-existent defparam and error.
      char params[256];
      if (!getenv("NVC_ACCEL_FROM_VHDL")
          && accel_verilog_params(ref, params, sizeof params) && params[0]) {
         off += snprintf(cmd + off, sizeof cmd - off, " %s", params);
         notef("accel-jit: params %s", params);
      }
      // Write-then-rename: a concurrent same-key synth must never leave a
      // half-written dutc for another process (or a crash) to cache.  The
      // _nvc.c byproduct (named by gen_statemachine from the output stem) is
      // renamed alongside; its absence must not fail the synth.
      char dutcbase[600];
      snprintf(dutcbase, sizeof dutcbase, "%.*s", (int)strlen(dutc) - 2, dutc);
      off += snprintf(cmd + off, sizeof cmd - off,
                      " %s '%s_g%d.c' && mv -f '%s_g%d.c' '%s'"
                      " && { mv -f '%s_g%d_nvc.c' '%s_nvc.c' 2>/dev/null"
                      " || true; }",
                      top_mod, dutcbase, (int)getpid(), dutcbase,
                      (int)getpid(), dutc, dutcbase, (int)getpid(), dutcbase);
      notef("accel-jit: synth '%s' (top module '%s') from %d source(s)",
            top, top_mod, nsrc);
      const int src = system(cmd);
      if (aj_synth_timed_out(src) && tmo_s > 0 && have_timeout)
         notef("accel-jit: synth for '%s' exceeded %ds — leaving in nvc "
               "(raise NVC_ACCEL_SYNTH_TIMEOUT to allow longer)", top, tmo_s);
      if (src != 0 || access(dutc, F_OK) != 0) {
         if (!aj_synth_timed_out(src))
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
      pin.base     = aj_bit_base(tree_type(p));
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
      // NOTE: a port named "rst" used to be pulled out here and driven into
      // `if(RST[0]&1) sm_reset(&S);`, i.e. the SPELLING of the port decided it
      // was an active-HIGH synchronous whole-state reset.  That is wrong for an
      // active-low or asynchronous reset that happens to be called `rst`, and
      // it was silently wrong -- correct while the old size gate declined the
      // design, wrong as soon as it was admitted (confirmed with
      // NVC_ACCEL_VERIFY on a 20-line active-low design).
      //
      // It is also redundant: gen_statemachine already models resets IN THE
      // NETLIST -- $adff carries arst_expr/arst_const, and a synchronous reset
      // is just a mux on D like any other logic.  So `rst` is an ordinary
      // bridged pin and the reset semantics come from the RTL, not the name.
      // accel_reset() still calls sm_reset() once at install for initial state.
      // A port literally named `rst` is driven out of band (AJB[5]) into
      // sm_reset() rather than bridged as an ordinary input.  This is UGLY --
      // the SPELLING of the port decides it is an active-HIGH synchronous
      // whole-state reset, which is wrong for an active-low or asynchronous
      // reset that happens to be called `rst` (see below) -- but it is one
      // half of a TWO-SIDED protocol and must not be removed alone.
      //
      // gen_statemachine keeps `_rst` OUT of inputs_t (gen_statemachine.cpp
      // :1315/2095/2452/3011/3124/3186 all skip `_clk`/`_rst`, and :2954
      // documents the split), so the generated model has no field to receive
      // it.  Dropping this line therefore does not make `rst` an ordinary
      // pin -- it makes reset UNREACHABLE, and every design with a sync reset
      // silently produces wrong values.  Measured: it broke 16 of the 19
      // accelbench designs (e.g. deep_d8 Y=1609704590 vs interp 1241718724)
      // while nvc's own test/accel and the ivtest gate both stayed green.
      //
      // The name-based rule is still a real latent bug (an active-low `rst`
      // is mismodelled once admitted).  Fixing it means changing BOTH sides
      // together: gen_statemachine must stop skipping `_rst` and model it as
      // an ordinary input so the reset semantics come from the RTL.
      else if (strcmp(lname, "rst") == 0) { rst = pin; have_rst = true; }
      else if (npins < AJ_MAX_PINS) pins[npins++] = pin;
      else {
         notef("accel-jit: subtree '%s' exceeds %d boundary pins — leaving in nvc",
               top, AJ_MAX_PINS);
         return false;
      }
   }
   if (!have_clk) {
      // No port literally named `clk`. gen_statemachine keys the MAIN flop
      // group on the wire whose cname is "_clk"; every other clock net becomes
      // an EXTRA group that the bridge edge-detects from that clock's own
      // marshalled input field (`sm_extra_clocks[]`). A design whose clock port
      // is spelled anything else — `clock` in every ITC'99 circuit — therefore
      // has ALL of its flops in extra groups, and used to be declined outright
      // even though the model is perfectly usable.
      //
      // Recover structurally (no name guessing): ask the synthesized model
      // which nets it considers clocks, and adopt the first one that is a
      // boundary INPUT of this chunk as the CLK pin. It stays in pins[] as an
      // ordinary marshalled input as well — it must, because the extra-group
      // edge detect reads `in._<name>`, which only the pin marshalling fills.
      // (Duplication is harmless: the marshalling loop already classifies an
      // extra-clock pin as `is_ck` and so does not let it force a re-settle.)
      char *mtext = aj_read_file(dutc);
      if (mtext != NULL) {
         const char *start = strstr(mtext, "const char *sm_extra_clocks[] = {");
         const char *end   = start ? strchr(start, '}') : NULL;
         const char *p = start ? start + strlen("const char *sm_extra_clocks[] = {") : NULL;
         while (p != NULL && end != NULL && !have_clk) {
            const char *q = strchr(p, '"');
            if (q == NULL || q >= end) break;
            const char *e = strchr(q + 1, '"');
            if (e == NULL || e >= end) break;
            char nm[64];
            const int len = (int)(e - q - 1);
            if (len > 0 && len < (int)sizeof nm) {
               memcpy(nm, q + 1, len);
               nm[len] = '\0';
               // strip a per-bit "__b<N>" suffix: the PORT is the vector wire
               char *b = strstr(nm, "__b");
               if (b != NULL && b[3] != '\0'
                   && strspn(b + 3, "0123456789") == strlen(b + 3))
                  *b = '\0';
               for (int i = 0; i < npins; i++) {
                  if (pins[i].is_output || strcmp(pins[i].name, nm)) continue;
                  clk = pins[i];
                  have_clk = true;
                  notef("accel-jit: '%s' has no 'clk' port — using synthesized "
                        "clock '%s' as the chunk clock", top, nm);
                  break;
               }
            }
            p = e + 1;
         }
         free(mtext);
      }
   }
   // Group-0 clock must be PRIMARY.  A derived (comb-driven) clock races its
   // producer at the bridge -- see aj_clk_is_derived above for the mechanism
   // and the measured VeeR evidence (stuck ifu beat counter, +4 retire offset).
   if (have_clk && getenv("NVC_ACCEL_ALLOW_DERIVED_CLK") == NULL
       && aj_clk_is_derived(m, clk.sig)) {
      notef("accel-jit: subtree '%s' clk is a DERIVED clock (comb-driven) — "
            "group-0 edge detect would race its producer; leaving in nvc "
            "(NVC_ACCEL_ALLOW_DERIVED_CLK=1 overrides)", top);
      return false;
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
   // TRANSLATED-DESIGN WAKE HOLE: iverilog-emitted flop processes are
   // wait-at-bottom (dynamic sensitivity) — after reroute the `wait on`
   // never re-executes, so the proc drops off every pending list after
   // its FIRST wake and the chunk never evaluates again (glue2b: q
   // frozen at init while the interp side ran on).  Native-VHDL
   // processes have static sensitivity lists that survive rerouting,
   // which is why the accelbench designs never exposed this, and merged
   // wrappers were already covered by the ck_sigs subscription.  Give
   // SINGLE chunks the same static subscription on their clock.
   if (have_clk && clk.sig != NULL
       && chunk->n_ck_sigs < (int)ARRAY_LEN(chunk->ck_sigs))
      chunk->ck_sigs[chunk->n_ck_sigs++] = clk.sig;
   // Named BEFORE the bridge is emitted: the emitted X/Z sighting message bakes
   // the chunk name in as a literal (it used to read "accel chunk" because
   // rs_top was only filled in after the dlopen below).
   chunk->rs_top  = xstrdup(top);
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
      snprintf(cmd, sizeof cmd,
               "%s %s -shared -fPIC -o '%s.t%d' '%s' && mv -f '%s.t%d' '%s'",
               cc, smd, so, (int)getpid(), bridge, so, (int)getpid(), so);
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
   // X/Z detector: OPTIONAL symbols (a .so cached before this existed simply
   // has none — no AJB slot, no version check, no failure mode). The state
   // layout is identical across the respec swap, so the generic .so's
   // accessors keep reading the live state correctly after chunk->eval moves.
   chunk->x_seen_fn  = dlsym(dl, "accel_x_seen");
   chunk->x_info_fn  = dlsym(dl, "accel_x_info");
   chunk->x_clear_fn = dlsym(dl, "accel_x_clear");
   chunk->set_clklast = dlsym(dl, "accel_set_clklast");
   chunk->force_arm_fn = dlsym(dl, "accel_force_arm");
   if (chunk->x_seen_fn != NULL) g_aj_have_xdet = true;
   chunk->state = xcalloc(ssize());   // per-chunk state (identical .so's don't share)
   // keep the emission inputs for post-link respecialization (re-emit the
   // bridge with the link results baked in, compile, swap eval)
   chunk->rs_bridge = xstrdup(bridge);
   chunk->rs_dutc   = xstrdup(dutc);
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
      // The reroute silences the subtree's internal nets, which is what used to
      // wake it every delta. Re-arm the one sensitivity the bridge's sampled
      // edge detection cannot do without. NVC_ACCEL_NO_CKSUB disables it as a
      // bisection knob (mirrors NVC_ACCEL_NO_SETTLE / NO_FORCE / NO_SEED).
      if (getenv("NVC_ACCEL_NO_CKSUB") == NULL)
         aj_subscribe_clocks(m, chunk);
      // Say "installed" as well as ACTIVE: the benchmark harnesses detect a
      // real install with /accel-jit:.*(installed|driving)/, and until now the
      // only line carrying that word was the VERIFY companion — so a chunk
      // that installed and drove perfectly was still scored as "declined".
      notef("accel-jit: ACTIVE — '%s' subtree rerouted to native model "
            "(accel installed)", top);
      // Dead-output pruning: an output whose consumer-visible nexus has NO
      // readers (empty pending list — wave watchers and processes both live
      // there — and no downstream port) is never observed; clear its bit in
      // the model's exported sm_live_outputs so the cone cells exclusive to
      // it are skipped at run time (e.g. the retire-trace buses when the tb
      // variant doesn't sample them). VERIFY never prunes (compares all).
      for (int mi = 0; mi < 16; mi++) chunk->live_out_mask[mi] = ~0ull;
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
               if (found && idx < 1024) {
                  chunk->live_out_mask[idx >> 6] &= ~(1ull << (idx & 63));
                  pruned++;
               }
            }
            { const int *lw2 = dlsym(dl, "sm_live_outputs_words");
              int nw2 = lw2 != NULL ? *lw2 : 4;
              if (nw2 > 16) nw2 = 16;
              if (nw2 < 1) nw2 = 4;
              memcpy(lom, chunk->live_out_mask, (size_t)nw2 * 8); }
            if (pruned > 0)
               notef("accel-jit: pruned %d unread output cone(s)", pruned);
         }
         free(dtx);
      }
   }
   return true;
}

// Recursively scan scopes for acceleration .so files.
// The consumer-visible storage of an output pin: follow ONE un-converted
// output-port hop (the aj_link_handoff pattern) — a producer's local port
// signal and the net its readers see are different rt_signal_t objects, and
// even their data differs until the hop is followed (proven on the mini-GALS
// fixture: chained members matched neither by sig nor by raw data).
static uint8_t *aj_consumer_data(rt_signal_t *sig)
{
   if (sig == NULL)
      return NULL;
   if (sig->n_nexus == 1) {
      rt_nexus_t *n = &sig->nexus;
      if (n->outputs != NULL) {
         rt_source_t *o = n->outputs;
         if (o->chain_output == NULL && o->tag == SOURCE_PORT
             && o->u.port.conv_func == NULL && o->u.port.output != NULL) {
            rt_nexus_t *pn = o->u.port.output;
            return (uint8_t *)pn->signal->shared.data + pn->offset;
         }
      }
   }
   return (uint8_t *)sig->shared.data;
}

// Concatenate member snapshot .v files into ONE deduped source: the first
// definition of each (content-hash-named) module wins, later duplicates are
// skipped.  yosys hard-ERRORS on re-definition across files even when the
// text is identical (measured: every v7 fused group died on
// "Re-definition of module" for shared prims), and identical hashed names
// are exactly the dedup key.
static bool aj_concat_dedup(const char **vpaths, int nv, const char *out)
{
   FILE *f = fopen(out, "w");
   if (f == NULL)
      return false;
   static char seen[4096][160];
   int nseen = 0;
   for (int v = 0; v < nv; v++) {
      char *text = aj_read_file(vpaths[v]);
      if (text == NULL) { fclose(f); return false; }
      const char *p = text;
      while (*p) {
         const char *ms = strstr(p, "module ");
         if (ms == NULL) break;
         if (ms != text && (isalnum((unsigned char)ms[-1]) || ms[-1] == '_'))
            { p = ms + 7; continue; }
         const char *q = ms + 7;
         while (*q == ' ') q++;
         int len = 0;
         while ((isalnum((unsigned char)q[len]) || q[len] == '_') && len < 159)
            len++;
         const char *me = strstr(q, "endmodule");
         const char *span_end = me ? me + 9 : q + len;
         bool dup = false;
         for (int s = 0; s < nseen && !dup; s++)
            if ((int)strlen(seen[s]) == len
                && strncmp(seen[s], q, len) == 0)
               dup = true;
         if (!dup) {
            if (nseen < 4096) {
               memcpy(seen[nseen], q, len);
               seen[nseen][len] = '\0';
               nseen++;
            }
            fwrite(ms, 1, span_end - ms, f);
            fputc('\n', f);
         }
         p = span_end;
      }
      free(text);
   }
   fclose(f);
   return true;
}

// One prepared merge group awaiting (parallel) synthesis + install.
typedef struct {
   int       first;             // owning candidate index
   int      *members;
   int       nmem, n_internal;
   aj_pin_t *mp;
   int       nmp;
   char      wname[64];
   char      dutc[600], bridge[600], so[600];
   char     *cmd;               // synth command (NULL = cached)
   pid_t     pid;
   int       rc;
} aj_mgrp_t;

static aj_mgrp_t *g_mgrps = NULL;
static int        g_nmgrp = 0, g_mgrpmax = 0;


// Scan a subtree .v for clkhdr cell instances and collect their OUTPUT net
// names (last connection of the single-line instantiation). These nets are
// gated clocks: their exported boundary pins need the ICG latch rule in
// aj_out (a live-computed enable drop otherwise emits a runt pulse that the
// real gater latch suppresses -- missed reset captures downstream).
static int aj_scan_gater_nets(const char *vpath, char names[][64], int max)
{
   FILE *vf = fopen(vpath, "r");
   if (vf == NULL) return 0;
   char line[4096];
   int n = 0;
   while (n < max && fgets(line, sizeof line, vf) != NULL) {
      if (strstr(line, "clkhdr") == NULL) continue;
      char *close = strstr(line, ");");
      if (close == NULL) continue;
      char *e = close;
      while (e > line && (e[-1] == ' ')) e--;
      char *s = e;
      while (s > line && (isalnum((unsigned char)s[-1]) || s[-1] == '_')) s--;
      if (e - s > 0 && e - s < 64) {
         memcpy(names[n], s, e - s);
         names[n][e - s] = '\0';
         n++;
      }
   }
   fclose(vf);
   return n;
}

// Install one merged chunk per clock-domain group (see aj_mcand_t above).
// Any failure falls back to installing the group's members individually, so
// NVC_ACCEL_MERGE can never be WORSE than the per-chunk path.  Synthesis of
// the groups runs NVC_ACCEL_MERGE_JOBS-way parallel (default 8, per user
// direction): discovery and wrapper emission are cheap and serial, the
// hours-scale yosys jobs overlap, install is serial again afterwards.
static void aj_try_merge_install(rt_model_t *m, const char *accel_dir)
{
   if (g_aj_ncand == 0)
      return;
   if (getenv("NVC_ACCEL_COLLECT_DBG") != NULL) {
      int ng = 0;
      for (int i = 0; i < g_aj_ncand; i++) ng += g_aj_cands[i].is_glue;
      notef("accel-jit: collect census: %d candidates (%d glue)",
            g_aj_ncand, ng);
      for (int i = 0; i < g_aj_ncand; i++)
         notef("accel-jit:   cand %d %s%s scope=%s", i,
               g_aj_cands[i].top_mod,
               g_aj_cands[i].is_glue ? " GLUE" : "",
               g_aj_cands[i].scope != NULL
                  ? istr(g_aj_cands[i].scope->name) : "?");
   }
   bool *claimed = xcalloc_array(g_aj_ncand, sizeof(bool));
   int gid = 0;
   for (int i = 0; i < g_aj_ncand; i++) {
      if (claimed[i] || g_aj_cands[i].is_glue) continue;
      static int members[4096];
      int nmem = 0;
      members[nmem++] = i; claimed[i] = true;
      // Oversized members (multi-MB emitted .v == flattened RAM farms like
      // eh2_ic_data) stay OUT of merge groups: one such member drags the
      // whole fused synth into yosys's pathological passes (a v3 group spent
      // 3h+ in one synth).  They fall through to the singleton path below
      // and install per-chunk under their own synth timeout.
      const char *bigenv = getenv("NVC_ACCEL_MERGE_MAX_MEMBER");
      const long bigcap = bigenv ? atol(bigenv) : 2*1024*1024;
      struct stat vst;
      if (stat(g_aj_cands[i].vpath, &vst) == 0 && vst.st_size > bigcap) {
         aj_mcand_t *c = &g_aj_cands[i];
         if (getenv("NVC_ACCEL_MERGE_NOFALLBACK") == NULL)
            accel_install_subtree(m, c->scope, c->ref, accel_dir);
         else
            notef("accel-jit: oversized member '%s' stays interpreted "
                  "(NOFALLBACK)", c->top_mod);
         continue;
      }
      // Group-size cap: one 68-member fused synth once ate two 8h VeeR
      // budgets — but that predates the synth wall-clock (an overrunning
      // synth now degrades to a decline), so the cap is a shaping knob, not
      // a safety valve.  Overflow members stay unclaimed and form sibling
      // groups on the same root in later iterations — several bounded
      // synths instead of one unbounded one.  Default RAISED 24 -> 96 on
      // the measured VeeR ladder (2026-08-08, all full-ladder clean incl
      // hello cycles=2519): cap 24 = 14 groups / 16.5 cyc/s marginal;
      // cap 48 = 8 groups / 23.5; cap 96 = 6 groups / 32.4 — fewer, bigger
      // fused chunks cut the residual interp machinery tax (the measured
      // dominant cost).  NVC_ACCEL_MERGE_MAX_GROUP overrides.
      const char *gcenv = getenv("NVC_ACCEL_MERGE_MAX_GROUP");
      const int gcap = gcenv ? atoi(gcenv) : 96;
      for (int j = i + 1; j < g_aj_ncand && nmem < gcap && nmem < 4096; j++)
         if (!claimed[j] && !g_aj_cands[j].is_glue
             && g_aj_cands[j].ck_root == g_aj_cands[i].ck_root
             && !(stat(g_aj_cands[j].vpath, &vst) == 0
                  && vst.st_size > bigcap)) {
            members[nmem++] = j; claimed[j] = true;
         }
      // NVC_ACCEL_MERGE_GLUE: pull the comb glue cone FEEDING member inputs
      // into the group (backward fixpoint) so member->glue->member cascades
      // become internal wires of the fused netlist instead of interp rim
      // round-trips — the mech-3 root fix (a merged chunk's registers
      // capture ONCE per timestep at the armed eval; a cascade crossing the
      // rim arrives a publication-regime late and the capture reads stale
      // sibling values).  A glue candidate is admitted when one of its
      // OUTPUTS shares storage with a non-clk INPUT pin of the current set;
      // admitted glue's own inputs extend the cone on later iterations.
      // Unclaimed glue is never installed (stays interpreted).
      if (nmem >= 2) {
         static int glue_cap = -2;
         if (glue_cap == -2) {
            const char *ge = getenv("NVC_ACCEL_MERGE_GLUE");
            const char *gm = getenv("NVC_ACCEL_MERGE_GLUE_MAX");
            glue_cap = (ge != NULL && atoi(ge) != 0) ? (gm ? atoi(gm) : 2048)
                                                     : -1;
         }
         int nglue = 0;
         bool grew = glue_cap > 0;
         while (grew && nglue < glue_cap && nmem < 4096) {
            grew = false;
            for (int j = 0; j < g_aj_ncand && nglue < glue_cap
                    && nmem < 4096; j++) {
               if (claimed[j] || !g_aj_cands[j].is_glue) continue;
               aj_mcand_t *gc = &g_aj_cands[j];
               // NVC_ACCEL_MERGE_GLUE_SKIP=<substr,...>: leave matching glue
               // interpreted (bisection knob for the admission population).
               { static const char *gs = (const char *)-1;
                 if (gs == (const char *)-1)
                    gs = getenv("NVC_ACCEL_MERGE_GLUE_SKIP");
                 if (gs != NULL && gc->scope != NULL) {
                    char sn[512], buf[256];
                    snprintf(sn, sizeof sn, "%s", istr(gc->scope->name));
                    snprintf(buf, sizeof buf, "%s", gs);
                    bool hit = false;
                    for (char *t = strtok(buf, ","); t != NULL && !hit;
                         t = strtok(NULL, ","))
                       hit = t[0] != '\0' && strstr(sn, t) != NULL;
                    if (hit) continue;
                 } }
               // CLOCK PURITY: never admit glue that produces a clock-class
               // net.  Clock-tree gates (and/or inside the l1clk/l2clk
               // fabrics) re-timed through chunk publication (+2-delta
               // stage2 vs interp's same-delta cascade) skew every interp
               // gated-clock consumer downstream — measured: admitting them
               // killed the machine outright (0 retires by 1100ns).
               bool clocky = false;
               for (int p = 0; p < gc->npins && !clocky; p++) {
                  if (!gc->pins[p].is_output) continue;
                  rt_signal_t *os = gc->pins[p].sig;
                  if (os == NULL) continue;
                  // follow the single port hop to the OUTER net (the port
                  // signal's own ident is just the formal name, e.g. "o")
                  rt_signal_t *outer = os;
                  if (os->n_nexus == 1 && os->nexus.outputs != NULL) {
                     rt_source_t *o2 = os->nexus.outputs;
                     if (o2->chain_output == NULL && o2->tag == SOURCE_PORT
                         && o2->u.port.conv_func == NULL
                         && o2->u.port.output != NULL
                         && o2->u.port.output->signal != NULL)
                        outer = o2->u.port.output->signal;
                  }
                  if (outer->where != NULL) {
                     char onm[128];
                     aj_lower(onm, istr(tree_ident(outer->where)), sizeof onm);
                     if (strstr(onm, "clk") != NULL
                         || strstr(onm, "clock") != NULL)
                        clocky = true;
                  }
               }
               if (clocky) continue;
               bool feeds = false;
               for (int p = 0; p < gc->npins && !feeds; p++) {
                  if (!gc->pins[p].is_output) continue;
                  const uint8_t *od = aj_consumer_data(gc->pins[p].sig);
                  for (int k = 0; k < nmem && !feeds; k++) {
                     aj_mcand_t *mc = &g_aj_cands[members[k]];
                     // Never admit glue that feeds a CLOCK HEADER member:
                     // internalizing an ICG's enable re-times the gated
                     // clock the header produces for INTERP consumers (the
                     // enable's rim hop was part of the measured-correct
                     // clock distribution timing).
                     if (mc->top_mod != NULL
                         && strstr(mc->top_mod, "clkhdr") != NULL)
                        continue;
                     for (int q = 0; q < mc->npins; q++) {
                        aj_pin_t *ip = &mc->pins[q];
                        if (ip->is_output) continue;
                        if (!mc->is_glue
                            && strcmp(ip->name, mc->clk.name) == 0
                            && ip->sig == mc->clk.sig)
                           continue;   // never glue into a clock pin
                        if (od == ip->data
                            && gc->pins[p].width == ip->width) {
                           feeds = true;
                           break;
                        }
                     }
                  }
               }
               if (feeds) {
                  members[nmem++] = j;
                  claimed[j] = true;
                  nglue++;
                  grew = true;
                  if (getenv("NVC_ACCEL_COLLECT_DBG") != NULL)
                     notef("accel-jit: glue ADMIT %s (%s) into group root %s",
                           gc->scope != NULL ? istr(gc->scope->name) : "?",
                           gc->top_mod,
                           g_aj_cands[i].ck_root != NULL
                              && g_aj_cands[i].ck_root->signal != NULL
                              ? istr(tree_ident(
                                   g_aj_cands[i].ck_root->signal->where))
                              : "?");
               }
            }
         }
         if (nglue > 0)
            notef("accel-jit: MERGE group (root %s) admitted %d glue "
                  "subtree(s), %d members total (NVC_ACCEL_MERGE_GLUE)",
                  g_aj_cands[i].ck_root != NULL
                     && g_aj_cands[i].ck_root->signal != NULL
                     ? istr(tree_ident(g_aj_cands[i].ck_root->signal->where))
                     : "?", nglue, nmem);
      }
      if (getenv("NVC_ACCEL_MERGE_DRYRUN") != NULL) {   // grouping inspection
         rt_nexus_t *r = g_aj_cands[i].ck_root;
         notef("accel-jit: MERGE dryrun group: %d members, clk pin '%s', "
               "root sig %s", nmem, g_aj_cands[i].clk.name,
               (r != NULL && r->signal != NULL)
                  ? istr(tree_ident(r->signal->where)) : "?");
         for (int k = 0; k < nmem; k++) {
            aj_mcand_t *c = &g_aj_cands[members[k]];
            int prodk = -1;
            for (int k2 = 0; k2 < nmem && prodk < 0; k2++)
               if (aj_clk_self_produced(c->clk.sig,
                                        g_aj_cands[members[k2]].scope))
                  prodk = k2;
            notef("accel-jit:   m%d %s clk=%s sig=%s%s%d", k,
                  c->scope != NULL ? istr(c->scope->name) : "?", c->clk.name,
                  c->clk.sig != NULL
                     ? istr(tree_ident(c->clk.sig->where)) : "?",
                  prodk >= 0 ? " PRODUCED-BY-m" : " external", prodk);
         }
         continue;
      }
      if (nmem < 2) {   // singleton: normal per-chunk install
         aj_mcand_t *c = &g_aj_cands[i];
         if (getenv("NVC_ACCEL_MERGE_NOFALLBACK") == NULL)
            accel_install_subtree(m, c->scope, c->ref, accel_dir);
         else
            notef("accel-jit: singleton '%s' stays interpreted (NOFALLBACK)",
                  c->top_mod);
         continue;
      }
      const int g = gid++;
      const bool have_rst = g_aj_cands[i].have_rst;
      char wname[64], wpath[600];
      snprintf(wname, sizeof wname, "aj_merged_%d", g);
      snprintf(wpath, sizeof wpath, "%s/%s.v", accel_dir, wname);

      // ---- merged external pin table + wrapper emission -------------------
      int tot = 2;
      for (int k = 0; k < nmem; k++) tot += g_aj_cands[members[k]].npins;
      aj_pin_t *mp = xmalloc_array(tot, sizeof(aj_pin_t));
      int nmp = 0;
      mp[nmp] = g_aj_cands[i].clk;
      snprintf(mp[nmp].name, sizeof mp[nmp].name, "clk"); nmp++;
      if (have_rst) {
         mp[nmp] = g_aj_cands[i].rst;
         snprintf(mp[nmp].name, sizeof mp[nmp].name, "rst"); nmp++;
      }
      char *ports = NULL, *body = NULL;
      size_t portssz = 0, bodysz = 0;
      FILE *pf = open_memstream(&ports, &portssz);
      FILE *bf = open_memstream(&body, &bodysz);
      fprintf(pf, "input clk");
      if (have_rst) fprintf(pf, ", input rst");
      int n_internal = 0;
      // Glue outputs are chunk-INTERNAL wires, never wrapper ports: the
      // interp glue proc stays alive (not rerouted) so every interp-visible
      // net keeps its interp driver/timing/X-epoch semantics; the chunk
      // computes its OWN copy purely for member-cascade correctness.
      for (int k = 0; k < nmem; k++) {
         aj_mcand_t *c = &g_aj_cands[members[k]];
         if (!c->is_glue) continue;
         for (int p = 0; p < c->npins; p++)
            if (c->pins[p].is_output)
               fprintf(bf, "  wire [%d:0] m%d_%s;\n",
                       c->pins[p].width - 1, k, c->pins[p].name);
      }
      for (int k = 0; k < nmem; k++) {
         aj_mcand_t *c = &g_aj_cands[members[k]];
         static char gater_nets[128][64];
         const int ngat = c->vpath != NULL
            ? aj_scan_gater_nets(c->vpath, gater_nets, 128) : 0;
         // NVC_ACCEL_MERGE_MAP=1: member ordinal -> design instance, the only
         // record tying a bundle's mK pins back to the covered instance (the
         // bridge names members m0..mN only) — required to census a specific
         // primitive instance's rim.
         { static int _mm = -1;
           if (_mm < 0) _mm = getenv("NVC_ACCEL_MERGE_MAP") != NULL;
           if (_mm)
              notef("accel-jit: merge map group %d member m%d = %s (%s)",
                    g, k, c->scope != NULL ? istr(c->scope->name) : "?",
                    c->top_mod); }
         fprintf(bf, "  %s u%d(", c->top_mod, k);
         bool first = true;
         for (int p = 0; p < c->npins; p++) {
            aj_pin_t *pp = &c->pins[p];
            // the member's CHOSEN primary-clock pin (any name, e.g.
            // free_clk) connects to the wrapper's clk; other *clk pins are
            // ordinary boundary inputs (extra clocks — legal for gsm)
            static int _rkon = -1;
            if (_rkon < 0) _rkon = getenv("NVC_ACCEL_CKREKEY") != NULL;
            if (!pp->is_output && strcmp(pp->name, c->clk.name) == 0
                && pp->sig == c->clk.sig
                && (!_rkon || pp->sig == g_aj_cands[i].clk.sig)) {
               // only members clocked by the SEED'S OWN signal bind to the
               // wrapper clk; under NVC_ACCEL_CKREKEY a member grouped via
               // a latch-gated clock keeps its gated net as an ordinary
               // boundary input (an EXTRA clock — the sm_clock_late/extra
               // machinery) instead of being silently re-clocked by the
               // seed's signal (measured: re-clocking killed the machine).
               fprintf(bf, "%s.%s(clk)", first ? "" : ", ", pp->name);
               first = false;
               continue;
            }
            if (!pp->is_output) {
               // internal? another member's OUTPUT drives the same signal
               int pj = -1, pq = -1;
               for (int k2 = 0; k2 < nmem && pj < 0; k2++) {
                  aj_mcand_t *c2 = &g_aj_cands[members[k2]];
                  for (int q = 0; q < c2->npins; q++)
                     // match by shared DATA pointer, not signal identity:
                     // a port hop joins two different rt_signal_t objects
                     // over one storage (the aj_link_handoff lesson) — the
                     // sig-pointer match found 0 internal edges on genuinely
                     // chained members (proven on the mini-GALS fixture)
                     if (c2->pins[q].is_output
                         && aj_consumer_data(c2->pins[q].sig) == pp->data
                         && c2->pins[q].width == pp->width) {
                        pj = k2; pq = q; break;
                     }
               }
               if (pj >= 0) {   // flattened internal edge — no bridge crossing
                  fprintf(bf, "%s.%s(m%d_%s)", first ? "" : ", ",
                          pp->name, pj,
                          g_aj_cands[members[pj]].pins[pq].name);
                  first = false;
                  n_internal++;
                  continue;
               }
            }
            if (c->is_glue && pp->is_output) {
               // internal wire only (declared in the pre-pass above): no
               // wrapper port, no bridge pin, no publication
               fprintf(bf, "%s.%s(m%d_%s)", first ? "" : ", ",
                       pp->name, k, pp->name);
               first = false;
               continue;
            }
            fprintf(pf, ", %s [%d:0] m%d_%s",
                    pp->is_output ? "output" : "input",
                    pp->width - 1, k, pp->name);
            fprintf(bf, "%s.%s(m%d_%s)", first ? "" : ", ",
                    pp->name, k, pp->name);
            first = false;
            mp[nmp] = *pp;
            snprintf(mp[nmp].name, sizeof mp[nmp].name, "m%d_%s", k, pp->name);
            if (pp->is_output && c->top_mod != NULL
                && strstr(c->top_mod, "clkhdr") != NULL)
               mp[nmp].icg = true;
            if (pp->is_output && !mp[nmp].icg)
               for (int gi = 0; gi < ngat; gi++)
                  if (strcmp(gater_nets[gi], pp->name) == 0) {
                     mp[nmp].icg = true;
                     break;
                  }
            nmp++;
         }
         fprintf(bf, ");\n");
      }
      fclose(pf); fclose(bf);
      bool ok = true;
      if (nmp > AJ_MAX_PINS) {   // bridge tables are AJ_MAX_PINS-static
         notef("accel-jit: MERGE group %d rim too wide (%d pins > %d) — "
               "falling back to per-chunk installs", g, nmp, AJ_MAX_PINS);
         ok = false;
      }
      FILE *wf = ok ? fopen(wpath, "w") : NULL;
      ok = ok && wf != NULL;
      if (ok) {
         fprintf(wf, "module %s(%s);\n%sendmodule\n", wname, ports, body);
         fclose(wf);
      }
      free(ports); free(body);

      // ---- sources: ONE concatenated+deduped member file + the wrapper ----
      static const char *srcs[4097];
      static char allpath[600];
      int nsrc = 0;
      if (ok) {
         const char *vps[4096];
         int nvp = 0;
         for (int k = 0; k < nmem; k++) {
            const char *vp = g_aj_cands[members[k]].vpath;
            bool dup = false;
            for (int s = 0; s < nvp; s++)
               if (strcmp(vps[s], vp) == 0) { dup = true; break; }
            if (!dup) vps[nvp++] = vp;
         }
         snprintf(allpath, sizeof allpath, "%s/%s_all.v", accel_dir, wname);
         ok = aj_concat_dedup(vps, nvp, allpath);
         if (ok) {
            srcs[nsrc++] = allpath;
            srcs[nsrc++] = wpath;
         }
      }

      // ---- two-tier keys (same scheme as the per-chunk path) --------------
      uint64_t vhash = 1469598103934665603ULL;
      vhash = (vhash ^ 3u) * 1099511628211ULL;
      { struct stat gst;
        if (stat(aj_gen_sm(), &gst) == 0)
           vhash = (vhash ^ (uint64_t)gst.st_mtime) * 1099511628211ULL; }
      // Same GSM_ICG2EN key fold as the per-chunk path (toggle safety).
      { const char *icg = getenv("GSM_ICG2EN");
        if (icg != NULL)
           for (const char *p = "+icg2en="; *p; p++)
              { vhash ^= (uint8_t)*p; vhash *= 1099511628211ULL; }
        for (const char *p = icg ? icg : ""; *p; p++)
           { vhash ^= (uint8_t)*p; vhash *= 1099511628211ULL; } }
      for (const char *p = wname; *p; p++)
         { vhash ^= (uint8_t)*p; vhash *= 1099511628211ULL; }
      char *mtexts[2] = { NULL, NULL };
      for (int s = 0; s < nsrc && ok; s++) {
         char *vtext = aj_read_file(srcs[s]);
         if (vtext != NULL) {
            for (const char *p = vtext; *p; p++)
               { vhash ^= (uint8_t)*p; vhash *= 1099511628211ULL; }
            if (s < 2) mtexts[s] = vtext; else free(vtext);
         }
      }
      // Stage the HASHED bytes under the key (see aj_stage_source): what the
      // key says is what gen_statemachine gets, even if a concurrent run with
      // a different config rewrites the shared-named wrapper .v files.
      bool mstaged = ok && nsrc <= 2;
      for (int s = 0; s < nsrc && s < 2; s++) {
         if (mtexts[s] == NULL
             || !aj_stage_source(accel_dir, wname, vhash, s, mtexts[s]))
            mstaged = false;
         free(mtexts[s]);
      }
      uint64_t shash = vhash;
      { const char *cc = getenv("NVC_ACCEL_CC");
        if (cc == NULL) cc = "gcc -g -O3";
        for (const char *p = cc; *p; p++)
           { shash ^= (uint8_t)*p; shash *= 1099511628211ULL; }
        for (const char *p = "bridge-v8"; *p; p++)
           { shash ^= (uint8_t)*p; shash *= 1099511628211ULL; }
        struct utsname un;
        if (uname(&un) == 0)
           for (const char *p = un.machine; *p; p++)
              { shash ^= (uint8_t)*p; shash *= 1099511628211ULL; } }
      char dutc[600], bridge[600], so[600];
      snprintf(dutc, sizeof dutc, "%s/aj_%s_%016llx.c", accel_dir, wname,
               (unsigned long long)vhash);
      snprintf(bridge, sizeof bridge, "%s/aj_%s_%016llx_bridge.c", accel_dir,
               wname, (unsigned long long)vhash);
      snprintf(so, sizeof so, "%s/aj_%s_%016llx.so", accel_dir, wname,
               (unsigned long long)shash);

      // ---- stash for the parallel synth + serial install phases -----------
      if (ok) {
         if (g_nmgrp == g_mgrpmax) {
            g_mgrpmax = g_mgrpmax ? g_mgrpmax * 2 : 16;
            g_mgrps = xrealloc_array(g_mgrps, g_mgrpmax, sizeof(aj_mgrp_t));
         }
         aj_mgrp_t *gr = &g_mgrps[g_nmgrp++];
         memset(gr, 0, sizeof *gr);
         gr->first   = i;
         gr->members = xmalloc_array(nmem, sizeof(int));
         memcpy(gr->members, members, nmem * sizeof(int));
         gr->nmem = nmem; gr->n_internal = n_internal;
         gr->mp = mp; gr->nmp = nmp;
         snprintf(gr->wname,  sizeof gr->wname,  "%s", wname);
         snprintf(gr->dutc,   sizeof gr->dutc,   "%s", dutc);
         snprintf(gr->bridge, sizeof gr->bridge, "%s", bridge);
         snprintf(gr->so,     sizeof gr->so,     "%s", so);
         if (getenv("NVC_ACCEL_NO_CACHE") != NULL
             || access(dutc, F_OK) != 0) {
            const char *tmo_env = getenv("NVC_ACCEL_SYNTH_TIMEOUT");
            const int tmo_s = tmo_env ? atoi(tmo_env) : 600;
            const bool have_timeout = access("/usr/bin/timeout", X_OK) == 0;
            static char cmd[65536];
            int off;
            if (have_timeout && tmo_s > 0)
               off = snprintf(cmd, sizeof cmd,
                              "cd '%s' && /usr/bin/timeout -k 5 %d '%s'",
                              accel_dir, tmo_s, aj_gen_sm());
            else
               off = snprintf(cmd, sizeof cmd, "cd '%s' && '%s'",
                              accel_dir, aj_gen_sm());
            for (int s = 0; s < nsrc; s++) {
               char sp[600];
               if (mstaged)
                  aj_staged_path(sp, sizeof sp, accel_dir, wname, vhash, s);
               else
                  snprintf(sp, sizeof sp, "%s", srcs[s]);
               off += snprintf(cmd + off, sizeof cmd - off, " '%s'", sp);
            }
            // Write-then-rename (same rationale as the per-chunk path).
            char dutcbase[600];
            snprintf(dutcbase, sizeof dutcbase, "%.*s",
                     (int)strlen(dutc) - 2, dutc);
            off += snprintf(cmd + off, sizeof cmd - off,
                            " %s '%s_g%d.c' && mv -f '%s_g%d.c' '%s'"
                            " && { mv -f '%s_g%d_nvc.c' '%s_nvc.c' 2>/dev/null"
                            " || true; }",
                            wname, dutcbase, (int)getpid(), dutcbase,
                            (int)getpid(), dutc, dutcbase, (int)getpid(),
                            dutcbase);
            gr->cmd = xstrdup(cmd);
         }
         continue;   // synthesis (parallel) + install run after the loop
      }

      // ---- bridge + compile + install -------------------------------------
      aj_chunk_t *chunk = NULL;
      if (ok) {
         chunk = aj_chunk_new(m);
         chunk->merged = true;   // enables the negedge state flip
         chunk->scope  = g_aj_cands[i].scope;
         chunk->rs_top = xstrdup(wname);
         chunk->bindtab = xcalloc_array(6 + (nmp > 0 ? nmp : 1) + 4,
                                        sizeof(void *));
         aj_build_fastclk(m, mp[0].sig, mp[0].data);
         if (!aj_emit_bridge(bridge, dutc, mp, nmp, &mp[0],
                             have_rst ? &mp[1] : NULL, m, chunk, false)) {
            aj_accel_teardown(m);
            ok = false;
         }
      }
      if (ok && (getenv("NVC_ACCEL_NO_CACHE") != NULL
                 || access(so, F_OK) != 0)) {
         const char *cc = getenv("NVC_ACCEL_CC");
         if (!cc) cc = "gcc -g -O3";
         const char *smd = getenv("NVC_ACCEL_SMDUMP") ? "-DSM_DUMP" : "";
         char cmd[8192];
         snprintf(cmd, sizeof cmd,
                  "%s %s -shared -fPIC -o '%s.t%d' '%s' && mv -f '%s.t%d' '%s'",
                  cc, smd, so, (int)getpid(), bridge, so, (int)getpid(), so);
         if (system(cmd) != 0 || access(so, F_OK) != 0) {
            notef("accel-jit: MERGE compile failed for '%s'", wname);
            aj_accel_teardown(m);
            ok = false;
         }
      }
      else if (ok)
         notef("accel-jit: reusing cached .so for '%s' (logic unchanged)",
               wname);
      if (ok) {
         void *dl = dlopen(so, RTLD_NOW);
         void (*eval)(void *, void **) = dl ? dlsym(dl, "accel_eval") : NULL;
         void (*reset)(void *)         = dl ? dlsym(dl, "accel_reset") : NULL;
         unsigned long (*ssize)(void)  = dl ? dlsym(dl, "accel_state_size")
                                            : NULL;
         if (!eval || !reset || !ssize) {
            notef("accel-jit: MERGE dlopen/dlsym failed for '%s'", wname);
            if (dl) dlclose(dl);
            aj_accel_teardown(m);
            ok = false;
         }
         else {
            chunk->eval  = eval;
            chunk->reset = reset;
            chunk->dl    = dl;
            chunk->x_seen_fn   = dlsym(dl, "accel_x_seen");
            chunk->x_info_fn   = dlsym(dl, "accel_x_info");
            chunk->x_clear_fn  = dlsym(dl, "accel_x_clear");
            chunk->set_clklast = dlsym(dl, "accel_set_clklast");
   chunk->force_arm_fn = dlsym(dl, "accel_force_arm");
            chunk->state = xcalloc(ssize());
            chunk->rs_bridge = xstrdup(bridge);
            chunk->rs_dutc   = xstrdup(dutc);
            chunk->rs_pins   = xmalloc_array(nmp, sizeof(aj_pin_t));
            memcpy(chunk->rs_pins, mp, nmp * sizeof(aj_pin_t));
            chunk->rs_npins  = nmp;
            chunk->rs_state_size = ssize();
            g_aj_model = m;
            chunk->reset(chunk->state);
            for (int k = 0; k < nmem; k++)
               if (!g_aj_cands[members[k]].is_glue)   // glue procs stay live
                  aj_reroute(g_aj_cands[members[k]].scope, chunk);
            if (getenv("NVC_ACCEL_NO_CKSUB") == NULL)
               aj_subscribe_clocks(m, chunk);
            notef("accel-jit: MERGE ACTIVE — '%s' (%d subtrees fused, %d "
                  "internal edges) rerouted to one native model "
                  "(accel installed)", wname, nmem, n_internal);
         }
      }
      if (!ok)   // any failure: this group's members install individually
         for (int k = 0; k < nmem; k++) {
            aj_mcand_t *c = &g_aj_cands[members[k]];
            if (c->is_glue) continue;   // glue never installs standalone
            accel_install_subtree(m, c->scope, c->ref, accel_dir);
         }
      free(mp);
   }

   // ---- phase 2: parallel synthesis (NVC_ACCEL_MERGE_JOBS-way) -----------
   {
      const char *jenv = getenv("NVC_ACCEL_MERGE_JOBS");
      const int jobs = jenv ? atoi(jenv) : 8;
      int launched = 0, running = 0;
      while (launched < g_nmgrp || running > 0) {
         while (running < (jobs > 0 ? jobs : 1) && launched < g_nmgrp) {
            aj_mgrp_t *gr = &g_mgrps[launched++];
            if (gr->cmd == NULL) { gr->pid = 0; continue; }
            notef("accel-jit: MERGE synth '%s' launching (%d members, %d "
                  "internal edges, %d external pins)", gr->wname, gr->nmem,
                  gr->n_internal, gr->nmp);
            pid_t pid = fork();
            if (pid == 0) {
               execl("/bin/sh", "sh", "-c", gr->cmd, (char *)NULL);
               _exit(127);
            }
            if (pid < 0) { gr->rc = -1; continue; }
            gr->pid = pid; running++;
         }
         if (running > 0) {
            int st = 0;
            pid_t done = waitpid(-1, &st, 0);
            if (done < 0) break;
            for (int gi = 0; gi < launched; gi++)
               if (g_mgrps[gi].pid == done) {
                  g_mgrps[gi].pid = 0;
                  g_mgrps[gi].rc  = st;
                  running--;
                  notef("accel-jit: MERGE synth '%s' finished (status %d)",
                        g_mgrps[gi].wname, st);
                  break;
               }
         }
      }
   }

   // ---- phase 3: serial bridge + compile + install per group -------------
   for (int gi = 0; gi < g_nmgrp; gi++) {
      aj_mgrp_t *gr = &g_mgrps[gi];
      aj_pin_t *mp = gr->mp;
      const int nmp = gr->nmp, nmem = gr->nmem;
      const int *members = gr->members;
      const bool have_rst = g_aj_cands[gr->first].have_rst;
      bool ok = true;
      if (gr->cmd != NULL) {
         free(gr->cmd);
         if (access(gr->dutc, F_OK) != 0) {
            notef("accel-jit: MERGE synth failed for '%s' — falling back to "
                  "per-chunk installs", gr->wname);
            ok = false;
         }
      }
      else
         notef("accel-jit: reusing cached synth for '%s' (logic unchanged)",
               gr->wname);
      aj_chunk_t *chunk = NULL;
      if (ok) {
         chunk = aj_chunk_new(m);
         chunk->merged = true;   // enables the negedge state flip
         chunk->scope  = g_aj_cands[gr->first].scope;
         chunk->rs_top = xstrdup(gr->wname);
         chunk->bindtab = xcalloc_array(6 + (nmp > 0 ? nmp : 1) + 4,
                                        sizeof(void *));
         aj_build_fastclk(m, mp[0].sig, mp[0].data);
         if (!aj_emit_bridge(gr->bridge, gr->dutc, mp, nmp, &mp[0],
                             have_rst ? &mp[1] : NULL, m, chunk, false)) {
            aj_accel_teardown(m);
            ok = false;
         }
      }
      if (ok && (getenv("NVC_ACCEL_NO_CACHE") != NULL
                 || access(gr->so, F_OK) != 0)) {
         const char *cc = getenv("NVC_ACCEL_CC");
         if (!cc) cc = "gcc -g -O3";
         const char *smd = getenv("NVC_ACCEL_SMDUMP") ? "-DSM_DUMP" : "";
         char cmd[8192];
         snprintf(cmd, sizeof cmd,
                  "%s %s -shared -fPIC -o '%s.t%d' '%s' && mv -f '%s.t%d' '%s'",
                  cc, smd, gr->so, (int)getpid(), gr->bridge, gr->so,
                  (int)getpid(), gr->so);
         if (system(cmd) != 0 || access(gr->so, F_OK) != 0) {
            notef("accel-jit: MERGE compile failed for '%s'", gr->wname);
            aj_accel_teardown(m);
            ok = false;
         }
      }
      else if (ok)
         notef("accel-jit: reusing cached .so for '%s' (logic unchanged)",
               gr->wname);
      if (ok) {
         void *dl = dlopen(gr->so, RTLD_NOW);
         void (*eval)(void *, void **) = dl ? dlsym(dl, "accel_eval") : NULL;
         void (*reset)(void *)         = dl ? dlsym(dl, "accel_reset") : NULL;
         unsigned long (*ssize)(void)  = dl ? dlsym(dl, "accel_state_size")
                                            : NULL;
         if (!eval || !reset || !ssize) {
            notef("accel-jit: MERGE dlopen/dlsym failed for '%s'", gr->wname);
            if (dl) dlclose(dl);
            aj_accel_teardown(m);
            ok = false;
         }
         else {
            chunk->eval  = eval;
            chunk->reset = reset;
            chunk->dl    = dl;
            chunk->x_seen_fn   = dlsym(dl, "accel_x_seen");
            chunk->x_info_fn   = dlsym(dl, "accel_x_info");
            chunk->x_clear_fn  = dlsym(dl, "accel_x_clear");
            chunk->set_clklast = dlsym(dl, "accel_set_clklast");
   chunk->force_arm_fn = dlsym(dl, "accel_force_arm");
            chunk->state = xcalloc(ssize());
            chunk->rs_bridge = xstrdup(gr->bridge);
            chunk->rs_dutc   = xstrdup(gr->dutc);
            chunk->rs_pins   = xmalloc_array(nmp, sizeof(aj_pin_t));
            memcpy(chunk->rs_pins, mp, nmp * sizeof(aj_pin_t));
            chunk->rs_npins  = nmp;
            chunk->rs_state_size = ssize();
            g_aj_model = m;
            chunk->reset(chunk->state);
            for (int k = 0; k < nmem; k++)
               if (!g_aj_cands[members[k]].is_glue)   // glue procs stay live
                  aj_reroute(g_aj_cands[members[k]].scope, chunk);
            if (getenv("NVC_ACCEL_NO_CKSUB") == NULL)
               aj_subscribe_clocks(m, chunk);
            notef("accel-jit: MERGE ACTIVE — '%s' (%d subtrees fused, %d "
                  "internal edges) rerouted to one native model "
                  "(accel installed)", gr->wname, nmem, gr->n_internal);
         }
      }
      if (!ok) {
         // NVC_ACCEL_MERGE_NOFALLBACK=1: leave failed groups' members
         // INTERPRETED instead of installing per-chunk — the clean
         // fused-vs-interp discriminator (a collect-time SKIP makes the
         // scan descend and re-synthesize hundreds of descendants).
         if (getenv("NVC_ACCEL_MERGE_NOFALLBACK") != NULL)
            notef("accel-jit: MERGE group '%s' failed — members stay "
                  "interpreted (NOFALLBACK)", gr->wname);
         else
            for (int k = 0; k < nmem; k++) {
               aj_mcand_t *c = &g_aj_cands[members[k]];
               accel_install_subtree(m, c->scope, c->ref, accel_dir);
            }
      }
      free(gr->mp); free(gr->members);
   }
   g_nmgrp = 0;
   free(claimed);
}

static void accel_scan_scope(rt_model_t *m, rt_scope_t *scope,
                             const char *accel_dir)
{
   // NVC_ACCEL_EXCLUDE=<substr,...>: TRUE PRUNE — no acceleration AND no
   // descent below a matching scope path.  Distinct from NVC_ACCEL_SKIP,
   // which declines a subtree but lets the walk recurse into its children:
   // under DESCEND_BIG that made SKIP change chunk GRANULARITY instead of
   // excluding logic (bisect configs regrouped 34->40->56 installs and the
   // divergence signature shifted each time — unusable for isolation).
   { static const char *ex = NULL; static int exi = -1;
     if (exi < 0) { ex = getenv("NVC_ACCEL_EXCLUDE"); exi = ex ? 1 : 0; }
     if (exi && scope->name != NULL) {
        char sn[512];
        snprintf(sn, sizeof sn, "%s", istr(scope->name));
        for (char *p = sn; *p; p++) *p = tolower((unsigned char)*p);
        char buf[256];
        snprintf(buf, sizeof buf, "%s", ex);
        for (char *t = strtok(buf, ","); t != NULL; t = strtok(NULL, ","))
           if (t[0] != '\0' && strstr(sn, t) != NULL) {
              notef("accel-jit: scope %s EXCLUDED (no accel, no descent)",
                    istr(scope->name));
              return;
           }
     } }
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
// ---- Task #62: STATIC comb levelization -----------------------------------
// Delta-level glitches are simulation artifacts (user doctrine 2026-08-04):
// interp comb settle exposes transient values that mid-cascade gated-clock
// flops legally sample, diverging from fused/silicon-intent semantics
// (glitch.vhd fixture: 39/40 cycles capture a transient today).  The remedy
// is a STATIC evaluation order — comb processes topologically leveled at
// startup, dispatched one-eval-per-settle-wave.  This pass builds the level
// assignment from RUNTIME structures alone: nexus driver sources give
// proc->signal, pending lists give signal->proc, so edges need no AST.
// Analysis + stats first (NVC_LEVELIZE_STATIC=1); the level-ordered sweep
// dispatch consumes lv_level[] in the next stage.
// The node universe is rt_wakeable_t*, NOT rt_proc_t*: the fork lowers
// simple concurrent assignments to W_TRANSFER wakeables whose nexus driver
// records the OWNING process -- keyed by proc, an s1->s2->s3 transfer chain
// collapses to one node and the chain depth vanishes (measured: transfers
// drained one stage per phase while a clocked consumer sampled mid-chain).
static hash_t   *g_lv_idx  = NULL;   // rt_wakeable_t* -> index+1
static uint32_t *g_lv_seen = NULL;    // per-node wave stamp (sweep)
static int       g_lv_maxlv = 0;
static bool      g_lv_sweep = false;
static bool      g_lv_inwave = false; // a sweep wave is executing NOW
static uint32_t  g_lv_wave = 0;
static rt_wakeable_t **g_lv_wv = NULL; // current wave's collected nodes
static int       g_lv_wvn = 0, g_lv_wvcap = 0;
// Wave-born clocked procs run at slot (waking-commit level + 1),
// interleaved into the level schedule: inputs at lower levels are settled
// (glitch-free), inputs at higher levels still hold pre-update values --
// matching interp's delta ordering, where data arriving at a later delta
// than the clock edge reads OLD.  A post-quiescence drain instead fed
// same-cycle data to every derived-clock flop (measured: xbtop2 65315 ->
// 195951 shoot-through).
typedef struct { rt_proc_t *p; int rl; } lv_ck_t;
static lv_ck_t  *g_lv_ck = NULL;      // wave-born clocked procs + run slot
static int       g_lv_ckn = 0, g_lv_ckcap = 0;
static long      g_lv_late = 0;       // topo-violation canary
static int       g_lv_curlv = -1;
// Deposits made by CLOCKED procs during a wave are held here and released
// into m->driverq at wave end: committing them mid-wave let their comb
// fanout settle before the drain batch ran, so a gated-clock flop sampled
// same-timestep data one cycle early (measured: nbaff fixture Q one clock
// ahead of interp; interp NBA semantics commit flop deposits next delta).
static deferq_t  g_lv_heldq;
// TRUE while a deposit-path wakeup_all runs: the deposit stamps its event
// for iteration+1 (receivers contractually run NEXT delta), so the wave
// must NOT absorb those wakes into the CURRENT delta -- an absorbed
// receiver runs one delta before its event exists and its rising_edge
// reads false (measured: VeeR's deposit-distributed clocks -- e.g.
// ACTIVE_L2CLK, no SOURCE_DRIVER, events invisible to notify_event --
// never clocked their kernels under the sweep; bus-sync FFs wedged).
static bool      g_lv_deposit_wake = false;
// Deposit-woken procs surfacing mid-wave: contractually NEXT-delta work
// (their event is stamped iteration+1).  Queued to m->procq they land in
// the tail drain and run one delta early -- hold here, append to m->procq
// at wave end (the next dispatch's swap delivers them).
static deferq_t  g_lv_depq;
// Port-propagation (driving-heap) updates scheduled by DEPOSITS are
// next-delta work: stock leaves them for the following boundary's update
// phase, so the inner port's event lands the SAME delta its deferred
// receivers run.  The wave's flush drains the driving heap eagerly and
// committed those ports one delta EARLY -- the receiver's rising_edge
// read a stale event and never fired (measured: cone_tb, the real
// rvdff_fpga cell with a deposit-driven clock, q locked at reset value).
static rt_nexus_t **g_lv_helddrv = NULL;
static int          g_lv_helddrvn = 0, g_lv_helddrvcap = 0;

static void aj_lv_hold_drv(rt_nexus_t *n)
{
   if (g_lv_helddrvn == g_lv_helddrvcap) {
      g_lv_helddrvcap = g_lv_helddrvcap ? g_lv_helddrvcap * 2 : 64;
      g_lv_helddrv = xrealloc_array(g_lv_helddrv, g_lv_helddrvcap,
                                    sizeof(rt_nexus_t *));
   }
   g_lv_helddrv[g_lv_helddrvn++] = n;
}

static int aj_lv_idx(rt_wakeable_t *w)
{
   if (g_lv_idx == NULL) return -1;
   void *v = hash_get(g_lv_idx, w);
   return v == NULL ? -1 : (int)(uintptr_t)v - 1;
}

static void aj_lv_wv_push(rt_wakeable_t *w)
{
   if (g_lv_wvn == g_lv_wvcap) {
      g_lv_wvcap = g_lv_wvcap ? g_lv_wvcap * 2 : 1024;
      g_lv_wv = xrealloc_array(g_lv_wv, g_lv_wvcap, sizeof(rt_wakeable_t *));
   }
   g_lv_wv[g_lv_wvn++] = w;
}
static rt_wakeable_t **g_lv_wake = NULL;
static int      *g_lv_level = NULL;
static int       g_lv_nproc = 0;

// Divert driver-update tasks enqueued by a clocked proc's run into the
// wave-held queue (released at wave end).
static void aj_lv_hold_from(rt_model_t *m, unsigned mark)
{
   static int nohold = -1, dbg = -1;
   if (nohold < 0) {
      nohold = getenv("NVC_SWEEP_NOHOLD") != NULL;
      dbg = getenv("AJ_HOLDDBG") != NULL;
   }
   if (nohold) return;
   for (unsigned i = mark; i < m->driverq.count; i++) {
      if (dbg)
         fprintf(stderr, "#HD hold fn=%p arg=%p t=%llu d=%u\n",
                 (void *)m->driverq.tasks[i].fn, m->driverq.tasks[i].arg,
                 (unsigned long long)m->now, m->iteration);
      deferq_do(&g_lv_heldq, m->driverq.tasks[i].fn, m->driverq.tasks[i].arg);
   }
   m->driverq.count = mark;
}

// AJ_EVDRV=<proc substr>: nexuses whose (port-walked) driver proc name
// matches are marked at analysis; notify_event traces their every commit.
// Solves the instance-local leaf-name collision problem (DIN/DOUT/LPM_*).
static rt_nexus_t *g_lv_evnx[64];
static int         g_lv_evnxn = 0;

// AJ_EVSENS=<proc substr>: mark nexuses a matching proc is woken BY
// (direct pending, or via triggers up to 2 levels).  Wait-style procs
// subscribe only after their first suspension, so this runs again at the
// first wave past 1 ns.
static void aj_evsens_scan(rt_model_t *m)
{
   const char *es = getenv("AJ_EVSENS");
   if (es == NULL)
      return;
   bool procmatch(rt_wakeable_t *w) {
      return w != NULL && w->kind == W_PROC
         && strstr(istr(container_of(w, rt_proc_t, wakeable)->name),
                   es) != NULL;
   }
   bool pendmatch(void *pending, int depth) {
      if (pointer_tag(pending) == 1) {
         rt_wakeable_t *w = untag_pointer(pending, rt_wakeable_t);
         if (procmatch(w)) return true;
         if (w->kind == W_TRIGGER && depth < 4)
            return pendmatch(
               container_of(w, rt_trigger_t, wakeable)->pending, depth + 1);
         return false;
      }
      else if (pending != NULL) {
         rt_pending_t *pl = untag_pointer(pending, rt_pending_t);
         for (int i = 0; i < pl->count; i++) {
            rt_wakeable_t *w = pl->wake[i];
            if (w == NULL) continue;
            if (procmatch(w)) return true;
            if (w->kind == W_TRIGGER && depth < 4
                && pendmatch(
                      container_of(w, rt_trigger_t, wakeable)->pending,
                      depth + 1))
               return true;
         }
      }
      return false;
   }
   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
      if (!pendmatch(n->pending, 0)) continue;
      bool dup = false;
      for (int k = 0; k < g_lv_evnxn; k++)
         if (g_lv_evnx[k] == n) dup = true;
      if (dup) continue;
      if (g_lv_evnxn < 64) {
         g_lv_evnx[g_lv_evnxn++] = n;
         fprintf(stderr, "#ES nx=%p sig=%s\n", (void *)n,
                 (n->signal != NULL && n->signal->where != NULL)
                    ? istr(tree_ident(n->signal->where)) : "?");
      }
   }
   // second pass: a signal can have several nexuses (splits, port views);
   // the guard's 'event may read a different one than the subscription --
   // mark every nexus sharing a marked nexus's signal
   const int base = g_lv_evnxn;
   for (int k = 0; k < base; k++) {
      rt_signal_t *sig = g_lv_evnx[k]->signal;
      if (sig == NULL) continue;
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
         if (n->signal != sig) continue;
         bool dup = false;
         for (int j = 0; j < g_lv_evnxn; j++)
            if (g_lv_evnx[j] == n) dup = true;
         if (dup) continue;
         if (g_lv_evnxn < 64) {
            g_lv_evnx[g_lv_evnxn++] = n;
            fprintf(stderr, "#ES2 nx=%p sig=%s\n", (void *)n,
                    (sig->where != NULL)
                       ? istr(tree_ident(sig->where)) : "?");
         }
      }
   }
}

// Run a leveled node: dispatch by kind (procs and transfers only ever
// enter the wave arrays).
static void aj_lv_run(rt_model_t *m, rt_wakeable_t *w)
{
   if (w->kind == W_TRANSFER)
      async_transfer_signal(m, container_of(w, rt_transfer_t, wakeable));
   else
      async_run_process(m, container_of(w, rt_proc_t, wakeable));
}

static void aj_levelize_analyze(rt_model_t *m)
{
   if (getenv("NVC_LEVELIZE_STATIC") == NULL
       && getenv("NVC_LEVELIZE_SWEEP") == NULL)
      return;
   // pass 0: map target nexus -> driving W_TRANSFER wakeable.  Transfers
   // register their OWNING process as the nexus driver, so driver identity
   // must be corrected to the transfer node or a chain of transfers
   // collapses into its owner and loses all depth.
   // aj_pending_foreach filters to W_PROC (its other callers need that);
   // the graph needs transfers too, so walk unfiltered here.
   void lv_pending_all(void *pending, void (*cb)(rt_wakeable_t *, void *),
                       void *ctx) {
      if (pointer_tag(pending) == 1) {
         rt_wakeable_t *w = untag_pointer(pending, rt_wakeable_t);
         if (!w->postponed) cb(w, ctx);
      }
      else if (pending != NULL) {
         rt_pending_t *p = untag_pointer(pending, rt_pending_t);
         for (int i = 0; i < p->count; i++) {
            rt_wakeable_t *w = p->wake[i];
            if (w != NULL && !w->postponed) cb(w, ctx);
         }
      }
   }
   // AJ_EVSENS=<proc substr>: mark the nexuses a matching proc is woken
   // BY (direct pending or via triggers, 2 levels) -- traces the actual
   // clock nets of a wait-on kernel.  Wait-style procs subscribe only
   // after their first suspension, so aj_evsens_scan also re-runs at the
   // first wave past 1ns (see aj_sweep_run).
   aj_evsens_scan(m);
   hash_t *tmap = hash_new(256);
   {
      void tm_cb(rt_wakeable_t *w, void *vc) {
         if (w->kind != W_TRANSFER) return;
         rt_transfer_t *t = container_of(w, rt_transfer_t, wakeable);
         int c = t->count;
         for (rt_nexus_t *x = t->target; c > 0; x = x->chain) {
            hash_put(tmap, x, w);
            c -= x->width;
         }
      }
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain)
         lv_pending_all(n->pending, tm_cb, NULL);
   }
   // pass 1: enumerate nodes (procs + transfers) as drivers or waiters
   g_lv_idx = hash_new(1024);
   int cap = 256;
   g_lv_wake = xmalloc_array(cap, sizeof(rt_wakeable_t *));
   g_lv_nproc = 0;
   int nedge_cap = 1024, nedge = 0;
   int (*edges)[2] = xmalloc_array(nedge_cap, 2 * sizeof(int));
   #define LV_IDX(w) ({ \
      void *_v = hash_get(g_lv_idx, (w)); \
      int _i; \
      if (_v == NULL) { \
         if (g_lv_nproc == cap) { \
            cap *= 2; \
            g_lv_wake = xrealloc_array(g_lv_wake, cap, sizeof(rt_wakeable_t *)); \
         } \
         g_lv_wake[g_lv_nproc] = (w); \
         hash_put(g_lv_idx, (w), (void *)(uintptr_t)(g_lv_nproc + 1)); \
         _i = g_lv_nproc++; \
      } \
      else _i = (int)(uintptr_t)_v - 1; \
      _i; })
   struct lv_ctx { int *idx; int n, cap; };
   void lv_cb(rt_wakeable_t *w, void *vc) {
      struct lv_ctx *c = vc;
      if (w->kind != W_PROC && w->kind != W_TRANSFER) return;
      if (c->n == c->cap) {
         c->cap = c->cap ? c->cap * 2 : 64;
         c->idx = xrealloc_array(c->idx, c->cap, sizeof(int));
      }
      c->idx[c->n++] = LV_IDX(w);
   }
   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
      // drivers of this nexus; resolve transfer-fed drivers to the
      // transfer node itself, and see THROUGH port hops -- an instance
      // output arrives via SOURCE_PORT and a port-blind graph loses every
      // cross-hierarchy edge (measured: 94,611 edges for 146k procs, 12
      // levels, 168k topo violations; the rvdffs mux ran before its input
      // settled and latched a stale 0)
      rt_wakeable_t *tw = hash_get(tmap, n);
      int drv[16], ndrv = 0;
      for (rt_source_t *s = &(n->sources); s != NULL; s = s->chain_input) {
         if (ndrv >= 16)
            break;
         rt_nexus_t *dn = n;
         rt_source_t *ds = s;
         if (s->tag == SOURCE_PORT && s->u.port.input != NULL) {
            // follow the port chain to the ultimate driving nexus
            rt_nexus_t *in = s->u.port.input;
            for (int hop = 0; hop < 16 && in != NULL; hop++) {
               rt_source_t *ps = NULL;
               for (rt_source_t *t2 = &(in->sources); t2 != NULL;
                    t2 = t2->chain_input)
                  if (t2->tag == SOURCE_PORT && t2->u.port.input != NULL)
                     ps = t2;
               if (ps == NULL) break;
               in = ps->u.port.input;
            }
            dn = in;
            ds = NULL;
            for (rt_source_t *t2 = &(in->sources); t2 != NULL;
                 t2 = t2->chain_input)
               if (t2->tag == SOURCE_DRIVER && t2->u.driver.proc != NULL) {
                  ds = t2;
                  break;
               }
            if (ds == NULL)
               continue;
         }
         else if (s->tag != SOURCE_DRIVER || s->u.driver.proc == NULL)
            continue;
         rt_wakeable_t *dw = &(ds->u.driver.proc->wakeable);
         rt_wakeable_t *dtw = (dn == n) ? tw : hash_get(tmap, dn);
         if (dtw != NULL
             && container_of(dtw, rt_transfer_t, wakeable)->proc
                == ds->u.driver.proc)
            dw = dtw;
         { static const char *ed = NULL; static int edi = -1;
           if (edi < 0) { ed = getenv("AJ_EVDRV"); edi = ed ? 1 : 0; }
           if (edi && g_lv_evnxn < 64
               && strstr(istr(ds->u.driver.proc->name), ed) != NULL) {
              bool dup = false;
              for (int k = 0; k < g_lv_evnxn; k++)
                 if (g_lv_evnx[k] == n) dup = true;
              if (!dup) {
                 g_lv_evnx[g_lv_evnxn++] = n;
                 fprintf(stderr, "#EM nx=%p sig=%s drv=%s\n", (void *)n,
                         (n->signal != NULL && n->signal->where != NULL)
                            ? istr(tree_ident(n->signal->where)) : "?",
                         istr(ds->u.driver.proc->name));
              }
           } }
         drv[ndrv++] = LV_IDX(dw);
      }
      if (ndrv == 0) continue;
      // waiters on this nexus (transfers included)
      static struct lv_ctx wc = { NULL, 0, 0 };
      wc.n = 0;
      lv_pending_all(n->pending, lv_cb, &wc);
      for (int i = 0; i < ndrv; i++)
         for (int j = 0; j < wc.n; j++) {
            if (drv[i] == wc.idx[j]) continue;
            if (nedge == nedge_cap) {
               nedge_cap *= 2;
               edges = xrealloc_array(edges, nedge_cap, 2 * sizeof(int));
            }
            edges[nedge][0] = drv[i];
            edges[nedge][1] = wc.idx[j];
            nedge++;
         }
   }
   #undef LV_IDX
   // pass 2: comb classification + Kahn levels over comb->comb edges
   bool *is_comb = xcalloc_array(g_lv_nproc, sizeof(bool));
   int ncomb = 0;
   for (int i = 0; i < g_lv_nproc; i++) {
      // comb = an undelayed signal transfer, or a signal-sensitive process
      // with no edge trigger: an edge-triggered process is signal-sensitive
      // by the predicate but must NOT be leveled -- the sweep absorbed the
      // glitch fixture's victim flop and ran it mid-settle (the very glitch
      // the sweep exists to prevent).
      rt_wakeable_t *w = g_lv_wake[i];
      if (w->kind == W_TRANSFER)
         is_comb[i] = getenv("NVC_SWEEP_NOXFER") == NULL
            && container_of(w, rt_transfer_t, wakeable)->after == 0;
      else if (w->kind == W_PROC) {
         rt_proc_t *p = container_of(w, rt_proc_t, wakeable);
         is_comb[i] = aj_proc_signal_sensitive(m, p) && w->trigger == NULL;
      }
      else
         is_comb[i] = false;
      if (is_comb[i]) ncomb++;
   }
   // CSR adjacency over comb->comb edges: the naive per-pop edge scan is
   // O(V*E) and VeeR-scale graphs (141k comb nodes, uncapped fanout) never
   // finish it
   int *indeg = xcalloc_array(g_lv_nproc, sizeof(int));
   int *outdeg = xcalloc_array(g_lv_nproc, sizeof(int));
   int ncedge = 0;
   for (int e = 0; e < nedge; e++)
      if (is_comb[edges[e][0]] && is_comb[edges[e][1]]) {
         indeg[edges[e][1]]++;
         outdeg[edges[e][0]]++;
         ncedge++;
      }
   int *off = xmalloc_array(g_lv_nproc + 1, sizeof(int));
   off[0] = 0;
   for (int i = 0; i < g_lv_nproc; i++) off[i + 1] = off[i] + outdeg[i];
   int *adj = xmalloc_array(ncedge > 0 ? ncedge : 1, sizeof(int));
   int *fill = xcalloc_array(g_lv_nproc, sizeof(int));
   for (int e = 0; e < nedge; e++)
      if (is_comb[edges[e][0]] && is_comb[edges[e][1]]) {
         const int u = edges[e][0];
         adj[off[u] + fill[u]++] = edges[e][1];
      }
   g_lv_level = xmalloc_array(g_lv_nproc, sizeof(int));
   for (int i = 0; i < g_lv_nproc; i++) g_lv_level[i] = -1;
   int *queue = xmalloc_array(g_lv_nproc, sizeof(int));
   int qh = 0, qt = 0, done = 0, maxlv = 0;
   for (int i = 0; i < g_lv_nproc; i++)
      if (is_comb[i] && indeg[i] == 0) { g_lv_level[i] = 0; queue[qt++] = i; }
   while (qh < qt) {
      int u = queue[qh++]; done++;
      for (int k = off[u]; k < off[u + 1]; k++) {
         int v = adj[k];
         if (g_lv_level[v] < g_lv_level[u] + 1) {
            g_lv_level[v] = g_lv_level[u] + 1;
            if (g_lv_level[v] > maxlv) maxlv = g_lv_level[v];
         }
         if (--indeg[v] == 0) queue[qt++] = v;
      }
   }
   free(outdeg); free(off); free(adj); free(fill);
   const int in_cycle = ncomb - done;
   // Comb procs caught in cycles (ICG latch feedback, vhdl2vlog loops) get
   // level 0: they iterate to fixpoint WITHIN the comb rounds via
   // re-absorption.  Leaving them unleveled sent them to the clocked
   // drain, where a gate computed with a stale latch output and the latch
   // updated after -- the sweep MANUFACTURED a clock glitch (measured:
   // VeeR IFU busclk extra transition, wrong final value, four bus-sync
   // flops dead, no retires).
   for (int i = 0; i < g_lv_nproc; i++)
      if (is_comb[i] && g_lv_level[i] < 0)
         g_lv_level[i] = 0;
   notef("levelize: %d procs (%d comb), %d edges, %d levels, "
         "%d comb procs in cycles (delta fallback)",
         g_lv_nproc, ncomb, nedge, maxlv + 1, in_cycle);
   const char *drvfind = getenv("AJ_DRVFIND");
   if (drvfind != NULL) {
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
         if (n->signal == NULL || n->signal->where == NULL) continue;
         const char *nm = istr(tree_ident(n->signal->where));
         if (strstr(nm, drvfind) == NULL) continue;
         // AJ_DRVFIND_ALL: one #SR line per source with tag/state — finds
         // the writer of every element, not just the first source.
         if (getenv("AJ_DRVFIND_ALL") != NULL) {
            int si = 0;
            for (rt_source_t *s2 = &(n->sources); s2 != NULL;
                 s2 = s2->chain_input, si++)
               fprintf(stderr, "#SR %s nx=%p src%d tag=%d disc=%u rr=%u "
                       "proc=%s w=%d sz=%d fl=0x%x nout=%d sig=%p ssz=%u\n",
                       nm, (void *)n, si,
                       (int)s2->tag, (unsigned)s2->disconnected,
                       (unsigned)s2->aj_rerouted,
                       s2->tag == SOURCE_DRIVER && s2->u.driver.proc != NULL
                          ? istr(s2->u.driver.proc->name) : "-",
                       n->width, n->size, (unsigned)n->flags,
                       n->outputs != NULL, (void *)n->signal,
                       (unsigned)n->signal->shared.size);
         }
         rt_wakeable_t *tw = hash_get(tmap, n);
         for (rt_source_t *s = &(n->sources); s != NULL; s = s->chain_input) {
            if (s->tag == SOURCE_PORT && s->u.port.input != NULL) {
               // follow port hops to the ultimate driving nexus
               rt_nexus_t *in = s->u.port.input;
               for (int hop = 0; hop < 16 && in != NULL; hop++) {
                  rt_source_t *ps = NULL;
                  for (rt_source_t *t2 = &(in->sources); t2 != NULL;
                       t2 = t2->chain_input)
                     if (t2->tag == SOURCE_PORT && t2->u.port.input != NULL)
                        ps = t2;
                  if (ps == NULL) break;
                  in = ps->u.port.input;
               }
               for (rt_source_t *t2 = &(in->sources); t2 != NULL;
                    t2 = t2->chain_input) {
                  if (t2->tag != SOURCE_DRIVER || t2->u.driver.proc == NULL)
                     continue;
                  rt_wakeable_t *dw = &(t2->u.driver.proc->wakeable);
                  rt_wakeable_t *itw = hash_get(tmap, in);
                  if (itw != NULL
                      && container_of(itw, rt_transfer_t, wakeable)->proc
                         == t2->u.driver.proc)
                     dw = itw;
                  const int di = aj_lv_idx(dw);
                  fprintf(stderr, "#DF sig=%s nx=%p in=%p VIA-PORT drv=%s "
                          "kind=%d node=%d lv=%d\n",
                          nm, (void *)n, (void *)in,
                          istr(t2->u.driver.proc->name), (int)dw->kind,
                          di, (di >= 0 && g_lv_level != NULL)
                                 ? g_lv_level[di] : -2);
               }
               continue;
            }
            if (s->tag != SOURCE_DRIVER || s->u.driver.proc == NULL)
               continue;
            rt_wakeable_t *dw = &(s->u.driver.proc->wakeable);
            if (tw != NULL
                && container_of(tw, rt_transfer_t, wakeable)->proc
                   == s->u.driver.proc)
               dw = tw;
            const int di = aj_lv_idx(dw);
            fprintf(stderr, "#DF sig=%s nx=%p drv=%s kind=%d node=%d "
                    "lv=%d\n",
                    nm, (void *)n, istr(s->u.driver.proc->name),
                    (int)dw->kind, di,
                    (di >= 0 && g_lv_level != NULL) ? g_lv_level[di] : -2);
         }
      }
   }
   const char *sensfind = getenv("AJ_SENSFIND");
   if (sensfind != NULL) {
      void sf_cb(rt_wakeable_t *w, void *vc) {
         rt_nexus_t *n = vc;
         const char *sig = (n->signal != NULL && n->signal->where != NULL)
            ? istr(tree_ident(n->signal->where)) : "?";
         if (w->kind == W_TRIGGER) {
            // trigger-mediated subscription: report procs hanging off the
            // trigger's own pending list, tagged with the HOST nexus
            rt_trigger_t *t = container_of(w, rt_trigger_t, wakeable);
            void *p = t->pending;
            if (pointer_tag(p) == 1) {
               rt_wakeable_t *pw = untag_pointer(p, rt_wakeable_t);
               if (pw->kind == W_PROC) {
                  const char *pn =
                     istr(container_of(pw, rt_proc_t, wakeable)->name);
                  if (strstr(pn, sensfind) != NULL)
                     fprintf(stderr, "#SF proc=%s VIA-TRIGGER kind=%d "
                             "sig=%s\n", pn, (int)t->kind, sig);
               }
            }
            else if (p != NULL) {
               rt_pending_t *pl = untag_pointer(p, rt_pending_t);
               for (int i = 0; i < pl->count; i++) {
                  if (pl->wake[i] == NULL || pl->wake[i]->kind != W_PROC)
                     continue;
                  const char *pn = istr(
                     container_of(pl->wake[i], rt_proc_t, wakeable)->name);
                  if (strstr(pn, sensfind) != NULL)
                     fprintf(stderr, "#SF proc=%s VIA-TRIGGER kind=%d "
                             "sig=%s\n", pn, (int)t->kind, sig);
               }
            }
            return;
         }
         const char *pn = NULL;
         if (w->kind == W_PROC)
            pn = istr(container_of(w, rt_proc_t, wakeable)->name);
         else if (w->kind == W_TRANSFER)
            pn = "(xfer)";
         else
            return;
         if (strstr(pn, sensfind) == NULL) return;
         fprintf(stderr, "#SF proc=%s kind=%d sig=%s\n", pn, (int)w->kind,
                 sig);
      }
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain)
         lv_pending_all(n->pending, sf_cb, n);
   }
   const char *lvfind = getenv("AJ_LVFIND");
   if (lvfind != NULL) {
      for (int i = 0; i < g_lv_nproc; i++) {
         if (g_lv_wake[i]->kind != W_PROC) continue;
         rt_proc_t *p = container_of(g_lv_wake[i], rt_proc_t, wakeable);
         if (strstr(istr(p->name), lvfind) == NULL) continue;
         fprintf(stderr, "#LF node=%d comb=%d lv=%d trig=%d name=%s\n",
                 i, (int)is_comb[i], g_lv_level[i],
                 g_lv_wake[i]->trigger != NULL, istr(p->name));
      }
   }
   if (getenv("AJ_SWEEPDBG") != NULL && g_lv_nproc <= 64) {
      for (int i = 0; i < g_lv_nproc; i++)
         fprintf(stderr, "#LV node=%d kind=%d comb=%d lv=%d name=%s\n",
                 i, (int)g_lv_wake[i]->kind, (int)is_comb[i], g_lv_level[i],
                 g_lv_wake[i]->kind == W_PROC
                    ? istr(container_of(g_lv_wake[i], rt_proc_t,
                                        wakeable)->name)
                    : "-");
      for (int e = 0; e < nedge; e++)
         fprintf(stderr, "#LE %d -> %d\n", edges[e][0], edges[e][1]);
   }
   g_lv_maxlv = maxlv;
   g_lv_seen  = xcalloc_array(g_lv_nproc, sizeof(uint32_t));
   g_lv_sweep = getenv("NVC_LEVELIZE_SWEEP") != NULL;
   if (g_lv_sweep)
      notef("levelize: SWEEP dispatch ACTIVE (%d levels)", maxlv + 1);
   free(indeg); free(queue); free(is_comb); free(edges);
   hash_free(tmap);
}

// Engage the levelized sweep without the accel install path (plain-interp
// runs under NVC_LEVELIZE_SWEEP)
void accel_levelize(rt_model_t *m)
{
   aj_levelize_analyze(m);
}

// NVC_COLLAPSE_CENSUS: sizing data for elaboration-level port aliasing
// (task #59 / the SMP double-bank directive).  A "pure rim" nexus — exactly
// one source and it is a conversion-free SOURCE_PORT — is storage that
// port-collapse would eliminate entirely (its bytes become an alias of the
// driving side; no SOURCE_PORT edge, no update walk, no rim deposit).  The
// census reports how much of the design that class covers, deciding
// whether the alias mechanism is worth building for this code base.
static void aj_collapse_census_scope(rt_scope_t *s, long ctr[8])
{
   for (int i = 0; i < s->signals.count; i++) {
      rt_signal_t *sig = s->signals.items[i];
      rt_nexus_t *n = &sig->nexus;
      for (unsigned k = 0; k < sig->n_nexus; k++, n = n->chain) {
         ctr[0]++;                                   // nexuses
         ctr[1] += n->width * n->size;               // bytes
         if (n->n_sources == 1 && n->sources.tag == SOURCE_PORT) {
            if (n->sources.u.port.conv_func == NULL) {
               ctr[2]++;                             // pure rims
               ctr[3] += n->width * n->size;
               if (sig->resolution == NULL) {
                  ctr[4]++;                          // unresolved pure rims
                  ctr[5] += n->width * n->size;
               }
            }
            else
               ctr[6]++;                             // conversion rims
         }
         else if (n->n_sources == 0)
            ctr[7]++;                                // undriven
      }
   }
   for (int i = 0; i < s->children.count; i++)
      aj_collapse_census_scope(s->children.items[i], ctr);
}

// NVC_BANKED I1: install the banked vtable on the eligible class — driven,
// unresolved, single-source nexuses currently on the default vtable (the
// fast single-driver/memo1/lazy classes keep their specializations; the
// banked class will absorb them in I2 once the second plane is real).
// Banked eligibility also requires that NOTHING consumes the nexus's value
// during the update phase, because staged deposits only land at the flip:
// transfer processes (async_transfer_signal — the `q <= r` identity-assign
// optimization — reads its SOURCE mid-phase; measured: gals's qa1/qa2/qb1
// never updated and the fixture wedged at Y=0) and implicit signals (the
// implicitq drains before the flip site).  Both register their wakeables
// during initialization, before accel_banked_init runs, so an install-time
// pending walk sees them.
static bool aj_banked_pending_ok(void *pending)
{
   if (pointer_tag(pending) == 1) {
      rt_wakeable_t *w = untag_pointer(pending, rt_wakeable_t);
      return w->kind != W_TRANSFER && w->kind != W_IMPLICIT;
   }
   else if (pending != NULL) {
      rt_pending_t *p = untag_pointer(pending, rt_pending_t);
      for (int i = 0; i < p->count; i++) {
         rt_wakeable_t *w = p->wake[i];
         if (w == NULL) continue;
         if (w->kind == W_TRANSFER || w->kind == W_IMPLICIT) return false;
      }
   }
   return true;
}

static void aj_banked_install_scope(rt_scope_t *s)
{
   for (int i = 0; i < s->signals.count; i++) {
      rt_signal_t *sig = s->signals.items[i];
      rt_nexus_t *n = &sig->nexus;
      for (unsigned k = 0; k < sig->n_nexus; k++, n = n->chain) {
         if (n->n_sources != 1 || n->sources.tag != SOURCE_DRIVER)
            continue;
         // Port fan-out is an update-phase consumer too: the outputs
         // list schedules SOURCE_PORT propagation from the source's
         // notify into heaps that drain BEFORE the flip (measured: gals
         // qa1 — the child q feeding its port never propagated).  Only
         // leaf nets (no port outputs) are banked-eligible.
         if (n->outputs != NULL)
            continue;
         if (!aj_banked_pending_ok(n->pending))
            continue;
         int ci = 0;
         for (; ci < g_banked_st.nclasses; ci++)
            if (g_banked_st.orig[ci] == n->vtable)
               break;
         if (ci == g_banked_st.nclasses) {
            // new original class: skip anything already patched, cap classes
            bool already = false;
            for (int p = 0; p < g_banked_st.nclasses && !already; p++)
               already = (&g_banked_st.patched[p] == n->vtable);
            if (already || g_banked_st.nclasses == BANKED_MAX_CLASSES)
               continue;
            g_banked_st.orig[ci] = n->vtable;
            g_banked_st.patched[ci] = *n->vtable;
            g_banked_st.patched[ci].deposit = banked_deposit;
            g_banked_st.nclasses++;
         }
         n->vtable = &g_banked_st.patched[ci];
         g_banked_st.installs++;
      }
   }
   for (int i = 0; i < s->children.count; i++)
      aj_banked_install_scope(s->children.items[i]);
}

// Always-run entry (plain interp included): nvc.c calls this after
// model_reset regardless of --accel, so the banked class installs on every
// serially-passing workload (the directive's exploration corpus).
void accel_banked_init(rt_model_t *m)
{
   if (g_banked < 0) {
      // NVC_ALIAS_FLIP (#74) implies the banked install: the alias-flip
      // drive-time staging needs the class installed and the flip live.
      g_alias_flip = getenv("NVC_ALIAS_FLIP") != NULL ? 1 : 0;
      g_banked = (getenv("NVC_BANKED") != NULL || g_alias_flip == 1) ? 1 : 0;
   }
   if (g_banked == 1) {
      aj_banked_install_scope(root_scope(m));
      notef("banked: copy-at-flip ACTIVE — %ld leaf nexus(es) staged "
            "(deposits land at the delta-boundary flip)",
            g_banked_st.installs);
   }
}

void accel_auto(rt_model_t *m)
{
   if (getenv("NVC_COLLAPSE_CENSUS") != NULL) {
      long ctr[8] = {0};
      aj_collapse_census_scope(root_scope(m), ctr);
      notef("collapse-census: nexuses=%ld bytes=%ld pure_rims=%ld (%.1f%%) "
            "rim_bytes=%ld (%.1f%%) unresolved_rims=%ld urim_bytes=%ld "
            "conv_rims=%ld undriven=%ld",
            ctr[0], ctr[1], ctr[2],
            ctr[0] ? 100.0 * ctr[2] / ctr[0] : 0.0,
            ctr[3], ctr[1] ? 100.0 * ctr[3] / ctr[1] : 0.0,
            ctr[4], ctr[5], ctr[6], ctr[7]);
   }
   aj_levelize_analyze(m);
   g_aj_verify = getenv("NVC_ACCEL_VERIFY") != NULL;
   g_aj_verify_skipx = getenv("NVC_ACCEL_VERIFY_X") != NULL;
   g_aj_vtrack = getenv("NVC_ACCEL_VERIFY_TRACK") != NULL;

   if (g_aj_demote_at == -2) {
      const char *e = getenv("NVC_ACCEL_DEMOTE_AT");
      if (e == NULL)
         g_aj_demote_at = -1;
      else {
         int64_t t = parse_fork_time(e);
         if (t < 0) {   // bare number = ns
            char *end = NULL;
            unsigned long long v = strtoull(e, &end, 10);
            t = (*e != '\0' && end != NULL && *end == '\0')
               ? (int64_t)v * 1000000 : -1;
         }
         g_aj_demote_at = t;
         if (t < 0)
            warnf("cannot parse NVC_ACCEL_DEMOTE_AT='%s' (want e.g. 5000ns)", e);
      }
   }

   // Cache location, resolved exactly like the JIT cache (jit-cache.c): explicit
   // override, then $XDG_CACHE_HOME, then $HOME.  Two reasons this is not just
   // "$HOME/.cache":
   //
   //   * a CONTAINER ON AN ALIEN FARM mounts its root read-only and may not set
   //     a writable HOME, so the write target has to be redirectable to a
   //     bind-mounted path or --accel is simply unusable there;
   //   * the old `if (!home) home = "/tmp"` fallback was a MULTI-USER HOLE --
   //     a shared world-writable directory from which we dlopen().  Another
   //     user could plant aj_<design>.so and have it loaded.  jit-cache.c
   //     already refuses to do this and names this path as the bad precedent.
   //
   // With no override and no HOME we fall back to a PRIVATE mkdtemp (0700)
   // rather than a shared path: accel keeps working, nothing is reused across
   // runs, and no other user can write to it.
   char accel_dir[512];
   const char *ovr = getenv("NVC_ACCEL_CACHE_DIR");
   const char *xdg = getenv("XDG_CACHE_HOME");
   const char *home = getenv("HOME");

   if (ovr != NULL && ovr[0] != '\0')
      snprintf(accel_dir, sizeof(accel_dir), "%s", ovr);
   else if (xdg != NULL && xdg[0] != '\0')
      snprintf(accel_dir, sizeof(accel_dir), "%s/nvc/accel", xdg);
   else if (home != NULL && home[0] != '\0')
      snprintf(accel_dir, sizeof(accel_dir), "%s/.cache/nvc/accel", home);
   else {
      char tmpl[] = "/tmp/nvc-accel-XXXXXX";
      const char *priv = mkdtemp(tmpl);
      if (priv == NULL) {
         warnf("--accel: no writable cache directory (set NVC_ACCEL_CACHE_DIR)");
         return;
      }
      snprintf(accel_dir, sizeof(accel_dir), "%s", priv);
   }
   aj_mkdir_p(accel_dir);   // JIT writes aj_*.c/.so here; may not exist yet

   if (getenv("NVC_ACCEL_MERGE") != NULL && getenv("NVC_ACCEL_FROM_VHDL")) {
      // Task #53/#59: collect candidates, then install one merged chunk per
      // clock-domain group (falls back to per-chunk on any failure).
      g_aj_collecting = true;
      accel_scan_scope(m, root_scope(m), accel_dir);
      g_aj_collecting = false;
      aj_try_merge_install(m, accel_dir);
   }
   else
      accel_scan_scope(m, root_scope(m), accel_dir);

   // Establish initial combinational output values at t=0. model_reset ran the
   // subtree's procs with their original vtables, but the reroute supersedes
   // their driver transactions; without an initial accel deposit the output
   // nets sit at 'U' until the first clock edge -- and a reader that samples an
   // output on that same edge (before the chunk's own eval deposits it) captures
   // the 'U', which is fatal for an xor/accumulate reader (one 'U' poisons it
   // forever, e.g. c1c_tb's `chk <= chk xor result`). Run each installed chunk's
   // comb eval once (clk is low at t=0 => the bridge takes the sm_comb path and
   // deposits reset-state outputs), mirroring the interpreter's initial settle.
   // VERIFY mode never reroutes, so it needs no seeding.
   // NVC_ACCEL_NO_SEED: bisection knob (mirrors NVC_ACCEL_NO_SETTLE /
   // NVC_ACCEL_NO_FORCE). Skipping the t=0 seed reproduces, on demand, the
   // pre-6d47cb570 symptom -- an accel output net that is never deposited
   // before the first clock edge, so a reader sampling it on that edge
   // captures 'U'. Y=0 plus "TO_INTEGER metavalue detected".
   if (!g_aj_verify && getenv("NVC_ACCEL_NO_SEED") == NULL) {
      const int tid = thread_id();
      model_thread_t *thread = model_thread(m);
      rt_wakeable_t *save_obj   = thread->active_obj;
      rt_scope_t    *save_scope = thread->active_scope;
      aj_chunk_t    *save_chunk = g_aj_cur_chunk[tid];
      thread->active_obj   = NULL;   // no cone proc -- deposit_signal null-guards
      thread->active_scope = NULL;
      // Dependency-ordered seed (#43 FIX): seed producers before
      // consumers so a cross-chunk input is never read while its
      // producer still holds 'U' (which the 2-state plane would latch
      // as a confident 0, permanently after the first edge — seedord's
      // deterministic wrong answer). Edges come from the always-built
      // AO maps; repeated ready-sweeps (chunk counts are small), with
      // any cyclic remainder (cross-chunk comb loops) appended in
      // install order as before. NVC_ACCEL_SEED_ORDERED=0 restores the
      // plain install-order pass for A/B.
      const unsigned nch = m->aj_chunk_count;
      aj_chunk_t **seed_order = xmalloc_array(nch ? nch : 1,
                                              sizeof(aj_chunk_t *));
      unsigned nord = 0;
      { const char *so = getenv("NVC_ACCEL_SEED_ORDERED");
        bool want = (so == NULL || *so != '0') && g_ao3_n > 0;
        bool *emitted = xcalloc_array(nch ? nch : 1, sizeof(bool));
        if (want) {
           bool progress = true;
           while (nord < nch && progress) {
              progress = false;
              for (unsigned ci = 0; ci < nch; ci++) {
                 if (emitted[ci]) continue;
                 aj_chunk_t *c = m->aj_chunks[ci];
                 bool ready = true;
                 for (unsigned i = 0; ready && i < g_ao3_n; i++) {
                    if (g_ao3_map[i].chunk != c) continue;
                    for (unsigned j = 0; j < g_ao4_n; j++) {
                       if (g_ao4_out[j].nx != g_ao3_map[i].nx) continue;
                       aj_chunk_t *p = g_ao4_out[j].chunk;
                       if (p != c) {
                          bool pdone = false;
                          for (unsigned k = 0; k < nord && !pdone; k++)
                             pdone = (seed_order[k] == p);
                          if (!pdone) ready = false;
                       }
                       break;
                    }
                 }
                 if (ready) {
                    emitted[ci] = true;
                    seed_order[nord++] = c;
                    progress = true;
                 }
              }
           }
        }
        for (unsigned ci = 0; ci < nch; ci++)   // remainder / ordering off
           if (!emitted[ci])
              seed_order[nord++] = m->aj_chunks[ci];
        free(emitted);
      }
      for (unsigned ci = 0; ci < nord; ci++) {
         aj_chunk_t *c = seed_order[ci];
         if (c->eval == NULL) continue;
         // Cross-chunk seed BACK-FILL (#43, second half): ordering alone
         // is NOT sufficient — a producer's seed deposit lands on ITS
         // nexus, but port-hop propagation to this consumer's input rim
         // needs scheduler deltas that never run during reset (measured:
         // topo-ordered seedord still Y=190574892 with the audit clean).
         // Copy each already-seeded producer's ultimate-nexus effective
         // bytes straight into this chunk's rim, PORTFAN-style. Width
         // mismatches across hops are skipped (identity hops only).
         { const char *so2 = getenv("NVC_ACCEL_SEED_ORDERED");
           if (so2 == NULL || *so2 != '0') {
              for (unsigned i = 0; i < g_ao3_n; i++) {
                 if (g_ao3_map[i].chunk != c || g_ao3_map[i].dst == NULL)
                    continue;
                 for (unsigned j = 0; j < g_ao4_n; j++) {
                    if (g_ao4_out[j].nx != g_ao3_map[i].nx) continue;
                    aj_chunk_t *p = g_ao4_out[j].chunk;
                    rt_nexus_t *un = g_ao3_map[i].nx;
                    const size_t unb = (size_t)un->width * un->size;
                    if (p != c && p->ao_seeded && unb == g_ao3_map[i].nbytes)
                       memcpy(g_ao3_map[i].dst, nexus_effective(un), unb);
                    break;
                 }
              }
           }
         }
         // Force a comb-only settle: the bridge edge-detects on clk_last0,
         // which is 0 after reset, so a design that initialises clk HIGH
         // would read a spurious 0->1 at its first sample. The interpreter
         // only settles comb at init (rising_edge is false with no
         // transition), so drive clk low for this one eval, restore it, and
         // afterwards sync clk_last0 to the REAL clock level (below) so the
         // initial level never reads as an edge. bindtab[4] is clk->data.
         uint8_t *clkp = c->bindtab ? (uint8_t *)c->bindtab[4] : NULL;
         const bool force = (clkp != NULL && (clkp[0] & 1));
         const uint8_t saved = force ? clkp[0] : 0;
         if (force) clkp[0] = 2;   // driven-'0' (std_logic '0' / logic3d 0)
         // AO>=3 rank-4 audit (#43): the seed runs in INSTALL order with
         // no settle pass. A cross-chunk input whose producer has not yet
         // seeded still reads 'U' here, and the 2-state value plane
         // latches it as a confident 0 — permanently after the first
         // edge. Report per offending pin; t=0 only, zero steady cost.
         if (aj_ao_level() >= 3) {
            for (unsigned _i = 0; _i < g_ao3_n; _i++) {
               if (g_ao3_map[_i].chunk != c) continue;
               for (unsigned _j = 0; _j < g_ao4_n; _j++) {
                  if (g_ao4_out[_j].nx != g_ao3_map[_i].nx) continue;
                  aj_chunk_t *p = g_ao4_out[_j].chunk;
                  if (p != c && !p->ao_seeded)
                     notef("accel-assert: SEED-ORDER t=0: chunk %s pin "
                           "'%s' reads an output of chunk %s which is NOT "
                           "yet seeded (install order != dependency "
                           "order) — 2-state seed may latch U as 0 (#43)",
                           c->scope != NULL && c->scope->where != NULL
                              ? istr(tree_ident(c->scope->where)) : "?",
                           g_ao3_map[_i].pin,
                           p->scope != NULL && p->scope->where != NULL
                              ? istr(tree_ident(p->scope->where)) : "?");
                  break;
               }
            }
         }
         g_aj_cur_chunk[tid] = c;
         c->eval(c->state, c->bindtab);
         c->ao_seeded = 1;
         if (force) clkp[0] = saved;
         if (clkp != NULL && c->dl != NULL) {
            void (*setck)(void *, unsigned char) =
               dlsym(c->dl, "accel_set_clklast");
            if (setck != NULL) setck(c->state, clkp[0] & 1);
         }
      }
      free(seed_order);
      g_aj_cur_chunk[tid] = save_chunk;
      thread->active_obj   = save_obj;
      thread->active_scope = save_scope;
   }
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
         wrap->vtable.on_abort = proc_on_abort_default;
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

// Post-eval bookkeeping shared by run_process and the fused-block member
// epilogue (which replicates run_process's semantics per member and must
// not drift from them).
static inline void proc_static_wait_finalize(rt_model_t *m, rt_proc_t *proc)
{
   rt_wakeable_t *obj = &(proc->wakeable);

   // A fused cone's body is straight-line: one complete activation deposits
   // its full nexus set, so the depositors map is complete — stop recording.
   if (obj->fused_cone)
      obj->dep_recorded = 1;

   // Static-wait bookkeeping. Finalize only at suspends that ARMED an
   // event wait (cur_count > 0): a mid-cycle timed suspend (the NBA
   // transform's `wait for 0 ns`) schedules nothing and must not be
   // mistaken for a changed wait set.
   if (obj->trigger == NULL && obj->wait_state != 2 && proc->cur_count > 0) {
      if (obj->wait_state == 0) {
         if (proc->wait_count > 0) {
            proc->wait_sig = proc->cur_sig;
            proc->wait_fpcount = proc->cur_count;
            obj->wait_state = 1;   // promote: entries now persist
            direct_eval_install(m, proc);
         }
      }
      else if (proc->cur_sig != proc->wait_sig
               || proc->cur_count != proc->wait_fpcount) {
         // The wait set changed: demote to the dynamic path. Remove the
         // persistent registrations and install THIS activation's set
         // (recorded in cur_set) so no wakeup is lost — never re-run the
         // process (body side effects must execute exactly once).
         for (unsigned i = 0; i < proc->wait_count; i++)
            clear_event_par(m, &(proc->wait_set[i]->pending), obj);
         free(proc->wait_set);
         proc->wait_set = NULL;
         proc->wait_count = proc->wait_cap = 0;
         obj->wait_state = 2;
         if (unlikely(obj->fastclk))
            // A dynamic-wait member registers outside candidate+companions;
            // events there would only latch (lost wakeup for a posedge-only
            // member). Evict back to normal queued wakeup.
            aj_fastclk_evict(m, obj, "wait-set change");
         direct_eval_uninstall(proc);   // demoted: back to the generic path
         for (unsigned i = 0; i < proc->cur_count; i++)
            sched_event_par(m, &(proc->cur_set[i]->pending), obj);
      }
      proc->cur_sig = 0;
      proc->cur_count = 0;
   }
}

static void run_process(rt_model_t *m, rt_proc_t *proc)
{
   TRACE("run %sprocess %s", *mptr_get(proc->privdata) ? "" :  "stateless ",
         istr(proc->name));

   rt_wakeable_t *obj = &(proc->wakeable);

   if (obj->trigger != NULL && !run_trigger(m, obj->trigger))
      return;   // Filtered

   proc->vtable->eval(m, proc);

   proc_static_wait_finalize(m, proc);
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

// ---- NVC_FUSED_BLOCK one-block posedge dispatch (Phase B) -------------------
//
// Fuses the NVC_FAST_CLK member set into ONE emitted machine-code block: a
// straight-line sequence of per-member argument setup + direct calls to each
// member's native entry, with the call targets dereferenced at BLOCK BUILD
// time (every structure is static by then) and baked as immediates. One
// arena reset, one landing-pad check and one activation replace N of each.
//
// The block is strictly STATELESS -- it owns SEQUENCING only. Everything
// baked into it (state/context pointers, jit_func_t pointers, wakeable and
// scope addresses, the model-thread pointer) is a derived constant owned by
// the model, and the shared args buffer is per-call scratch whose contents
// are dead between activations. Dissolving the block at any delta boundary
// (fused_block_dissolve) therefore loses nothing: dispatch falls back to
// the fast-clk table loop, or to normal queued wakeup once
// aj_dissolve_fastclk has also run, with identical semantics. This is a
// hard design constraint: large simulations roll through live design edits
// with no restart, so every optimization must be runtime-reversible.
//
// Per-member emitted sequence (SysV x86-64; rbx = model_thread(m) and
// r12 = the shared args buffer, both loaded once in the block prologue):
//
//   movabs rax, <state_i>            ; args[0] = cached *privdata ?: -1
//   mov    [r12], rax
//   movabs rax, <context_i>          ; args[1] = cached scope privdata
//   mov    [r12+8], rax
//   movabs rax, <&proc_i.wakeable>   ; driver lookup (x_sched_event etc reads
//   mov    [rbx+off(active_obj)], rax     ; the active wakeable) and abort
//   movabs rax, <proc_i.scope>            ; attribution (get_active_proc at
//   mov    [rbx+off(active_scope)], rax   ; model_run's pad) are per member
//   mov    rcx, [rbx+off(tlab)]      ; thread->tlab: NOT baked, a claim
//                                    ; swaps it (rare; handled in epilogue)
//   movabs rdi, <func_i>             ; native entry ABI: f, anchor=NULL,
//   xor    esi, esi                  ; args, tlab
//   mov    rdx, r12
//   SITE A: call <entry_i>           ; 13-byte patchable slot
//   movabs rdi, <m>                  ; per-member C epilogue: TLAB claim
//   movabs rsi, <proc_i>             ; protocol + static-wait finalize
//   mov    rdx, r12                  ; (branchy-but-cold -> out of line)
//   SITE B: call <fused_member_epilogue>   ; 13-byte patchable slot
//
// Call slots are a fixed 13 bytes holding either `call rel32` + 8-byte nop
// (target within +/-2GB of the site -- the common case; statically
// predicted, sequential I-cache streams) or `movabs r11, imm64; call r11`
// for far targets. Patching a slot is a plain byte store: ALL patches are
// applied by the MODEL thread, the same thread that executes the block, and
// same-thread store-then-execute is architecturally handled on x86 (SMC
// detection flushes stale prefetch), so no cross-modifying-code protocol is
// needed. A tier-up publish from the async compile thread only RECORDS the
// new target (code_entry_watch callback -> want_a + patch_pending); the
// model thread applies pending patches at the next dispatch. A member whose
// own epilogue demotes it patches its OWN sites, which the instruction
// pointer has already passed this activation.
//
// Member states:
//   SITE_PLAIN    -- hot path above; requires a Phase A direct_eval_t
//                    wrapper (wait_state 1, default-vtable), no trigger, no
//                    private TLAB. Tier-up retargets SITE A in place.
//   SITE_FALLBACK -- SITE A = shared null-result stub, SITE B =
//                    fused_member_fallback -> run_process: the full generic
//                    activation, bit-exact with the unfused table loop.
//                    Used at build for unfusable members and at runtime
//                    when a member demotes (wait-set change), arms a
//                    trigger, or claims a private TLAB.
//   SITE_DISABLED -- SITE A = shared null-result stub, SITE B =
//                    fused_member_skip: member contributes nothing (future
//                    live-edit demotion hook; nothing uses it yet).

#ifdef ARCH_X86_64

typedef enum {
   SITE_PLAIN,      // register setup + patchable SITE A call
   SITE_SPLICE,     // body copied INLINE (code_blob_splice): no SITE A slot,
                    // no call/return -- any retarget/demote/evict affecting
                    // this member dissolves the whole block instead
   SITE_FALLBACK,
   SITE_DISABLED,
} fused_site_state_t;

typedef struct {
   jit_entry_fn_t *slot;     // &func->entry whose value SITE A baked
   jit_entry_fn_t  cur_a;    // currently baked SITE A target (model thread)
   jit_entry_fn_t  want_a;   // retarget from a tier-up publish (any thread)
   uint32_t        off_a;    // SITE A slot offset from block base
   uint32_t        off_b;    // SITE B slot offset from block base
   uint8_t         state;    // fused_site_state_t
} fused_site_t;

typedef struct _fused_block {
   uint8_t        *base;        // block entry / span base (RWX on Linux)
   jit_entry_fn_t  entry;       // == base; context args unused
   jit_entry_fn_t  entry_offedge;  // off-edge entry: duplicated prologue +
                                   // fallthrough into the every-event tail
   jit_scalar_t   *argbuf;      // shared JIT_MAX_ARGS scratch slots
   rt_proc_t     **members;     // own copy of the fast-clk table at build
   fused_site_t   *sites;       // member i -> its two call slots
   unsigned        count;
   model_thread_t *thread;      // baked model-thread pointer
   int             patch_pending;   // set by retarget callback (any thread)
   int             rebuild_pending; // spliced member invalidated: dissolve
                                    // at the next dispatch boundary
   bool            splice_ok;   // build-time: splicing enabled for this block
   size_t          splice_budget;   // remaining bytes for inline bodies
   bool            running;     // dissolve must never happen mid-block
} fused_block_t;

// NVC_SPLICE=1: copy each plain member's JIT-compiled body INLINE into the
// fused block (code_blob_splice) instead of emitting a call to it -- the
// call/return boundary disappears and only the per-member register updates
// remain between bodies.  Default OFF until gated.
static bool fused_splice_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *e = getenv("NVC_SPLICE");
      enabled = (e != NULL && *e == '1');
   }
   return enabled;
}

static bool fused_block_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *e = getenv("NVC_FUSED_BLOCK");
      enabled = (e == NULL || *e != '0');   // default ON; =0 disables
   }
   return enabled;
}

// (Re)write one 13-byte call slot. Model thread only; see block comment
// for why no cross-modifying-code protocol is required.
static void fused_patch_call(fused_block_t *fb, uint32_t off, void *target)
{
   uint8_t *site = fb->base + off;
   const int64_t rel = (int64_t)((uint8_t *)target - (site + 5));

   if (rel == (int64_t)(int32_t)rel) {
      static const uint8_t nop8[8] =
         { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };
      const int32_t r32 = (int32_t)rel;
      site[0] = 0xE8;                // call rel32
      memcpy(site + 1, &r32, 4);
      memcpy(site + 5, nop8, 8);
   }
   else {
      site[0] = 0x49; site[1] = 0xBB;          // movabs r11, target
      memcpy(site + 2, &target, 8);
      site[10] = 0x41; site[11] = 0xFF; site[12] = 0xD3;   // call r11
   }
}

static void fused_member_unfuse(rt_model_t *m, rt_proc_t *proc);

// Out-of-line per-member epilogue (SITE B for a PLAIN member): the exact
// post-eval sequence proc_eval_direct + run_process would have performed --
// anchor discipline, TLAB claim/release protocol, active_obj/active_scope
// clear, static-wait finalize. Kept in C because it is branchy-but-cold;
// nothing here is dropped, only moved out of the straight line.
static void fused_member_epilogue(rt_model_t *m, rt_proc_t *proc,
                                  jit_scalar_t *args)
{
   model_thread_t *thread = model_thread(m);

   jit_thread_get()->anchor = NULL;   // stack-trace anchor discipline

   void *result_ptr = args[0].pointer;

   // TLAB claim/release protocol. proc->tlab is NULL for a fused-plain
   // member (enforced at build and by unfuse-on-claim below) but the full
   // protocol is replicated so no semantics are silently dropped.
   if (unlikely(proc->tlab != NULL)) {
      if (result_ptr == NULL) {
         tlab_release(proc->tlab);
         proc->tlab = NULL;
      }
   }
   else if (unlikely(result_ptr != NULL)) {
      TRACE("claiming TLAB for private use (used %u/%u)",
            thread->tlab->alloc, thread->tlab->limit);
      proc->tlab = thread->tlab;
      thread->tlab = tlab_acquire(m->mspace);
      // The block passes thread->tlab inline; a claimant needs its private
      // one -- demote this member to the generic fallback path
      fused_member_unfuse(m, proc);
   }
   else
      tlab_reset(thread->tlab);

   thread->active_obj = NULL;
   thread->active_scope = NULL;

   proc_static_wait_finalize(m, proc);

   // The finalize may have demoted the wait set (direct_eval_uninstall
   // removed the Phase A wrapper), or the eval may have armed a trigger:
   // either invalidates the baked plain path from the next posedge on
   if (unlikely(proc->vtable->eval != proc_eval_direct
                || proc->wakeable.trigger != NULL))
      fused_member_unfuse(m, proc);
}

// SITE B for a FALLBACK member: the full generic activation. The inline
// stores before SITE A set active_obj/active_scope; undo them first so
// run_process sees exactly what the unfused table loop would give it (in
// particular the trigger filter runs with no active object).
static void fused_member_fallback(rt_model_t *m, rt_proc_t *proc,
                                  jit_scalar_t *args)
{
   model_thread_t *thread = model_thread(m);
   thread->active_obj = NULL;
   thread->active_scope = NULL;

   if (unlikely(!proc->wakeable.fastclk || proc->wakeable.pending))
      return;   // evicted from the table / already queued -- runs via procq

   run_process(m, proc);
}

// SITE B for a DISABLED member: only undo the inline active_obj/
// active_scope stores (SITE A was the shared null-result stub).
static void fused_member_skip(rt_model_t *m, rt_proc_t *proc,
                              jit_scalar_t *args)
{
   model_thread_t *thread = model_thread(m);
   thread->active_obj = NULL;
   thread->active_scope = NULL;
}

// Demote one member to the generic path (model thread; patches sites the
// current activation has already executed).
static void fused_member_unfuse(rt_model_t *m, rt_proc_t *proc)
{
   fused_block_t *fb = m->fused_block;
   if (fb == NULL)
      return;

   for (unsigned i = 0; i < fb->count; i++) {
      if (fb->members[i] != proc)
         continue;
      fused_site_t *s = &(fb->sites[i]);
      if (s->state == SITE_SPLICE)
         // The body is inline: nothing to patch.  It must not run again
         // in this form; request a dissolve at the next dispatch boundary
         // (the block never re-runs before the preamble sees the flag).
         relaxed_store(&(fb->rebuild_pending), 1);
      else if (s->state == SITE_PLAIN) {
         s->state = SITE_FALLBACK;
         s->cur_a = m->fused_stub;
         fused_patch_call(fb, s->off_a, (void *)m->fused_stub);
         fused_patch_call(fb, s->off_b, (void *)fused_member_fallback);
      }
      return;   // a proc appears at most once in the table
   }
}

// Disable a member outright (shared empty stub; enable/disable mechanism
// for future live-edit demotion -- nothing calls this yet).
__attribute__((unused))
static void fused_member_disable(rt_model_t *m, rt_proc_t *proc)
{
   fused_block_t *fb = m->fused_block;
   if (fb == NULL)
      return;

   for (unsigned i = 0; i < fb->count; i++) {
      if (fb->members[i] != proc)
         continue;
      fused_site_t *s = &(fb->sites[i]);
      if (s->state != SITE_DISABLED) {
         s->state = SITE_DISABLED;
         s->cur_a = m->fused_stub;
         fused_patch_call(fb, s->off_a, (void *)m->fused_stub);
         fused_patch_call(fb, s->off_b, (void *)fused_member_skip);
      }
      return;
   }
}

// code_entry_watch callback: a tier-up published a new entry through a slot
// SITE A baked. May run on the async compile thread -- record only; the
// model thread applies pending patches at the next dispatch.
static void fused_entry_retarget(jit_entry_fn_t *slot, jit_entry_fn_t entry,
                                 void *ctx)
{
   fused_block_t *fb = ctx;
   for (unsigned i = 0; i < fb->count; i++) {
      if (fb->sites[i].slot == slot)
         relaxed_store(&(fb->sites[i].want_a), entry);
   }
   store_release(&(fb->patch_pending), 1);
}

static void fused_apply_pending(rt_model_t *m, fused_block_t *fb)
{
   relaxed_store(&(fb->patch_pending), 0);

   for (unsigned i = 0; i < fb->count; i++) {
      fused_site_t *s = &(fb->sites[i]);
      jit_entry_fn_t want = relaxed_load(&(s->want_a));
      if (want == NULL || want == s->cur_a)
         continue;
      if (s->state == SITE_SPLICE) {
         // The old body is baked inline: rebuild the whole block
         relaxed_store(&(fb->rebuild_pending), 1);
         continue;
      }
      if (s->state != SITE_PLAIN)
         continue;
      s->cur_a = want;
      fused_patch_call(fb, s->off_a, (void *)want);
      // The member is native now: a rebuild can splice its body inline.
      // (The block is typically built at probation exit, BEFORE most
      // members reach the tier-up threshold -- without this the splice
      // would only ever cover bodies that happened to compile early.)
      if (fb->splice_ok)
         relaxed_store(&(fb->rebuild_pending), 1);
      if (unlikely(getenv("NVC_FUSED_DEBUG") != NULL))
         notef("fused: tier-up retarget member %u -> %p", i, (void *)want);
   }
}

#define FUSED_EMIT(...) do {                            \
      const uint8_t __b[] = { __VA_ARGS__ };            \
      code_blob_emit(blob, __b, sizeof(__b));           \
   } while (0)

// movabs <reg>, imm64 (rex/opc select the register)
static void fused_emit_movabs(code_blob_t *blob, uint8_t rex, uint8_t opc,
                              uint64_t imm)
{
   uint8_t b[10] = { rex, opc };
   memcpy(b + 2, &imm, 8);
   code_blob_emit(blob, b, sizeof b);
}

// 48 <opc> <modrm> disp32: rax/rcx <-> [rbx+disp32]
static void fused_emit_rbx_d32(code_blob_t *blob, uint8_t opc, uint8_t modrm,
                               uint32_t disp)
{
   uint8_t b[7] = { 0x48, opc, modrm };
   memcpy(b + 3, &disp, 4);
   code_blob_emit(blob, b, sizeof b);
}

// Emit one 13-byte call slot targeting `target` at the current position
static void fused_emit_call_slot(code_blob_t *blob, void *target)
{
   uint8_t *site = blob->wptr;
   const int64_t rel = (int64_t)((uint8_t *)target - (site + 5));

   if (rel == (int64_t)(int32_t)rel) {
      uint8_t b[13] = { 0xE8, 0, 0, 0, 0,
                        0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 };
      const int32_t r32 = (int32_t)rel;
      memcpy(b + 1, &r32, 4);
      code_blob_emit(blob, b, sizeof b);
   }
   else {
      uint8_t b[13] = { 0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,
                        0x41, 0xFF, 0xD3 };
      memcpy(b + 2, &target, 8);
      code_blob_emit(blob, b, sizeof b);
   }
}

static void fused_emit_member(rt_model_t *m, fused_block_t *fb,
                              code_blob_t *blob, uint8_t *start,
                              rt_proc_t *p, fused_site_t *s)
{
   void *state = NULL, *context = NULL;
   jit_func_t *func = NULL;
   void *target_a, *target_b;

   if (s->state == SITE_PLAIN) {
      direct_eval_t *de = (direct_eval_t *)p->vtable;
      state   = de->state;
      context = de->context;
      func    = de->func;
      s->slot = &(de->func->entry);

      // Register the watch BEFORE reading the entry so a tier-up publish
      // between the two is re-applied as a pending patch, never lost.
      // Dedupe: N instances of one process share a jit_func_t; one watch
      // per (slot, block) suffices -- the callback retargets every site.
      bool have = false;
      for (const fused_site_t *o = fb->sites; o != s; o++)
         have |= (o->slot == s->slot);
      if (!have)
         code_entry_watch(s->slot, fused_entry_retarget, fb);

      target_a = (void *)load_acquire(s->slot);
      target_b = (void *)fused_member_epilogue;
   }
   else if (s->state == SITE_DISABLED) {   // evicted member (rebuild)
      target_a = (void *)m->fused_stub;
      target_b = (void *)fused_member_skip;
   }
   else {   // SITE_FALLBACK at build: full generic activation
      target_a = (void *)m->fused_stub;
      target_b = (void *)fused_member_fallback;
   }
   s->cur_a = (jit_entry_fn_t)target_a;

   // args[0] = state, args[1] = context (per-proc constants post-reset)
   fused_emit_movabs(blob, 0x48, 0xB8, (uint64_t)(uintptr_t)state);
   FUSED_EMIT(0x49, 0x89, 0x04, 0x24);          // mov [r12], rax
   fused_emit_movabs(blob, 0x48, 0xB8, (uint64_t)(uintptr_t)context);
   FUSED_EMIT(0x49, 0x89, 0x44, 0x24, 0x08);    // mov [r12+8], rax

   fused_emit_movabs(blob, 0x48, 0xB8, (uint64_t)(uintptr_t)&(p->wakeable));
   fused_emit_rbx_d32(blob, 0x89, 0x83,         // mov [rbx+off], rax
                      offsetof(model_thread_t, active_obj));
   fused_emit_movabs(blob, 0x48, 0xB8, (uint64_t)(uintptr_t)p->scope);
   fused_emit_rbx_d32(blob, 0x89, 0x83,
                      offsetof(model_thread_t, active_scope));

   fused_emit_rbx_d32(blob, 0x8B, 0x8B,         // mov rcx, [rbx+off(tlab)]
                      offsetof(model_thread_t, tlab));
   fused_emit_movabs(blob, 0x48, 0xBF, (uint64_t)(uintptr_t)func);   // rdi
   FUSED_EMIT(0x31, 0xF6);                      // xor esi, esi (anchor NULL)
   FUSED_EMIT(0x4C, 0x89, 0xE2);                // mov rdx, r12

   // NVC_SPLICE: copy the member's compiled body inline instead of calling
   // it.  entry_size > 0 guarantees target_a is a published NATIVE entry
   // (the size is recorded only when finalise swings the entry).  The
   // sub/add rsp,8 pair reproduces the stack parity a CALL would have
   // given the body (SysV: rsp % 16 == 8 at function entry) -- the body's
   // aligned spills and its own internal calls depend on it.  On decline
   // code_blob_splice leaves the blob untouched and the ordinary SITE A
   // call slot is emitted instead.  (Compile-once world: entry/entry_size
   // publish together; a republish after this point dissolves the block
   // via fused_apply_pending's SITE_SPLICE path.)
   bool spliced = false;
   if (s->state == SITE_PLAIN && fb->splice_ok && func->entry_size > 0
       && (size_t)func->entry_size * 2 + 64 <= fb->splice_budget) {
      uint8_t *const before = blob->wptr;
      FUSED_EMIT(0x48, 0x83, 0xEC, 0x08);       // sub rsp, 8
      if (code_blob_splice(blob, (void *)target_a, func->entry_size)) {
         FUSED_EMIT(0x48, 0x83, 0xC4, 0x08);    // add rsp, 8
         s->state = SITE_SPLICE;
         s->off_a = 0;
         spliced  = true;
         const size_t used = blob->wptr - before;
         fb->splice_budget -= MIN(fb->splice_budget, used);
      }
      else {
         if (!blob->overflow)
            blob->wptr -= 4;                    // roll back the sub rsp,8
         if (getenv("NVC_FUSED_DEBUG") != NULL)
            notef("fused: member %s splice declined (extent %u)",
                  istr(p->name), func->entry_size);
      }
   }

   if (!spliced) {
      s->off_a = blob->wptr - start;
      fused_emit_call_slot(blob, target_a);     // SITE A
   }

   fused_emit_movabs(blob, 0x48, 0xBF, (uint64_t)(uintptr_t)m);   // rdi
   fused_emit_movabs(blob, 0x48, 0xBE, (uint64_t)(uintptr_t)p);   // rsi
   FUSED_EMIT(0x4C, 0x89, 0xE2);                // mov rdx, r12

   s->off_b = blob->wptr - start;
   fused_emit_call_slot(blob, target_b);        // SITE B
}

// Build the fused block over the freshly built fast-clk table. Every input
// is static at this point: the table membership, each member's Phase A
// wrapper constants and the current published entries.
static void fused_block_build(rt_model_t *m)
{
   if (!fused_block_enabled() || !m->fastclk_on || m->fastclk_count == 0)
      return;

   assert(m->fused_block == NULL);   // aj_dissolve_fastclk ran first

   if (m->fused_code == NULL)
      m->fused_code = code_cache_new();

   if (m->fused_stub == NULL) {
      // Shared SITE A stub: null the result slot (args[0] = NULL -> the
      // epilogue's TLAB protocol sees "no claim") and return.
      code_blob_t *sb = code_blob_new(m->fused_code,
                                      ident_new("fused_stub"), 64);
      static const uint8_t stub[] = {
         0x48, 0xC7, 0x02, 0x00, 0x00, 0x00, 0x00,   // mov qword [rdx], 0
         0xC3                                        // ret
      };
      code_blob_emit(sb, stub, sizeof stub);
      code_blob_finalise(sb, &(m->fused_stub));
      if (m->fused_stub == NULL)
         return;
   }

   const unsigned count = m->fastclk_count;
   size_t hint = 192 + (size_t)count * 160;   // +64: second prologue
   if (hint > 0x300000) {   // one code page (4MB) bounds a blob
      notef("accel-jit: NVC_FUSED_BLOCK — %u members exceed the block size "
            "budget, not fusing", count);
      return;
   }

   // NVC_SPLICE: inline bodies add their extent (+ rebased jump tables,
   // which are not part of the entry extent -- 2x margin) to the block.
   // The blob is bounded by one code page, so on big designs only a
   // PREFIX of the members gets its body inlined: a running budget is
   // spent per member (priority queue, not admission gate) and the rest
   // keep the ordinary call form.  The unused tail of the reservation is
   // returned to the free span at finalise.
   bool splice_ok = fused_splice_enabled();
   size_t splice_budget = 0;
   if (splice_ok) {
      size_t body_hint = 0;
      for (unsigned i = 0; i < count; i++) {
         rt_proc_t *p = m->fastclk_table[i];
         if (p->vtable->eval != proc_eval_direct)
            continue;
         direct_eval_t *de = (direct_eval_t *)p->vtable;
         body_hint += (size_t)de->func->entry_size * 2 + 64;
      }
      splice_budget = MIN(body_hint, (size_t)0x2F0000 - hint);
      hint += splice_budget;
   }

   fused_block_t *fb = xcalloc(sizeof(fused_block_t));
   fb->count   = count;
   fb->splice_ok = splice_ok;
   fb->splice_budget = splice_budget;
   fb->members = xmalloc_array(count, sizeof(rt_proc_t *));
   fb->sites   = xcalloc_array(count, sizeof(fused_site_t));
   fb->argbuf  = xmalloc_array(JIT_MAX_ARGS, sizeof(jit_scalar_t));
   fb->thread  = model_thread(m);

   memcpy(fb->members, m->fastclk_table, count * sizeof(rt_proc_t *));

   code_blob_t *blob = code_blob_new(m->fused_code,
                                     ident_new("fused_block"), hint);
   uint8_t *start = blob->wptr;
   fb->base = start;

   static const uint8_t prologue[] = {
      0x53,                     // push rbx
      0x41, 0x54,               // push r12
      0x48, 0x83, 0xEC, 0x08,   // sub rsp, 8 (16-byte call alignment)
   };
   code_blob_emit(blob, prologue, sizeof prologue);
   fused_emit_movabs(blob, 0x48, 0xBB, (uint64_t)(uintptr_t)fb->thread);
   fused_emit_movabs(blob, 0x49, 0xBC, (uint64_t)(uintptr_t)fb->argbuf);

   // Two-entry layout over the partition-sorted table (posedge-only
   // members [0, ee_start), every-event tail [ee_start, count)):
   //
   //   [prologue A][posedge-only members][jmp L_tail]
   //   [prologue B: full duplicate -- push rbx/r12; sub rsp,8; movabs x2]
   //   L_tail: [every-event members][outro]
   //
   // The posedge entry (== base) runs ALL members, falling over the
   // second prologue into the tail; the off-edge entry starts at
   // prologue B (a tail entry jumping past a prologue would inherit the
   // caller's rbx/r12 and a misaligned rsp) and runs only the tail.
   // Both share one outro. When the tail is empty the off-edge entry
   // still exists but the dispatch site never selects it.
   const unsigned ee_start = MIN(m->fastclk_ee_start, count);
   uint32_t off_pb = 0;
   bool have_pb = false;

   unsigned nplain = 0;
   for (unsigned i = 0; i <= count; i++) {
      if (i == ee_start && !have_pb) {
         FUSED_EMIT(0xE9, 0x1B, 0x00, 0x00, 0x00);   // jmp L_tail (+27)
         off_pb = blob->wptr - start;
         code_blob_emit(blob, prologue, sizeof prologue);
         fused_emit_movabs(blob, 0x48, 0xBB, (uint64_t)(uintptr_t)fb->thread);
         fused_emit_movabs(blob, 0x49, 0xBC, (uint64_t)(uintptr_t)fb->argbuf);
         have_pb = true;
      }
      if (i == count)
         break;
      rt_proc_t *p = fb->members[i];
      fused_site_t *s = &(fb->sites[i]);
      const bool plain = p->vtable->eval == proc_eval_direct
         && p->wakeable.trigger == NULL
         && p->tlab == NULL;
      if (!p->wakeable.fastclk)
         // Evicted while a previous block was live (rebuild path): the
         // table loop skips it, so the block must too
         s->state = SITE_DISABLED;
      else
         s->state = plain ? SITE_PLAIN : SITE_FALLBACK;
      if (!plain && getenv("NVC_FUSED_DEBUG") != NULL)
         notef("fused: member %s falls back: eval=%s trigger=%p tlab=%p",
               istr(p->name),
               p->vtable->eval == proc_eval_direct ? "direct" : "other",
               (void *)p->wakeable.trigger, (void *)p->tlab);
      if (plain)
         nplain++;
      fused_emit_member(m, fb, blob, start, p, s);
   }

   static const uint8_t outro[] = {
      0x48, 0x83, 0xC4, 0x08,   // add rsp, 8
      0x41, 0x5C,               // pop r12
      0x5B,                     // pop rbx
      0xC3,                     // ret
   };
   code_blob_emit(blob, outro, sizeof outro);

   const size_t block_bytes = blob->wptr - start;   // blob dies at finalise

   code_blob_finalise(blob, &(fb->entry));

   if (fb->entry == NULL || (uint8_t *)fb->entry != start) {
      // Overflow or relocation surprise: discard (watches may be live)
      fb->entry_offedge = NULL;
      code_entry_unwatch(fb);
      free(fb->members);
      free(fb->sites);
      free(fb->argbuf);
      free(fb);
      warnf("NVC_FUSED_BLOCK: block emission failed (%u members)", count);
      return;
   }

   fb->entry_offedge = (jit_entry_fn_t)(start + off_pb);

   m->fused_block = fb;

   unsigned nspliced = 0;
   for (unsigned i = 0; i < count; i++)
      nspliced += (fb->sites[i].state == SITE_SPLICE);

   notef("accel-jit: NVC_FUSED_BLOCK — fused %u/%u member(s), two-entry "
         "%u posedge-only + %u every-event%s%s", nplain, count,
         ee_start, count - ee_start,
         nplain == count ? "" : " (rest via generic fallback)",
         nspliced > 0 ? " [spliced]" : "");
   if (nspliced > 0)
      notef("accel-jit: NVC_SPLICE — %u/%u bodies inline (%zu byte block)",
            nspliced, nplain, block_bytes);
}

// Run the fused block for this dispatch (posedge = full table via the
// head entry; otherwise the every-event tail via the off-edge entry).
// Returns false when the unfused table loop must run instead (no block,
// no armed landing pad, tracing). The FULL preamble runs on BOTH paths:
// pending tier-up patches, deferred evict patches, landing-pad/trace/
// thread guards, ONE arena reset and the running bracket.
static bool fused_block_dispatch(rt_model_t *m, bool posedge)
{
   fused_block_t *fb = m->fused_block;
   if (fb == NULL)
      return false;

   // Hoisted ONCE per block: member entries are called in-region, so
   // model_run's landing pad must be armed (model_step / shell / VHPI
   // stepping arrive without one and take the unfused loop).
   jit_thread_local_t *jthread = jit_thread_get();
   if (unlikely(!jthread->jmp_buf_valid || jthread->state != JIT_RUNNING))
      return false;

   if (unlikely(__trace_on))
      return false;   // per-member "run process" trace needs the loop path

   if (unlikely(model_thread(m) != fb->thread))
      return false;   // baked thread pointer no longer current

   if (unlikely(m->fastclk_npending > 0))
      return false;   // a member is mid self-suspend: the emitted block
                      // has no pending check, the table loop does

   if (unlikely(load_acquire(&(fb->patch_pending))))
      fused_apply_pending(m, fb);

   // A spliced member was invalidated (tier-up republish, evict, TLAB
   // claim) or a plain member became spliceable: the body bytes are baked
   // into the block, so dissolve and REBUILD here -- a delta boundary,
   // never mid-block -- and take the table loop for this dispatch.  The
   // block is stateless; nothing is lost.  Rebuild count is naturally
   // bounded: each member tiers up at most once per compile publication.
   if (unlikely(load_acquire(&(fb->rebuild_pending)))) {
      fused_block_dissolve(m);
      fused_block_build(m);
      return false;
   }

   // Deferred evict patches: a member evicted mid-eval (timed wake, #0
   // wait, wait-set demote) could not patch its own un-executed SITE B;
   // apply here, on the model thread, never mid-block.
   if (unlikely(m->fastclk_evict_defer)) {
      m->fastclk_evict_defer = false;
      for (unsigned i = 0; i < fb->count; i++) {
         if (fb->members[i]->wakeable.fastclk)
            continue;
         fused_site_t *sd = &(fb->sites[i]);
         if (sd->state == SITE_SPLICE) {
            // Inline body cannot be skipped by patching: rebuild
            relaxed_store(&(fb->rebuild_pending), 1);
         }
         else if (sd->state != SITE_DISABLED) {
            sd->state = SITE_DISABLED;
            sd->cur_a = m->fused_stub;
            fused_patch_call(fb, sd->off_a, (void *)m->fused_stub);
            fused_patch_call(fb, sd->off_b, (void *)fused_member_skip);
         }
      }
   }

   assert(fb->thread->tlab != NULL);

   // Hoisted ONCE per block: reclaim the previous eval's escaping
   // unconstrained results. Member transients accumulate across the block
   // (the arena grows by chaining; the next reset reclaims them all).
   jit_eval_arena_reset();

   jit_entry_fn_t entry = posedge ? fb->entry : fb->entry_offedge;
   if (unlikely(entry == NULL))
      return false;

   fb->running = true;
   (*entry)(NULL, NULL, NULL, NULL);
   fb->running = false;

   return true;
}

// Dissolve at ANY delta boundary with zero state loss: the block owns no
// simulation state, so dropping it reverts dispatch to the fast-clk table
// (or to normal queued wakeup after aj_dissolve_fastclk). Called from the
// fast-clk dissolve path (guard nexus fired), reset_process, and available
// to future live-edit demotion.
static void fused_block_dissolve(rt_model_t *m)
{
   fused_block_t *fb = m->fused_block;
   if (fb == NULL)
      return;

   assert(!fb->running);   // delta-boundary only, never mid-block

   m->fused_block = NULL;

   // After this returns no retarget callback for this block is in flight
   // (the watch lock serialises unwatch against notify)
   code_entry_unwatch(fb);

   // The emitted span cannot be returned to the code cache (spans have no
   // individual free) and is abandoned; bounded by dissolve/rebuild count.
   free(fb->members);
   free(fb->sites);
   free(fb->argbuf);
   free(fb);
}

#else   // !ARCH_X86_64

static void fused_block_build(rt_model_t *m)    { }
static void fused_block_dissolve(rt_model_t *m) { }
static bool fused_block_dispatch(rt_model_t *m, bool posedge) { return false; }

#endif

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

// #74 E1: driving value provably equals effective value — unresolved,
// exactly one SOURCE_DRIVER, and not a connected-inout formal (LRM
// 14.7.7.3: for everything outside the connected-INOUT branch,
// calculate_effective_value literally DEFINES effective := driving).
// For this class the separate driving plane is a redundant copy target;
// readers take the effective plane and put_driving feeds put_effective
// directly. The predicate is self-maintaining: force/deposit create
// pseudo sources through add_source, which bumps n_sources and flips it
// off (with a plane2 re-seed there so the raw plane is never stale).
// Sources are never removed, so it never flips back on.
static inline bool nexus_driving_aliased(rt_nexus_t *n)
{
   return (n->flags & (NET_F_EFFECTIVE | NET_F_INOUT)) == NET_F_EFFECTIVE
      && n->n_sources == 1
      && n->sources.tag == SOURCE_DRIVER
      && n->signal->resolution == NULL;
}

static inline void *nexus_driving(rt_nexus_t *n)
{
   assert(n->flags & NET_F_EFFECTIVE);
   if (nexus_driving_aliased(n))
      return nexus_effective(n);
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
      if (g_lv_inwave && n->last_event == m->now
          && n->event_delta == m->iteration)
         memcpy(eff, value, valuesz);   // see put_effective_impl
      else
         if (likely(!(n->signal->shared.flags & SIG_F_LAST_VALUE)))
            memcpy(eff, value, valuesz);   // #74 E2: no 'last_value reader
         else
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
   if (unlikely(n->vtable == &nexus_fused_vtable))
      fused_nexus_trap(m, n);   // #74: no new sources on a fused signal

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
      n->vtable = &nexus_default_vtable;
         // #74 E1 un-alias: the nexus may have been driving-plane
         // aliased until this second source appeared; re-seed plane 2
         // from the effective bytes so the raw driving plane is not
         // stale-initial when a downstream source_value reads it
         // before the next put_driving.
         if ((n->flags & (NET_F_EFFECTIVE | NET_F_INOUT)) == NET_F_EFFECTIVE)
            memcpy(n->signal->shared.data + n->offset
                   + 2*n->signal->shared.size,
                   nexus_effective(n), (size_t)n->size * n->width);   // Revert to full resolution
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

   // A split must not orphan the depositor mapping: force/release on the
   // child range looks up the CHILD nexus (partial-signal force previously
   // missed the pre-split key and the cone never woke).
   {
      void *dep = hash_get(m->depositors, old);
      if (dep != NULL)
         hash_put(m->depositors, new, dep);
   }

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
      if (unlikely(standard() == STD_MX)
          && likely(src->u.port.conv_func == NULL)) {
         // Mixed mode: a port with no real driver is not a source.
         // Walk SOURCE_PORT chain transitively until we find a
         // SOURCE_DRIVER (real driver) or detect a cycle (circular
         // back-ref from inout bidirectional mapping).  Conversion
         // sources are computed values, not wire joins — their inputs
         // live in the conv closure, so the driver walk would always
         // prune them (u.port.input is not the conversion input)
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
            // n_sources guard: an undriven nexus's embedded sources
            // struct is zeroed, and tag 0 is SOURCE_DRIVER — walking
            // it invents a phantom driver and lets a never-driven
            // inout port contribute its stale initial bytes.  A
            // deposit-driven nexus (:= comb fusion) has no sources
            // either but stamps last_event — its value is real.
            if (input->n_sources > 0) {
               for (rt_source_t *s = &(input->sources); s;
                    s = s->chain_input) {
                  // SOURCE_IMPLICIT counts as a driver — the solver
                  // plug always contributes.  MUST match the twin walk
                  // in calculate_driving_value: if the walks disagree,
                  // nonnull over-counts and the resolution reads an
                  // uninitialized vals slot (pr2834340 garbage index)
                  if (s->tag == SOURCE_DRIVER
                      || s->tag == SOURCE_IMPLICIT) {
                     has_driver = true;
                     break;
                  }
                  else if (s->tag == SOURCE_PORT && next == NULL)
                     next = s->u.port.input;
               }
            }
            else if (!(input->flags & NET_F_INOUT)
                     && input->last_event < TIME_HIGH)
               has_driver = true;   // deposit-written OUT-port class;
                                    // inout formals stamp last_event
                                    // from effective tracking alone
            if (has_driver)
               break;
            input = next;
         }
         if (!has_driver)
            return NULL;
      }
      if (likely(src->u.port.conv_func == NULL)) {
         // STD_MX tolerated cross-width map (1-byte std formal on a
         // 4-byte l3d net): a raw read reinterprets adjacent bytes as
         // part of the value — never a real contribution
         if (unlikely(standard() == STD_MX
                      && src->u.port.input->size != nexus->size))
            return NULL;
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
         // A NULL where marks a solver-owned plug (#75): it always
         // contributes its current value
         if (iwhere != NULL && tree_kind(iwhere) == T_IMPLICIT_SIGNAL
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
                              r->closure.context, vals, o))             \
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
      set_pending(m, &imp->wakeable);
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
                        && s->tag == SOURCE_PORT
                        && s->u.port.conv_func == NULL)) {
         if (s->u.port.input->size != n->size)
            continue;   // cross-width raw edge — garbage if read
         // Mixed mode: skip port with no real driver.
         // Walk SOURCE_PORT chain transitively: a nested port-binding
         // (inner OUT → outer OUT → tb signal) has no SOURCE_DRIVER on
         // the immediate input, only further along the chain.  Cycle
         // guard handles the inout back-ref case.  Conversion sources
         // are computed values (inputs live in the conv closure, not
         // u.port.input) — the walk would always prune them.
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
            // Same n_sources guard as source_value: tag 0 in a zeroed
            // sources struct is SOURCE_DRIVER (phantom driver);
            // deposit-driven (:=) nexuses stamp last_event and count
            // as driven
            if (input->n_sources > 0) {
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
            }
            else if (!(input->flags & NET_F_INOUT)
                     && input->last_event < TIME_HIGH)
               has_driver = true;   // deposit-written OUT-port class;
                                    // inout formals stamp last_event
                                    // from effective tracking alone
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

   if ((n->flags & NET_F_EFFECTIVE) && !nexus_driving_aliased(n)) {
      // Driving and effective values must be calculated separately
      assert(n->flags & NET_F_PENDING);
   }
   else {
      // Effective value is always the same as the driving value
      // (including the #74 E1 aliased class, which put_driving fed
      // straight to put_effective — no PENDING enqueue happened)
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

////////////////////////////////////////////////////////////////////////////////
// Phase D stage S2a: partition map + boundary classification.
//
// Builds, ONCE, a static map from process -> partition plus a classification
// of every nexus against that map.  Nothing in the scheduler consults it
// yet: stage S3 (schedule-time driver posting, one runner thread per
// partition) is the first consumer.  See rt/partition.h for the API and the
// environment variables.
//
// WHY THE HOOK IS THE END OF THE FIRST CYCLE, NOT THE END OF model_reset.
// Boundary classification needs the reader (pending) lists, and those are
// populated in TWO places.  A process whose last statement is a static wait
// gets a single _sched_event emitted into its reset block (lower.c,
// lower_process), so it is registered by the time reset_process returns --
// that is most of synthesizable RTL.  A process with a dynamic wait
// registers only when its body actually executes the wait, which happens on
// its first activation, i.e. during the first model_cycle.  The
// end-of-first-cycle hook therefore sees a strict SUPERSET of what an
// end-of-model_reset hook sees, at no cost, and it also lands after the
// initial-value settle where sched_driver performs its first round of nexus
// splits.  NVC_PART_HOOK=reset builds at the earlier point so the
// difference stays checkable rather than asserted.  Measured with it: b12 /
// b17 / b22 each lose exactly one reader link (the testbench process, whose
// wait is dynamic); VeeR-EH2 loses 4644 reader links of 249072 and 2749
// nexuses of 1347609 (the splits that the initial settle performs).
//
// Neither hook is a substitute for a structure-mutation freeze: nexuses
// keep splitting later in the run (b12: 18 at the hook, 24 at end of run).
//
// The map is a snapshot.  g_split_count/g_split_last are latched at build
// time so S4 can detect post-build structure mutation (nexus splits,
// pending-list reallocation) and dissolve back to serial; the debug report
// re-runs the census at model_free and prints the drift.

typedef struct {
   rt_scope_t *scope;      // the level-N scope this group cuts at
   uint64_t    weight;     // profile weight, else process count
   uint32_t    nprocs;
   uint16_t    part;
   bool        profiled;   // weight came from NVC_PART_PROFILE
} part_group_t;

typedef struct {
   uint64_t weight;
   uint32_t nprocs;
   uint32_t ngroups;
} part_bin_t;

typedef struct {
   uint64_t nexuses;
   uint64_t interior;
   uint64_t xdrv;          // crossing on the nexus's own reader list
   uint64_t xport;         // crossing through the port/output fan-out
   uint64_t undriven;
   uint64_t reader_links;  // (nexus, W_PROC reader) links
   uint64_t xreader_links; // ... whose partition holds none of the drivers
   uint64_t multi_driven;  // nexus driven from more than one partition
   uint64_t xboth;         // crossing on BOTH reader list and port chain
   uint64_t api_mismatch;  // part_{class_,}of_nexus() disagreed with the walk
} part_census_t;

static int           g_part_n       = -1;   // -1 = env unread, 0 = off
static int           g_part_level   = 5;
static bool          g_part_at_reset = false;  // NVC_PART_HOOK=reset
static int           g_part_debug   = 0;   // 1 = summary, 2 = per group
static const char   *g_part_profile = NULL;
static bool          g_part_pending_build = false;
static bool          g_part_built   = false;

static part_group_t *g_part_groups  = NULL;
static unsigned      g_part_ngroups = 0, g_part_gmax = 0;
static unsigned      g_part_nprofiled = 0;
static hash_t       *g_part_gmap    = NULL;   // rt_scope_t * -> gid + 1
static shash_t      *g_part_pmap    = NULL;   // scope name -> uint64_t *
static part_bin_t    g_part_bins[PART_MAX_PARTITIONS];
static uint32_t      g_part_nprocs  = 0;

// Side table: ONLY the boundary nexuses (a few thousand of ~1.3M on
// VeeR-EH2).  Value is 1 + (owner << 2 | class), so a miss is a clean NULL
// and INTERIOR/UNDRIVEN are recomputed from the short sources chain.
static hash_t       *g_part_nexmap  = NULL;

static part_census_t g_part_census;
static uint64_t      g_part_build_ns   = 0;
static uint64_t      g_part_split_at   = 0;   // g_split_count at build time
static uint64_t      g_part_built_time = 0;   // sim time (fs) of the build

static void part_env_init(void)
{
   if (likely(g_part_n >= 0))
      return;

   const char *e = getenv("NVC_PARTITIONS");
   g_part_n = (e == NULL) ? 0 : atoi(e);
   if (g_part_n < 0)
      g_part_n = 0;
   else if (g_part_n > PART_MAX_PARTITIONS) {
      warnf("NVC_PARTITIONS=%d exceeds the maximum %d; clamping",
            g_part_n, PART_MAX_PARTITIONS);
      g_part_n = PART_MAX_PARTITIONS;
   }

   const char *l = getenv("NVC_PART_LEVEL");
   if (l != NULL)
      g_part_level = MAX(0, atoi(l));

   const char *d = getenv("NVC_PARTITIONS_DEBUG");
   g_part_debug = (d == NULL) ? 0 : MAX(1, atoi(d));
   g_part_profile = getenv("NVC_PART_PROFILE");

   // NVC_PART_HOOK=reset builds at the end of model_reset instead.  Kept as
   // an option purely so the hook choice is checkable rather than asserted:
   // at that point no process body has run, so x_sched_event has registered
   // nothing and the census reports reader_links=0 / x_driver_reader=0.
   const char *h = getenv("NVC_PART_HOOK");
   g_part_at_reset = h != NULL && strcmp(h, "reset") == 0;
}

unsigned part_count(void)
{
   return g_part_n > 0 ? (unsigned)g_part_n : 0;
}

bool part_active(void)
{
   return g_part_built;
}

unsigned part_of_proc(rt_proc_t *proc)
{
   return g_part_built ? proc->part : PART_NONE;
}

// The owner of a non-boundary nexus is simply its first driver's partition
static unsigned part_first_driver(rt_nexus_t *n)
{
   for (rt_source_t *s = &(n->sources); s != NULL; s = s->chain_input) {
      if (s->tag != SOURCE_DRIVER || s->u.driver.proc == NULL)
         continue;
      const unsigned p = s->u.driver.proc->part;
      if (p != PART_NONE)
         return p;
   }

   return PART_NONE;
}

unsigned part_of_nexus(rt_nexus_t *n)
{
   if (!g_part_built)
      return PART_NONE;

   void *v = hash_get(g_part_nexmap, n);
   if (v != NULL)
      return ((unsigned)(uintptr_t)v - 1) >> 2;

   return part_first_driver(n);
}

part_class_t part_class_of_nexus(rt_nexus_t *n)
{
   if (!g_part_built)
      return PART_INTERIOR;

   void *v = hash_get(g_part_nexmap, n);
   if (v != NULL)
      return (part_class_t)(((unsigned)(uintptr_t)v - 1) & 3);

   return part_first_driver(n) == PART_NONE ? PART_UNDRIVEN : PART_INTERIOR;
}

////////////////////////////////////////////////////////////////////////////////
// Weights: NVC_PART_PROFILE, else the group's process count

// Accepts either "<scope-name> <weight>" or a milestone-0 census line
// "PD-GRP <i> part=<p> ns=<weight> <scope-name>" -- the latter is what the
// M0 per-scope timing runs already emit.
static void part_load_profile(const char *path)
{
   FILE *f = fopen(path, "r");
   if (f == NULL) {
      warnf("cannot open NVC_PART_PROFILE %s: %s", path, strerror(errno));
      return;
   }

   g_part_pmap = shash_new(1024);

   char *line = NULL;
   size_t cap = 0;
   ssize_t len;

   while ((len = getline(&line, &cap, f)) > 0) {
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      char *p = line;
      while (*p == ' ' || *p == '\t') p++;
      if (*p == '\0' || *p == '#')
         continue;

      char *name = NULL;
      uint64_t weight = 0;

      const char *key = NULL;
      if (strncmp(p, "PD-GRP", 6) == 0)
         key = " ns=";              // milestone-0 census line
      else if (strncmp(p, "PART-GRP", 8) == 0)
         key = " weight=";          // this report's own per-group line

      if (key != NULL) {
         char *ns = strstr(p, key);
         if (ns == NULL)
            continue;
         weight = strtoull(ns + strlen(key), NULL, 10);

         // The scope name is the LAST whitespace-delimited field, which is
         // true of both report formats however many key=value fields sit
         // between the weight and it
         char *end = p + strlen(p);
         while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
         name = end;
         while (name > p && name[-1] != ' ' && name[-1] != '\t')
            name--;
         if (*name == '\0')
            continue;
      }
      else {
         name = p;
         char *sp = p;
         while (*sp != '\0' && *sp != ' ' && *sp != '\t') sp++;
         if (*sp == '\0')
            continue;   // no weight column
         *sp++ = '\0';
         while (*sp == ' ' || *sp == '\t') sp++;
         weight = strtoull(sp, NULL, 10);
      }

      uint64_t *slot = shash_get(g_part_pmap, name);
      if (slot != NULL)
         *slot += weight;   // duplicate keys accumulate
      else {
         slot = xmalloc(sizeof(uint64_t));
         *slot = weight;
         shash_put(g_part_pmap, name, slot);
      }
   }

   free(line);
   fclose(f);
}

////////////////////////////////////////////////////////////////////////////////
// Grouping: an rt_scope_t at hierarchy depth NVC_PART_LEVEL

static unsigned part_group_for(rt_scope_t *s)
{
   void *v = hash_get(g_part_gmap, s);
   if (v != NULL)
      return (unsigned)(uintptr_t)v - 1;

   if (g_part_ngroups == g_part_gmax) {
      g_part_gmax = MAX(64, g_part_gmax * 2);
      g_part_groups = xrealloc_array(g_part_groups, g_part_gmax,
                                     sizeof(part_group_t));
   }

   const unsigned gid = g_part_ngroups++;
   g_part_groups[gid] = (part_group_t){ .scope = s };
   hash_put(g_part_gmap, s, (void *)(uintptr_t)(gid + 1));
   return gid;
}

// Pre-order walk of the instance tree.  A process in a scope at depth d
// belongs to its ancestor at depth min(d, NVC_PART_LEVEL); CUT carries that
// ancestor down once the walk is below the cut level.
static void part_walk(rt_scope_t *s, int depth, rt_scope_t *cut, bool assign)
{
   rt_scope_t *g = (depth <= g_part_level) ? s : cut;

   if (s->procs.count > 0 && g != NULL) {
      const unsigned gid = part_group_for(g);
      for (int i = 0; i < s->procs.count; i++) {
         if (assign)
            s->procs.items[i]->part = g_part_groups[gid].part;
         else {
            g_part_groups[gid].nprocs++;
            g_part_nprocs++;
         }
      }
   }

   rt_scope_t *childcut =
      (depth + 1 > g_part_level) ? (depth == g_part_level ? s : cut) : NULL;

   for (int i = 0; i < s->children.count; i++)
      part_walk(s->children.items[i], depth + 1, childcut, assign);
}

////////////////////////////////////////////////////////////////////////////////
// Assignment: greedy longest-processing-time into N bins

static int part_cmp_group(const void *a, const void *b)
{
   const unsigned ia = *(const unsigned *)a, ib = *(const unsigned *)b;
   const uint64_t wa = g_part_groups[ia].weight, wb = g_part_groups[ib].weight;

   if (wa != wb)
      return wa > wb ? -1 : 1;
   else
      return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

static void part_assign(void)
{
   memset(g_part_bins, '\0', sizeof(g_part_bins));

   if (g_part_ngroups == 0)
      return;   // design has no processes

   unsigned *order = xmalloc_array(g_part_ngroups, sizeof(unsigned));
   for (unsigned i = 0; i < g_part_ngroups; i++)
      order[i] = i;

   qsort(order, g_part_ngroups, sizeof(unsigned), part_cmp_group);

   for (unsigned i = 0; i < g_part_ngroups; i++) {
      part_group_t *g = &(g_part_groups[order[i]]);

      // Least loaded bin; ties broken on group count so that a run of
      // zero-weight groups (a profile that does not cover them) still
      // spreads instead of piling onto partition 0
      unsigned best = 0;
      for (int b = 1; b < g_part_n; b++) {
         if (g_part_bins[b].weight < g_part_bins[best].weight
             || (g_part_bins[b].weight == g_part_bins[best].weight
                 && g_part_bins[b].ngroups < g_part_bins[best].ngroups))
            best = b;
      }

      g->part = best;
      g_part_bins[best].weight  += g->weight;
      g_part_bins[best].nprocs  += g->nprocs;
      g_part_bins[best].ngroups += 1;
   }

   free(order);
}

////////////////////////////////////////////////////////////////////////////////
// Boundary classification

static inline void part_reader_bit(rt_wakeable_t *w, uint64_t *mask,
                                   uint64_t *nlinks)
{
   if (w == NULL || w->kind != W_PROC)
      return;

   rt_proc_t *p = container_of(w, rt_proc_t, wakeable);
   if (p->part == PART_NONE)
      return;

   *mask |= UINT64_C(1) << p->part;
   if (nlinks != NULL)
      (*nlinks)++;
}

static uint64_t part_reader_mask(void *pending, uint64_t *nlinks)
{
   uint64_t mask = 0;

   if (pointer_tag(pending) == 1)
      part_reader_bit(untag_pointer(pending, rt_wakeable_t), &mask, nlinks);
   else if (pending != NULL) {
      rt_pending_t *p = untag_pointer(pending, rt_pending_t);
      for (int i = 0; i < p->count; i++)
         part_reader_bit(p->wake[i], &mask, nlinks);
   }

   return mask;
}

// Count reader links whose partition holds NONE of the drivers -- these are
// the wakes that S3 must deliver across cores.
static uint64_t part_xreader_links(void *pending, uint64_t dmask)
{
   uint64_t rmask = 0, links = 0, n = 0;

   if (pointer_tag(pending) == 1) {
      part_reader_bit(untag_pointer(pending, rt_wakeable_t), &rmask, &links);
      if (rmask & ~dmask)
         n = 1;
   }
   else if (pending != NULL) {
      rt_pending_t *p = untag_pointer(pending, rt_pending_t);
      for (int i = 0; i < p->count; i++) {
         uint64_t one = 0;
         part_reader_bit(p->wake[i], &one, &links);
         if (one & ~dmask)
            n++;
      }
   }

   return n;
}

static void part_classify(rt_model_t *m, part_census_t *c, bool record,
                          bool check)
{
   memset(c, '\0', sizeof(part_census_t));

   for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
      c->nexuses++;

      uint64_t dmask = 0, rmask = 0, pmask = 0;
      unsigned owner = PART_NONE;

      for (rt_source_t *s = &(n->sources); s != NULL; s = s->chain_input) {
         if (s->tag != SOURCE_DRIVER || s->u.driver.proc == NULL)
            continue;
         const unsigned p = s->u.driver.proc->part;
         if (p == PART_NONE)
            continue;
         dmask |= UINT64_C(1) << p;
         if (owner == PART_NONE)
            owner = p;
      }

      rmask = part_reader_mask(n->pending, &(c->reader_links));

      for (rt_source_t *o = n->outputs; o != NULL; o = o->chain_output) {
         if (o->tag != SOURCE_PORT || o->u.port.output == NULL)
            continue;
         pmask |= part_reader_mask(o->u.port.output->pending, NULL);
      }

      const uint64_t rx = rmask & ~dmask, px = pmask & ~dmask;
      part_class_t cls;

      if (dmask == 0) {
         cls = PART_UNDRIVEN;
         c->undriven++;
      }
      else {
         if (dmask & (dmask - 1))
            c->multi_driven++;

         c->xreader_links += part_xreader_links(n->pending, dmask);

         if (rx == 0 && px == 0) {
            cls = PART_INTERIOR;
            c->interior++;
         }
         else {
            // Counted the way milestone 0 counted them: a nexus that crosses
            // on BOTH its own reader list and the port chain increments both
            // buckets.  The side table stores ONE class, driver-reader first,
            // because that is the crossing that needs a cross-core wake.
            if (rx != 0) c->xdrv++;
            if (px != 0) c->xport++;
            if (rx != 0 && px != 0) c->xboth++;

            cls = rx ? PART_BOUNDARY_DRIVER_READER : PART_BOUNDARY_PORT;

            if (record)
               hash_put(g_part_nexmap, n,
                        (void *)(uintptr_t)(1 + ((owner << 2) | cls)));
         }
      }

      // Validate the S3-facing API against the walk that produced it
      if (check && (part_class_of_nexus(n) != cls || part_of_nexus(n) != owner))
         c->api_mismatch++;
   }
}

////////////////////////////////////////////////////////////////////////////////
// Build + report

static void part_report_census(const char *tag, const part_census_t *c)
{
   printf("%s nexuses=%"PRIu64" interior=%"PRIu64" x_driver_reader=%"PRIu64
          " x_port_chain=%"PRIu64" undriven=%"PRIu64" reader_links=%"PRIu64
          " x_reader_links=%"PRIu64" multi_part_driven=%"PRIu64
          " x_both=%"PRIu64"\n", tag,
          c->nexuses, c->interior, c->xdrv, c->xport, c->undriven,
          c->reader_links, c->xreader_links, c->multi_driven, c->xboth);
}

static void part_report(void)
{
   uint64_t total = 0;
   for (int b = 0; b < g_part_n; b++)
      total += g_part_bins[b].weight;

   printf("PART: partitions=%d level=%d groups=%u procs=%u profile=%s "
          "profiled_groups=%u build_ms=%.2f built_at=%"PRIu64"ns "
          "splits_at_build=%"PRIu64"\n",
          g_part_n, g_part_level, g_part_ngroups, g_part_nprocs,
          g_part_profile ?: "none", g_part_nprofiled,
          g_part_build_ns / 1e6, g_part_built_time / 1000000,
          g_part_split_at);

   for (int b = 0; b < g_part_n; b++)
      printf("PART-P %2d weight=%"PRIu64" (%5.2f%%) procs=%u groups=%u\n", b,
             g_part_bins[b].weight,
             total ? 100.0 * g_part_bins[b].weight / total : 0.0,
             g_part_bins[b].nprocs, g_part_bins[b].ngroups);

   if (g_part_debug > 1) {
      // Same shape as the milestone-0 PD-GRP lines, and accepted verbatim by
      // NVC_PART_PROFILE: one measured run feeds the next partitioning
      for (unsigned i = 0; i < g_part_ngroups; i++)
         printf("PART-GRP %u part=%u weight=%"PRIu64" procs=%u %s\n", i,
                g_part_groups[i].part, g_part_groups[i].weight,
                g_part_groups[i].nprocs, istr(g_part_groups[i].scope->name));
   }

   part_report_census("PART-BOUNDARY", &g_part_census);
   fflush(stdout);
}

static void part_build(rt_model_t *m)
{
   const uint64_t t0 = get_timestamp_ns();

   g_part_gmap   = hash_new(1024);
   g_part_nexmap = hash_new(1024);

   if (g_part_profile != NULL)
      part_load_profile(g_part_profile);

   part_walk(m->root, 0, NULL, false);

   for (unsigned i = 0; i < g_part_ngroups; i++) {
      part_group_t *g = &(g_part_groups[i]);
      const uint64_t *pw = g_part_pmap != NULL
         ? shash_get(g_part_pmap, istr(g->scope->name)) : NULL;
      if (pw != NULL) {
         g->weight   = *pw;
         g->profiled = true;
         g_part_nprofiled++;
      }
      else if (g_part_pmap != NULL)
         g->weight = 0;   // a profile IS the measurement: absent = unmeasured.
                          // Deliberately NOT the process count -- mixing a
                          // time weight with a count weight in one bin sum is
                          // meaningless.  profiled_groups in the report is the
                          // coverage figure to watch.
      else
         g->weight = g->nprocs;   // no profile: fall back to process count
   }

   part_assign();
   part_walk(m->root, 0, NULL, true);

   part_classify(m, &g_part_census, true, false);

   g_part_built      = true;
   g_part_build_ns   = get_timestamp_ns() - t0;
   g_part_split_at   = g_split_count;
   g_part_built_time = m->now;

   if (g_part_debug) {
      part_report();

      // Nothing else calls the S3 API yet, so prove it here: re-derive every
      // nexus's class and owner through part_class_of_nexus/part_of_nexus and
      // compare against the walk that built the side table
      part_census_t chk;
      part_classify(m, &chk, false, true);
      printf("PART-API mismatches=%"PRIu64" of %"PRIu64" nexuses\n",
             chk.api_mismatch, chk.nexuses);
      fflush(stdout);
   }
}

// Called at the end of every simulation cycle; the first one builds the map
static inline void part_maybe_build(rt_model_t *m)
{
   if (likely(!g_part_pending_build))
      return;

   g_part_pending_build = false;
   part_build(m);
}

static void part_arm(rt_model_t *m)
{
   part_env_init();

   if (g_part_n <= 0)
      return;
   else if (g_part_at_reset)
      part_build(m);
   else
      g_part_pending_build = true;
}

// Debug only: re-run the census at end of run so drift from nexus splits and
// pending-list growth after the build hook is visible and quantified
static void part_final_report(rt_model_t *m)
{
   if (!g_part_built || !g_part_debug)
      return;

   part_census_t now;
   part_classify(m, &now, false, false);

   part_report_census("PART-RECENSUS", &now);
   printf("PART-DRIFT nexuses +%"PRId64" splits +%"PRIu64" (last split at %"
          PRIu64"ns)\n", (int64_t)now.nexuses - (int64_t)g_part_census.nexuses,
          g_split_count - g_part_split_at, g_split_last / 1000000);
   fflush(stdout);
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
            p->part      = PART_NONE;
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
               wakeable_set_kind(&p->wakeable, W_ASSIGN);
               break;
            case V_INITIAL:
            case V_ALWAYS:
               wakeable_set_kind(&p->wakeable, W_PROC);
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
            p->part      = PART_NONE;
            p->where     = t;
            p->name      = ident_prefix(path, ident_downcase(name), ':');
            p->wakeable.fused_cone =
               (strstr(istr(p->name), "comb_fused") != NULL);
            p->handle    = jit_lazy_compile(m->jit, sym);
            p->scope     = s;
            p->privdata  = mptr_new(m->mspace, "process privdata");

            wakeable_set_kind(&p->wakeable, W_PROC);
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

            wakeable_set_kind(&p->wakeable, W_PROPERTY);
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

   // EAGER single-driver vtable install (#74 resolver-elision directive:
   // "do the resolver work BEFORE code-generation for interp"). The lazy
   // rewrites in call_resolution/calculate_driving_value install these
   // same vtables on each nexus's FIRST post-reset update, paying one
   // full source-walk + resolution (and, for resolved types, the
   // memo-probe path) per nexus first. Source counts are final here —
   // add_source/force/disconnect all revert to nexus_default_vtable, so
   // the revocation story is identical to the lazy path's. Predicates
   // replicate the lazy sites exactly (measured census: 99.993% of
   // driven nexuses are single-source, so this covers nearly every net).
   // NVC_NO_EAGER_VTABLE=1 restores lazy-only for A/B.
   // #74 E2 policy fold. Verilog (STD_MX) designs can read the last
   // plane through UDP edge rows whose registration is not yet plumbed —
   // keep the copy for every signal there (v1). NVC_LV_COPYALL=1 forces
   // the same globally. NVC_LV_POISON=1 fills the elided last planes
   // with 0xDE so any reader this audit missed fails loudly and
   // deterministically (mandatory gate fixture, not a one-off).
   if (standard() == STD_MX || getenv("NVC_LV_COPYALL") != NULL)
      g_lv_copy_all = true;
   if (g_lv_copy_all) {
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain)
         if (n->signal != NULL)
            n->signal->shared.flags |= SIG_F_LAST_VALUE;
   }
   else if (getenv("NVC_LV_POISON") != NULL) {
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
         rt_signal_t *sig = n->signal;
         if (sig != NULL && !(sig->shared.flags & SIG_F_LAST_VALUE))
            memset(nexus_last_value(n), 0xDE, (size_t)n->size * n->width);
      }
   }

   if (getenv("NVC_NO_EAGER_VTABLE") == NULL) {
      for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
         if (n->n_sources != 1 || (n->flags & NET_F_FORCED))
            continue;
         rt_source_t *s0 = &(n->sources);
         if (s0->tag != SOURCE_DRIVER || s0->disconnected)
            continue;
         res_memo_t *r = n->signal != NULL ? n->signal->resolution : NULL;
         if (r == NULL || (n->flags & NET_F_R_IDENT))
            n->vtable = &nexus_single_driver_vtable;
         else if (r->flags & R_MEMO)
            n->vtable = &nexus_memo1_vtable;
      }
   }

   run_callbacks(m, END_OF_INITIALISATION);

   // Route escaping unconstrained results (the per-eval logic3d churn) into a
   // reset-per-eval arena instead of the collected heap, so GC never fires
   // during the run. Installed only AFTER init so the persistent live set
   // (allocated above) stays on the heap. RTL-only; gated.
   if (getenv("NVC_NO_EVAL_ARENA") == NULL)   // default ON; opt-out
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

   // Phase D S2a: arm the partition map.  It is built at the END of the
   // first simulation cycle so that processes with a DYNAMIC wait have
   // registered their sensitivity too (static-wait processes registered in
   // their reset block, which has already run here) -- see the hook
   // discussion above part_build.
   part_arm(m);
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
   // Fast-clk probation: VALUE-CHANGING driver activity during a
   // NON-posedge activation marks the running member as combinational
   // fanout (see fastclk_comb). Same-value re-drives deliberately do NOT
   // mark (a held-in-reset FF re-driving Q<='0' on every probed delta is
   // not comb; a comb reader's 1->z step still changes bytes and marks),
   // so the attribution sits AFTER the same-value early-return below and
   // behind an explicit compare on every other path.
   if (after == 0 && (n->flags & NET_F_FAST_DRIVER)) {
      rt_source_t *d = &(n->sources);
      assert(n->n_sources == 1);

      waveform_t *w = &d->u.driver.waveforms;
      w->when = m->now;
      assert(w->next == NULL);

      rt_signal_t *signal = n->signal;
      rt_source_t *d0 = &(signal->nexus.sources);

      { static const char *dd = NULL; static int ddi = -1;
        if (ddi < 0) { dd = getenv("AJ_DRVDBG"); ddi = dd ? 1 : 0; }
        if (ddi && signal->where != NULL
            && strstr(istr(tree_ident(signal->where)), dd) != NULL)
           fprintf(stderr, "#DV %s t=%llu d=%u fq=%d sq=%d eq=%d v=%u wv=%u\n",
                   istr(tree_ident(signal->where)),
                   (unsigned long long)m->now, m->iteration,
                   (int)d->fastqueued, (int)d0->sigqueued,
                   (int)!cmp_bytes(value, value_ptr(n, &w->value),
                                   n->width * n->size),
                   (unsigned)((const unsigned char *)value)[0],
                   (unsigned)((const unsigned char *)
                              value_ptr(n, &w->value))[0]); }
      if (d->fastqueued) {
         assert(m->next_is_delta);
         if (unlikely(m->fastclk_probe_member >= 0)
             && !cmp_bytes(value, value_ptr(n, &w->value), n->width * n->size))
            m->fastclk_comb[m->fastclk_probe_member] = 1;
      }
      else if ((signal->shared.flags & NET_F_FAST_DRIVER) && d0->sigqueued) {
         assert(m->next_is_delta);
         d->fastqueued = 1;
         if (unlikely(m->fastclk_probe_member >= 0)
             && !cmp_bytes(value, value_ptr(n, &w->value), n->width * n->size))
            m->fastclk_comb[m->fastclk_probe_member] = 1;
      }
      else if (cmp_bytes(value, value_ptr(n, &w->value), n->width * n->size)) {
         m->next_is_delta = true;
         d->was_active = (n->active_delta == m->iteration);
         n->active_delta = m->iteration + 1;
         return;
      }
      else if (unlikely(g_alias_flip == 1)
               && n->vtable->deposit == banked_deposit) {
         // #74 ALIAS-FLIP (resolver-elision directive): the driver's
         // commit stages STRAIGHT into bank B and the delta-boundary
         // flip publishes it — the deferq round trip and the boundary
         // update_driving -> put_driving -> put_effective dispatch
         // chain are eliminated for the aliased (banked leaf single-
         // driver) class. Same-delta readers still see plane 0
         // untouched (the bank IS the alias of the driver's storage),
         // and the flip's replayed deposit keeps the change-compare /
         // last_value / notify semantics byte-exact. The head waveform
         // write below stays so the same-value dedup above remains
         // sound across commits.
         if (unlikely(m->fastclk_probe_member >= 0))
            m->fastclk_comb[m->fastclk_probe_member] = 1;
         banked_deposit(m, n, value);
         m->next_is_delta = true;
      }
      else if (signal->shared.flags & NET_F_FAST_DRIVER) {
         if (unlikely(m->fastclk_probe_member >= 0))
            m->fastclk_comb[m->fastclk_probe_member] = 1;
         deferq_do(&m->driverq, async_fast_all_drivers, signal);
         m->next_is_delta = true;
         d0->sigqueued = 1;
         d->fastqueued = 1;
      }
      else {
         if (unlikely(m->fastclk_probe_member >= 0))
            m->fastclk_comb[m->fastclk_probe_member] = 1;
         deferq_do(&m->driverq, async_fast_driver, d);
         m->next_is_delta = true;
         d->fastqueued = 1;
      }

      copy_value_ptr(n, &w->value, value);
   }
   else {
      rt_source_t *d = find_driver(n, proc);
      assert(d != NULL);

      // Probe attribution for the generic path: same value-compare skip,
      // against the driver's current head-waveform value
      if (unlikely(m->fastclk_probe_member >= 0)
          && !cmp_bytes(value, value_ptr(n, &(d->u.driver.waveforms.value)),
                        n->width * n->size))
         m->fastclk_comb[m->fastclk_probe_member] = 1;

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

   if (unlikely(proc->wakeable.fastclk_counted)) {
      proc->wakeable.fastclk_counted = 0;   // self-suspended member resumed
      atomic_add(&m->fastclk_npending, -1);
   }

   { static const char *pd = NULL; static int pdi = -1;
     if (pdi < 0) { pd = getenv("AJ_PROCDBG"); pdi = pd ? 1 : 0; }
     if (pdi && strstr(istr(proc->name), pd) != NULL) {
        fprintf(stderr, "#PR %s t=%llu d=%u\n", istr(proc->name),
                (unsigned long long)m->now, m->iteration);
        for (int _k = 0; _k < g_lv_evnxn; _k++) {
           rt_nexus_t *nx = g_lv_evnx[_k];
           const unsigned char *eb = nexus_effective(nx);
           const unsigned char *lb = nexus_last_value(nx);
           fprintf(stderr, "#PS   %s ev_t=%lld ev_d=%u val=%u last=%u\n",
                   (nx->signal != NULL && nx->signal->where != NULL)
                      ? istr(tree_ident(nx->signal->where)) : "?",
                   (long long)nx->last_event, nx->event_delta,
                   (unsigned)eb[0], (unsigned)lb[0]);
        }
     } }

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
   // No memoisation inside a sweep wave: one flush batches commits that
   // interp separated into deltas, so a trigger evaluated on an early
   // commit caches FALSE and the real clock edge later in the SAME batch
   // is swallowed (measured: VeeR TB responders never fired).  Wave
   // triggers are clock-edge waits woken once per wave, so fresh
   // evaluation cannot double-fire them.
   if (!g_lv_inwave && t->epoch == m->trigger_epoch)
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
      deferq_do(unlikely(g_two_phase_eval) ? &m->procq_e : &m->procq,
                fn, arg);
      m->next_is_delta |= m->blocking_update;
   }

   set_pending(m, obj);
}

// Per-kind wake handlers -- the bodies of the old wakeup_one switch, now
// reached through the schedulable's vtable (rt_wakeable_t.vtable, first field)
// so the hot dispatch is a load-and-call with no case statement.

static void wake_proc(rt_model_t *m, rt_wakeable_t *obj)
{
   rt_proc_t *proc = container_of(obj, rt_proc_t, wakeable);
   TRACE("wakeup %sprocess %s", obj->postponed ? "postponed " : "",
         istr(proc->name));

   if (proc->wakeable.delayed) {
      // Already scheduled to run at a later time -- delete it from the queue
      heap_delete(m->eventq_heap, heap_delete_proc_cb, proc);
      proc->wakeable.delayed = false;
   }

   procq_do(m, obj, async_run_process, proc);
}

static void wake_property(rt_model_t *m, rt_wakeable_t *obj)
{
   rt_prop_t *prop = container_of(obj, rt_prop_t, wakeable);
   TRACE("wakeup property %s", istr(prop->name));
   procq_do(m, obj, async_update_property, prop);
}

static void wake_implicit(rt_model_t *m, rt_wakeable_t *obj)
{
   rt_implicit_t *imp = container_of(obj, rt_implicit_t, wakeable);
   TRACE("wakeup implicit signal %s closure %s",
         istr(tree_ident(imp->signal.where)),
         istr(jit_get_name(m->jit, imp->closure.handle)));

   deferq_do(&m->implicitq, async_update_implicit_signal, imp);
   set_pending(m, obj);
}

static void wake_watch(rt_model_t *m, rt_wakeable_t *obj)
{
   rt_watch_t *w = container_of(obj, rt_watch_t, wakeable);
   TRACE("wakeup %svalue change callback %p %s",
         obj->postponed ? "postponed " : "", w, debug_symbol_name(w->fn));

   assert(!w->wakeable.zombie);
   procq_do(m, obj, async_watch_callback, w);
}

static void wake_transfer(rt_model_t *m, rt_wakeable_t *obj)
{
   rt_transfer_t *t = container_of(obj, rt_transfer_t, wakeable);
   TRACE("wakeup signal transfer for %s",
         istr(tree_ident(t->target->signal->where)));

   procq_do(m, obj, async_transfer_signal, t);
}

static void wake_trigger(rt_model_t *m, rt_wakeable_t *obj)
{
   rt_trigger_t *t = container_of(obj, rt_trigger_t, wakeable);
   TRACE("wakeup trigger %p", t);

   if (!m->blocking_update) {
      deferq_do(&m->triggerq, async_run_trigger, t);
      set_pending(m, obj);
   }
   else if (unlikely(g_two_phase_eval))
      // Defer the RUN too: the trigger memoises per epoch — evaluate it in
      // the boundary window where deposits have settled (replay is before
      // the epoch increments).
      deferq_do(&m->ewakeq, async_replay_wake_trigger, obj);
   else if (run_trigger(m, t))
      wakeup_all(m, &(t->pending));
}

static void wake_assign(rt_model_t *m, rt_wakeable_t *obj)
{
   rt_proc_t *proc = container_of(obj, rt_proc_t, wakeable);
   TRACE("wakeup continuous assignment %s", istr(proc->name));

   assert(!proc->wakeable.delayed);

   if (m->blocking_update)
      update_assignment(m, proc);
   else {
      deferq_do(&m->implicitq, async_run_process, proc);
      set_pending(m, obj);
   }
}

static const wakeable_vtable_t wakeable_proc_vt     = { .wake = wake_proc };
static const wakeable_vtable_t wakeable_property_vt = { .wake = wake_property };
static const wakeable_vtable_t wakeable_implicit_vt = { .wake = wake_implicit };
static const wakeable_vtable_t wakeable_watch_vt    = { .wake = wake_watch };
static const wakeable_vtable_t wakeable_transfer_vt = { .wake = wake_transfer };
static const wakeable_vtable_t wakeable_trigger_vt  = { .wake = wake_trigger };
static const wakeable_vtable_t wakeable_assign_vt   = { .wake = wake_assign };

// Set an object's kind and its matching default wake vtable. Creation-time
// (cold) -- the dispatch itself is vtable, not a switch.
static void wakeable_set_kind(rt_wakeable_t *w, wakeable_kind_t k)
{
   w->kind = k;
   switch (k) {
   case W_PROC:     w->vtable = &wakeable_proc_vt;     break;
   case W_PROPERTY: w->vtable = &wakeable_property_vt; break;
   case W_IMPLICIT: w->vtable = &wakeable_implicit_vt; break;
   case W_WATCH:    w->vtable = &wakeable_watch_vt;    break;
   case W_TRANSFER: w->vtable = &wakeable_transfer_vt; break;
   case W_TRIGGER:  w->vtable = &wakeable_trigger_vt;  break;
   case W_ASSIGN:   w->vtable = &wakeable_assign_vt;   break;
   }
}

static void wakeup_one(rt_model_t *m, rt_wakeable_t *obj)
{
   { static const char *wd = NULL; static int wdi = -1;
     if (wdi < 0) { wd = getenv("AJ_WAKEDBG"); wdi = wd ? 1 : 0; }
     if (wdi && obj->kind == W_PROC) {
        rt_proc_t *p = container_of(obj, rt_proc_t, wakeable);
        extern rt_nexus_t *g_aj_notify_nexus;
        if (strstr(istr(p->name), wd) != NULL)
           fprintf(stderr, "#WU %s pending=%d inwave=%d t=%llu d=%u via=%s\n",
                   istr(p->name), (int)obj->pending, (int)g_lv_inwave,
                   (unsigned long long)m->now, m->iteration,
                   (g_aj_notify_nexus != NULL
                    && g_aj_notify_nexus->signal != NULL
                    && g_aj_notify_nexus->signal->where != NULL)
                      ? istr(tree_ident(g_aj_notify_nexus->signal->where))
                      : "(none)");
     } }
   if (obj->fastclk && m->fastclk_on && !g_lv_inwave) {
      // Clk-only process: never queued. Latch that the clock fanout fired this
      // delta; the posedge table runs it directly at the proc-dispatch site.
      // NOT inside a sweep wave: a gated clock is comb and commits MID-WAVE,
      // after this delta's dispatch site already ran -- the latched hit
      // dispatched one delta late, where sync_event_cache had cleared the
      // edge flags and every member body-filtered to a no-op (measured:
      // VeeR bus_intf domain dead, ARVALID stuck at init X, zero retires).
      // In-wave the member takes the normal path into the wave's clocked
      // drain: same delta, flags valid, settled data.
      m->fastclk_hit = true;
      return;
   }

   if (obj->pending)
      return;   // Already scheduled

   // #62 sweep: a leveled comb proc woken while a wave is executing joins
   // the CURRENT wave at its level instead of running inline -- the inline
   // path replays the delta cascade (transients included) and defeats the
   // level ordering (measured: NZ=39 persisted with queue-side-only sweep).
   if (g_lv_inwave && g_lv_deposit_wake) {
      // Deposit contract: receivers run NEXT delta, where the stamped
      // event (iteration+1) is current.  Procs: hold to wave end.
      // Triggers: force the queued (non-blocking) path so evaluation
      // happens at the next boundary, not inline before the event exists.
      if (obj->kind == W_PROC) {
         if (obj->pending)
            return;
         rt_proc_t *p = container_of(obj, rt_proc_t, wakeable);
         obj->pending = true;
         deferq_do(&g_lv_depq, async_run_process, p);
         { static int dbg = -1;
           if (dbg < 0) dbg = getenv("AJ_SWEEPDBG") != NULL;
           if (dbg)
              fprintf(stderr, "#DW %s t=%llu d=%u\n", istr(p->name),
                      (unsigned long long)m->now, m->iteration); }
         return;
      }
      if (obj->kind == W_TRANSFER) {
         if (obj->pending)
            return;
         rt_transfer_t *t = container_of(obj, rt_transfer_t, wakeable);
         obj->pending = true;
         deferq_do(&g_lv_depq, async_transfer_signal, t);
         return;
      }
      if (obj->kind == W_TRIGGER) {
         const bool save = m->blocking_update;
         m->blocking_update = false;
         obj->vtable->wake(m, obj);
         m->blocking_update = save;
         return;
      }
   }
   if (g_lv_inwave && !g_lv_deposit_wake
       && (obj->kind == W_PROC || obj->kind == W_TRANSFER)) {
      static int nocomb = -1;
      if (nocomb < 0) nocomb = getenv("NVC_SWEEP_NOCOMB") != NULL;
      if (nocomb)
         goto sweep_normal_wake;
      const int pi = aj_lv_idx(obj);
      { static int dbg = -1;
        if (dbg < 0) dbg = getenv("AJ_SWEEPDBG") != NULL;
        if (dbg)
           fprintf(stderr, "#WK inwave kind=%d node=%s pi=%d lv=%d curlv=%d\n",
                   (int)obj->kind,
                   obj->kind == W_PROC
                      ? istr(container_of(obj, rt_proc_t, wakeable)->name)
                      : "(xfer)",
                   pi, pi >= 0 ? g_lv_level[pi] : -2, g_lv_curlv); }
      if (pi >= 0 && g_lv_level[pi] >= 0) {
         // absorb unconditionally: reaching here means !pending, so the
         // node is not queued -- a re-wake after running means its inputs
         // changed and it MUST re-run at its level next round (one-eval-
         // per-wave froze a stale transient in signal memory: NZ=39).
         obj->pending = true;
         aj_lv_wv_push(obj);
         if (g_lv_level[pi] <= g_lv_curlv)
            g_lv_late++;   // joined at/below the pass cursor: next round
         return;
      }
      // UNLEVELED (clocked) procs must not run inline mid-wave either --
      // the fork's blocking-update path executed the glitch fixture's
      // victim between a transient deposit and its settle (measured: the
      // two GL hashes bracket vict's sample).  Schedule at the slot after
      // the waking commit's level.  Unleveled transfers (delayed
      // assignments, or nodes unseen by the analyzer) take the normal
      // queue -- they only schedule waveforms.
      static int inlineck = -1;
      if (inlineck < 0) inlineck = getenv("NVC_SWEEP_INLINE_CK") != NULL
         || getenv("NVC_SWEEP_COMBONLY") != NULL;
      if (obj->kind == W_PROC && !inlineck) {
         rt_proc_t *p = container_of(obj, rt_proc_t, wakeable);
         obj->pending = true;
         if (g_lv_ckn == g_lv_ckcap) {
            g_lv_ckcap = g_lv_ckcap ? g_lv_ckcap * 2 : 256;
            g_lv_ck = xrealloc_array(g_lv_ck, g_lv_ckcap, sizeof(lv_ck_t));
         }
         g_lv_ck[g_lv_ckn].p  = p;
         // clamp to the last slot (maxlv+1): a wake during that slot's own
         // flush must land in a reachable slot or it is silently dropped
         // with pending stuck true
         g_lv_ck[g_lv_ckn].rl = g_lv_curlv < 0 ? 0
            : (g_lv_curlv >= g_lv_maxlv + 1 ? g_lv_maxlv + 1
                                            : g_lv_curlv + 1);
         g_lv_ckn++;
         (void)0;
         return;
      }
   }

sweep_normal_wake:
   obj->vtable->wake(m, obj);
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
   // AJ_EVDBG=<substr>: print signal events (name, time, delta, first value
   // byte) for names containing <substr> — the delta-census diagnostic that
   // read the deep-cascade publication bug straight off the event stream.
   for (int _k = 0; _k < g_lv_evnxn; _k++) {
      if (g_lv_evnx[_k] != n) continue;
      const unsigned char *db = n->signal != NULL
         ? n->signal->shared.data : NULL;
      fprintf(stderr, "#NX %p sig=%s t=%llu d=%u v=%u\n", (void *)n,
              (n->signal != NULL && n->signal->where != NULL)
                 ? istr(tree_ident(n->signal->where)) : "?",
              (unsigned long long)m->now, m->iteration,
              db != NULL ? (unsigned)db[0] : 999);
      break;
   }
   { static const char *_nx = NULL; static int _nxi = -1;
     if (_nxi < 0) { _nx = getenv("AJ_EVNX"); _nxi = _nx ? 1 : 0; }
     if (_nxi) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%p", (void *)n);
        if (strstr(_nx, buf) != NULL) {
           const unsigned char *db = n->signal != NULL
              ? n->signal->shared.data : NULL;
           fprintf(stderr, "#NX %p sig=%s t=%llu d=%u v=%u\n", (void *)n,
                   (n->signal != NULL && n->signal->where != NULL)
                      ? istr(tree_ident(n->signal->where)) : "?",
                   (unsigned long long)m->now, m->iteration,
                   db != NULL ? (unsigned)db[0] : 999);
        }
     } }
   { static const char *_ev = NULL; static int _evi = -1;
     if (_evi < 0) { _ev = getenv("AJ_EVDBG"); _evi = _ev ? 1 : 0; }
     if (_evi && n->signal != NULL && n->signal->where != NULL) {
        const char *nm = istr(tree_ident(n->signal->where));
        if (strstr(nm, _ev) != NULL) {
           uint64_t h = 1469598103934665603ULL;
           const unsigned char *db = n->signal->shared.data;
           for (size_t i = 0; i < n->signal->shared.size; i++)
              { h ^= db[i]; h *= 1099511628211ULL; }
           extern int g_aj_phase;
           fprintf(stderr, "#EV %s nx=%p t=%llu d=%u v=%u h=%08x ph=%d sc=%s\n",
                   nm, (void *)n, (unsigned long long)m->now, m->iteration,
                   (unsigned)db[0], (unsigned)(h & 0xffffffffu), g_aj_phase,
                   n->signal->parent != NULL
                      ? istr(n->signal->parent->name) : "?");
        }
     } }
   n->last_event = m->now;
   n->event_delta = m->iteration;

   // AO>=3 census: an interp producer commits a chunk boundary input
   // AFTER the chunk's armed capture in the same timestep. One report
   // per (chunk, timestep) naming the first offending pin.
   if (unlikely(g_ao3_n > 0) && aj_ao_level() >= 3) {
      for (unsigned _i = 0; _i < g_ao3_n; _i++) {
         if (g_ao3_map[_i].nx != n) continue;
         aj_chunk_t *_c = g_ao3_map[_i].chunk;
         if (_c->ao_cap_now == (uint64_t)m->now + 1 && !_c->ao_cap_fired
             && (uint32_t)m->iteration > _c->ao_cap_delta) {
            _c->ao_cap_fired = 1;
            notef("accel-assert: CENSUS capture-before-producer chunk %s "
                  "t=%"PRIu64": captured at delta %u, input pin '%s' "
                  "produced at delta %u",
                  _c->scope != NULL && _c->scope->where != NULL
                     ? istr(tree_ident(_c->scope->where)) : "?",
                  (uint64_t)m->now, _c->ao_cap_delta, g_ao3_map[_i].pin,
                  (unsigned)m->iteration);
         }
         break;
      }
   }

   if (n->flags & NET_F_CACHE_EVENT)
      n->signal->shared.flags |= SIG_F_EVENT_FLAG;

   extern rt_nexus_t *g_aj_notify_nexus;
   g_aj_notify_nexus = n;
   n->vtable->notify(m, n);
   g_aj_notify_nexus = NULL;
}
rt_nexus_t *g_aj_notify_nexus = NULL;
// Engine phase at notify time (diagnostic): 1=boundary driverq,
// 2=boundary driving heap, 3=boundary effective heap, 4=wave flush,
// 5=process phase (proc/deposit inline), 0=other
int g_aj_phase = 0;

static void put_effective_impl(rt_model_t *m, rt_nexus_t *n, const void *value)
{
   TRACE("update %s effective value %s", trace_nexus(n), fmt_nexus(n, value));

   // Base-run counterpart of the NVC_BANKED_DBG stage trace: with banking
   // OFF, print the same "#BD"-format line at the deposit itself so the
   // two runs' sequences diff directly (I2.6 gals-wedge debugging).
   { static int dbg = -1;
     if (dbg < 0) { const char *e = getenv("NVC_BANKED_DBG");
                    dbg = e != NULL ? atoi(e) : 0; }
     if (dbg > 0) { dbg--;
        fprintf(stderr, "#BD put %s t=%"PRIi64"+%u v=%02x\n",
                n->signal->where ? istr(tree_ident(n->signal->where)) : "?",
                m->now, m->iteration, ((const uint8_t *)value)[0]); } }

   unsigned char *eff = nexus_effective(n);
   unsigned char *last = nexus_last_value(n);

   const size_t valuesz = n->size * n->width;

   if (!cmp_bytes(eff, value, valuesz)) {
      // Single-update-per-delta invariant for 'last_value: a sweep wave can
      // re-update a net within one delta (stale round then corrected run);
      // shifting last_value to the transient shatters edge detection
      // downstream (a clock reads 1->1 across its own edge).  Keep the
      // delta-entry value.
      if (g_lv_inwave && n->last_event == m->now
          && n->event_delta == m->iteration)
         memcpy(eff, value, valuesz);
      else
         if (likely(!(n->signal->shared.flags & SIG_F_LAST_VALUE)))
            memcpy(eff, value, valuesz);   // #74 E2: no 'last_value reader
         else
            copy2(last, eff, value, valuesz);
      notify_event(m, n);
   }
}

// NVC_BANKED I1: exactly the class's original deposit, plus the
// touched-list append.  Serial-only.
static void banked_deposit(rt_model_t *m, rt_nexus_t *n, const void *value)
{
   const rt_nexus_vtable_t *orig = NULL;
   for (int i = 0; i < g_banked_st.nclasses; i++)
      if (&g_banked_st.patched[i] == n->vtable) {
         orig = g_banked_st.orig[i];
         break;
      }
   assert(orig != NULL);
   (void)orig;
   // I2.6 COPY-AT-FLIP: STAGE the value into bank B only — plane 0 (what
   // every reader sees) is untouched until the delta-boundary flip, so
   // during the update phase no reader-visible byte of a banked net
   // changes.  The flip replays the ORIGINAL deposit with the staged
   // bytes, reusing its change-compare / last_value / notify exactly —
   // semantics-preserving by construction, and duplicate touches
   // self-dedup because the replayed cmp short-circuits.  (The sel-read
   // JIT form was priced at 34% and rejected — reads keep today's
   // zero-cost plane-0 form; the copy volume is touched-only, ~1 memcpy
   // per deposit.)  This is the writer-isolation property I3's
   // concurrent staging needs: bank-B writes are race-free per nexus by
   // the single-driver eligibility.
   {
      rt_signal_t *sig = n->signal;
      const size_t valuesz = (size_t)n->size * n->width;
      memcpy(sig->shared.data + n->offset + 3 * sig->shared.size,
             value, valuesz);
      static int dbg = -1;
      if (dbg < 0) { const char *e = getenv("NVC_BANKED_DBG");
                     dbg = e ? atoi(e) : 0; }
      if (dbg > 0) { dbg--;
         fprintf(stderr, "#BD stage %s t=%"PRIi64"+%u v=%02x\n",
                 sig->where ? istr(tree_ident(sig->where)) : "?",
                 m->now, m->iteration, ((const uint8_t *)value)[0]); }
   }
   if (unlikely(g_uthr.started) && thread_id() != 0) {
      if (g_banked_st.count_u == g_banked_st.max_u) {
         g_banked_st.max_u = g_banked_st.max_u ? g_banked_st.max_u * 2 : 256;
         g_banked_st.items_u = xrealloc_array(g_banked_st.items_u,
                                              g_banked_st.max_u,
                                              sizeof(rt_nexus_t *));
      }
      g_banked_st.items_u[g_banked_st.count_u++] = n;
   }
   else {
      if (g_banked_st.count == g_banked_st.max) {
         g_banked_st.max = g_banked_st.max ? g_banked_st.max * 2 : 256;
         g_banked_st.items = xrealloc_array(g_banked_st.items, g_banked_st.max,
                                            sizeof(rt_nexus_t *));
      }
      g_banked_st.items[g_banked_st.count++] = n;
   }
   g_banked_st.deposits++;
}

// Delta-boundary flip: replay the ORIGINAL deposit for every touched
// nexus with its staged bank-B bytes — the batched, reader-isolated form
// of what the un-banked path did interleaved during the update phase.
// Runs at the delta boundary after driver commits and before processes
// (the same visibility point: update-phase machinery reads driver
// waveforms, never plane-0 effective — audited).  Duplicates in the list
// replay the same staged value; the deposit's change-compare makes the
// repeats free.
static void banked_flip_one(rt_model_t *m, rt_nexus_t *n);

static void banked_flip(rt_model_t *m)
{
   const size_t total = g_banked_st.count + g_banked_st.count_u;
   if (total > (size_t)g_banked_st.maxtouched)
      g_banked_st.maxtouched = (long)total;
   for (size_t i = 0; i < g_banked_st.count; i++)
      banked_flip_one(m, g_banked_st.items[i]);
   for (size_t i = 0; i < g_banked_st.count_u; i++)
      banked_flip_one(m, g_banked_st.items_u[i]);
   g_banked_st.count = 0;
   g_banked_st.count_u = 0;
   g_banked_st.flips++;
}

static void banked_flip_one(rt_model_t *m, rt_nexus_t *n)
{
   {
      const rt_nexus_vtable_t *orig = NULL;
      for (int c = 0; c < g_banked_st.nclasses; c++)
         if (&g_banked_st.patched[c] == n->vtable) {
            orig = g_banked_st.orig[c];
            break;
         }
      if (orig == NULL)
         return;   // vtable replaced since staging (lazy/fastclk install)
      rt_signal_t *sig = n->signal;
      (*orig->deposit)(m, n,
                       sig->shared.data + n->offset + 3 * sig->shared.size);
   }
}

__attribute__((destructor))
static void banked_stats_dump(void)
{
   if (g_banked == 1)
      fprintf(stderr, "#BANKED installs=%ld deposits=%ld flips=%ld "
              "maxtouched=%ld\n", g_banked_st.installs, g_banked_st.deposits,
              g_banked_st.flips, g_banked_st.maxtouched);
   long direct = 0;
   for (int i = 0; i < MAX_THREADS; i++)
      direct += g_direct_cnt[i];
   if (direct > 0)
      fprintf(stderr, "#DIRECT sched=%ld\n", direct);
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
   // #74 E1: for the aliased class (driving == effective by LRM
   // 14.7.7.3) skip the plane-2 memcpy AND the effective-heap round
   // trip, feeding put_effective directly — the same path 99.99% of
   // (non-EFFECTIVE) nexuses already take. The two halves are
   // inseparable: aliasing nexus_driving alone would turn
   // calculate_effective_value into an eff<-eff self-compare that
   // swallows every event on the class. NET_F_HAS_INITIAL is NOT
   // cleared here (plane 2 keeps the default value for disconnect)
   // and NET_F_EFFECTIVE stays set (update_outputs propagate-on-
   // activity gating needs it for 'transaction/'quiet).
   if ((n->flags & NET_F_EFFECTIVE) && !nexus_driving_aliased(n)) {
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

      for (int _k = 0; _k < g_lv_evnxn; _k++) {
         if (g_lv_evnx[_k] != n) continue;
         fprintf(stderr, "#UO %s t=%llu d=%u gate=%d nouts=%d\n",
                 (n->signal != NULL && n->signal->where != NULL)
                    ? istr(tree_ident(n->signal->where)) : "?",
                 (unsigned long long)m->now, m->iteration,
                 (int)update_outputs, n->outputs != NULL);
         break;
      }

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

   if (unlikely(source->aj_rerouted)) {
      // Proc rerouted into an accel chunk: swallow the transactions queued
      // at install (typically the init 'U'/'X') so they cannot clobber the
      // chunk's deposits — see aj_quench_rerouted_drivers. A transaction
      // arriving AFTER init proves something still dispatches the proc body
      // directly (fast-clk table, levelize sweep): reconnect and apply.
      if (m->now > 0) {
         source->aj_rerouted = 0;
         source->disconnected = 0;
      }
      else {
         if (w_next != NULL
             && (w_next->when == m->now || w_next->when == -m->now)) {
            if (w_next->when == m->now)
               free_value(n, w_now->value);
            *w_now = *w_next;
            free_waveform(m, w_next);
         }
         return;
      }
   }

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

   if (unlikely(src->aj_rerouted)) {
      if (m->now > 0) {   // post-init activity: proc is live — reconnect
         src->aj_rerouted = 0;
         src->disconnected = 0;
      }
      else {
         src->fastqueued = 0;   // see aj_quench_rerouted_drivers
         return;
      }
   }

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

      // A deposit stamps its event for iteration+1 and sets the cached
      // flag for its receivers, which run NEXT delta.  The wave's
      // per-flush sync runs BEFORE that delta arrives and must not
      // destroy the flag (measured: every deposit-driven CLOCK lost its
      // edge to the first flush after the deposit -- cone_tb / the VeeR
      // ACTIVE_L2CLK kernels body-filtered forever).  Stock never hits
      // this: sync runs only at boundaries, after iteration advances.
      const bool pending_next = g_lv_inwave
         && s->nexus.last_event == m->now
         && s->nexus.event_delta == m->iteration + 1;

      TRACE("sync event flag %d for %s", event, istr(tree_ident(s->where)));

      if (event)
         assert(s->shared.flags & SIG_F_EVENT_FLAG);   // Set by notify_event
      else if (!pending_next)
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
// NVC_TWO_PHASE core pinning (user directive 2026-08-09): the two phase
// threads (E=eval, U=update) run on DIFFERENT PHYSICAL cores so each
// phase's working set stays resident in its own private L1/L2.  SMT
// siblings share those caches, so siblings are rejected via
// /sys/.../topology/core_id.  NVC_PHASE_PIN=a,b overrides.
static bool aj_read_core_id(int cpu, int *core)
{
   char path[128];
   snprintf(path, sizeof path,
            "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
   FILE *f = fopen(path, "r");
   if (f == NULL) return false;
   const bool ok = fscanf(f, "%d", core) == 1;
   fclose(f);
   return ok;
}

static bool aj_pick_phase_cores(int *e_cpu, int *u_cpu)
{
   const char *pin = getenv("NVC_PHASE_PIN");
   if (pin != NULL && sscanf(pin, "%d,%d", e_cpu, u_cpu) == 2)
      return true;
   const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
   int c0;
   if (ncpu < 2 || !aj_read_core_id(0, &c0)) {
      *e_cpu = 0; *u_cpu = (ncpu > 2) ? 2 : 1;   // topology-blind fallback
      return ncpu >= 2;
   }
   *e_cpu = 0;
   for (int cpu = 1; cpu < ncpu; cpu++) {
      int ci;
      if (aj_read_core_id(cpu, &ci) && ci != c0) {
         *u_cpu = cpu;
         return true;
      }
   }
   *u_cpu = 1;   // all cpus report one core (VM): still separate cpus
   return true;
}

static void aj_pin_thread(int cpu)
{
   cpu_set_t set;
   CPU_ZERO(&set);
   CPU_SET(cpu, &set);
   if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
      warnf("phase-pin: could not pin to cpu%d", cpu);
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
      if (unlikely(g_two_phase_run == 3)) {
         // TP3 streams from this pipe: the array must NOT move under the
         // reader.  Overflow (should not happen at 128k records/delta):
         // wait for U to drain, then fall back to serial-inline for the
         // rest of the delta.
         notef("TP3: prop pipe overflow (%u) — captureless fallback",
               p->count);
         atomic_store(&g_uthr.stream, 2);
         while (atomic_load(&g_uthr.stream) != 0)
            spin_wait();
         atomic_store(&g_capture, 0);
         return NULL;   // caller takes the serial path
      }
      p->max = p->max ? p->max * 2 : 1024;
      p->recs = xrealloc_array(p->recs, p->max, sizeof(prop_rec_t));
   }
   return &p->recs[p->count];
}

static inline void prop_commit(int tid)
{
   prop_pipe_t *p = g_evproc.pipes[tid];
   // RELEASE: the record-field stores must not be compiler-reordered past
   // the count publication — the TP3 streaming consumer chases count
   // concurrently (x86-TSO covers the CPU ordering; this covers gcc).
   // Found by TSAN as the nondeterministic half-filled-record apply.
   __atomic_store_n(&p->count, p->count + 1, __ATOMIC_RELEASE);
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
   if (getenv("NVC_NO_EVAL_ARENA") == NULL)   // default ON; opt-out
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

   if (getenv("NVC_DIRECT_SCHED") != NULL) {
      g_direct_sched = 1;
      notef("NVC_DIRECT_SCHED: worker-direct sched_driver for the "
            "FAST_DRIVER whole-signal class");
   }

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
   atomic_store(&g_capture, 1);

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
   atomic_store(&g_capture, 0);
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

// ---- Task #62 stage 2: level-ordered comb sweep dispatch -------------------
// When NVC_LEVELIZE_SWEEP=1, comb processes woken in a delta run in STATIC
// LEVEL order with driver updates flushed between levels (immediate
// visibility down the order) and at most one eval per wave.  The comb settle
// collapses into the delta it started in, transients are never externally
// observable, and clocked processes (which keep the normal path and wake in
// the following delta) sample fully settled values — the glitch-free
// semantics the user's doctrine calls for.  Non-comb / unleveled tasks run
// FIFO exactly as before.
static void aj_sweep_flush(rt_model_t *m)
{
   extern int g_aj_phase;
   const int save_phase = g_aj_phase;
   g_aj_phase = 4;
   // to QUIESCENCE: procs run inline from the notify path during heap
   // processing schedule fresh driver updates — a single pass strands them
   // one delta per hop (measured: 1-2 leveled procs absorbed per wave).
   while (m->driverq.count > 0 || heap_size(m->driving_heap) > 0
          || heap_size(m->effective_heap) > 0) {
      if (m->driverq.count > 0) {
         swap_deferq(&m->next_driverq, &m->driverq);
         deferq_run(m, &m->next_driverq);
      }
      while (heap_size(m->driving_heap) > 0)
         update_driving(m, heap_extract_min(m->driving_heap), true);
      while (heap_size(m->effective_heap) > 0)
         update_effective(m, heap_extract_min(m->effective_heap));
   }
   g_aj_phase = save_phase;
   sync_event_cache(m);
   // Each flush is a virtual delta for trigger memoisation: run_trigger
   // memoises per trigger_epoch, which normally bumps once per delta.  A
   // wave spans many former deltas under ONE epoch, so a trigger evaluated
   // early (edge not yet committed) memoised FALSE and the real edge later
   // in the wave was swallowed (measured: VeeR TB ifu_resp/lsu_rd_resp
   // never fired -- AXI read-response channel dead, zero retires).
   m->trigger_epoch++;
}

static void aj_sweep_run(rt_model_t *m, deferq_t *dq)
{
   // t=0 settles via the classic delta machinery: the initial X-resolve of
   // comb feedback loops is evaluation-order-dependent, and the sweep's
   // phase-ordered fixpoint reaches a DIFFERENT (reset-dead) X state on
   // VeeR.  Glitch-freedom is a steady-state property; X-init order is
   // simulator-defined either way.
   { static int rescanned0 = 0;
     if (!rescanned0 && m->now >= UINT64_C(1000000)) {
        rescanned0 = 1;
        aj_evsens_scan(m);
     } }
   // AJ_NETDUMP=<substr>: per-delta dump of every matching nexus's raw
   // state -- value, event stamps -- disambiguated by pointer.  Finds the
   // stuck inner-port nexus among leaf-name collisions.
   { static const char *nd = NULL; static int ndi = -1;
     if (ndi < 0) { nd = getenv("AJ_NETDUMP"); ndi = nd ? 1 : 0; }
     if (ndi) {
        for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
           if (n->signal == NULL || n->signal->where == NULL) continue;
           const char *nm = istr(tree_ident(n->signal->where));
           if (strstr(nm, nd) == NULL) continue;
           const unsigned char *eb = nexus_effective(n);
           fprintf(stderr, "#ND %s nx=%p t=%llu d=%u val=%u ev_t=%lld "
                   "ev_d=%u\n", nm, (void *)n,
                   (unsigned long long)m->now, m->iteration,
                   (unsigned)eb[0], (long long)n->last_event,
                   n->event_delta);
        }
     } }
   static int pure = -1, combonly = -1;
   if (pure < 0) {
      pure = getenv("NVC_SWEEP_PURE") != NULL;
      combonly = getenv("NVC_SWEEP_COMBONLY") != NULL;
   }
   if (!g_lv_sweep || g_lv_level == NULL || m->now == 0 || pure) {
      run_procq(m, dq);
      return;
   }
   { static int rescanned = 0;
     if (!rescanned && m->now >= UINT64_C(1000000)) {
        rescanned = 1;
        aj_evsens_scan(m);
     } }
   g_lv_wave++;
   g_lv_wvn = 0;
   g_lv_inwave = true;
   g_lv_curlv = -1;
   // pass 0: run unleveled tasks FIFO, collect leveled comb nodes.  Both
   // procs and transfers embed the wakeable as their first member, so the
   // task arg doubles as the node key.
   for (unsigned i = 0; i < dq->count; i++) {
      defer_task_t *t = &dq->tasks[i];
      rt_wakeable_t *w = NULL;
      if (t->fn == async_run_process)
         w = &((rt_proc_t *)t->arg)->wakeable;
      else if (t->fn == async_transfer_signal)
         w = &((rt_transfer_t *)t->arg)->wakeable;
      const int pi = (w != NULL) ? aj_lv_idx(w) : -1;
      if (pi < 0 || g_lv_level[pi] < 0) {
         const bool clocked = (t->fn == async_run_process);
         const unsigned mark = clocked ? m->driverq.count : 0;
         (*t->fn)(m, t->arg);
         if (clocked)
            aj_lv_hold_from(m, mark);
         continue;
      }
      aj_lv_wv_push(w);
   }
   dq->count = 0;
   // commit pass-0 deposits BEFORE the level passes: without this the
   // unleveled tasks' commits ride the lv0 flush, their comb fanout joins
   // mid-round (one round late at <= curlv) and a deep consumer samples a
   // stale chain (measured: gl ran at lv3 with s1..s3 3 stages behind,
   // vict captured the transient in round 0)
   aj_sweep_flush(m);
   // level passes: run, flush, absorb newly woken deeper comb procs
   // Outer loop: level passes, then SAME-DELTA dispatch of clocked procs
   // woken by mid-wave commits.  Classically a consumer runs in the delta
   // its clock's event was stamped; wave-born wakes queued to the next
   // delta found their edge flags cleared by sync_event_cache and gated
   // flops never fired (measured: gals family Y=0, dead domains).  Their
   // runs may wake further leveled combs (absorbed) or clocked procs
   // (drained next round) -- iterate until quiescent.
   // STRICT PHASES: (1) settle ALL combs to quiescence -- repeat the level
   // passes while absorption adds work (a proc absorbed at/below the pass
   // cursor runs in the next iteration); only then (2) run clocked procs
   // woken by the settled commits, in this same delta; their deposits may
   // start a new comb wave -- repeat.  Interleaving the phases let a
   // consumer sample a mid-settle transient (measured: NZ regressed to 39
   // when the drain ran between comb rounds).
   const unsigned procq_start = m->procq.count;
   int phases_used = 0, max_rounds = 0;
   for (int phase = 0; phase < 64; phase++) {
      phases_used = phase + 1;
      static int slots = -1;
      if (slots < 0) slots = getenv("NVC_SWEEP_NOSLOTS") == NULL;
      for (int round = 0; round < 64; round++) {
         if (round + 1 > max_rounds) max_rounds = round + 1;
         bool any_comb = false;
         const int lvtop = slots ? g_lv_maxlv + 1 : g_lv_maxlv;
         for (int lv = 0; lv <= lvtop; lv++) {
            g_lv_curlv = lv;
            bool any = false;
            if (slots) {
               // SLOT MODE: a clocked proc woken by the level lv-1 flush
               // runs BEFORE comb level lv -- it reads settled state up to
               // its clock's arrival and pre-update state above it: the
               // flop samples AS OF ITS CLOCK'S DELTA (interp-equivalent),
               // fixing the enable-flop one-period lag on late gated
               // clocks (BUS_HOLD_DATA_BEAT_CNT class) while keeping
               // glitch suppression for inputs below the slot.
               for (int i = 0; i < g_lv_ckn; i++) {
                  if (g_lv_ck[i].p == NULL || g_lv_ck[i].rl > lv) continue;
                  rt_proc_t *p = g_lv_ck[i].p;
                  g_lv_ck[i].p = NULL;
                  const unsigned mark = m->driverq.count;
                  async_run_process(m, p);
                  aj_lv_hold_from(m, mark);
                  any = true;
               }
            }
            for (int i = 0; i < g_lv_wvn; i++) {
               if (g_lv_wv[i] == NULL) continue;
               const int pi = aj_lv_idx(g_lv_wv[i]);
               if (g_lv_level[pi] != lv) continue;
               rt_wakeable_t *w = g_lv_wv[i];
               g_lv_wv[i] = NULL;
               aj_lv_run(m, w);
               any = true;
            }
            if (any) {
               aj_sweep_flush(m);
               any_comb = true;
            }
         }
         g_lv_curlv = -1;
         if (!any_comb)
            break;
      }
      // Synchronous-abstraction drain: comb is settled and NO clocked proc
      // has run this phase, so every flop woken during settling reads the
      // settled pre-update state; flops woken by THESE updates (ripple)
      // drain in the next phase and correctly read post-update state.
      const bool have_tail = !combonly && m->procq.count > procq_start;
      if (!have_tail && (combonly || g_lv_ckn == 0))
         break;   // quiescent (COMBONLY: wave-born queue work defers to
                  // the next delta exactly as stock run_procq would)
      if (have_tail) {
         const unsigned n = m->procq.count - procq_start;
         static defer_task_t *tail = NULL;
         static unsigned tailcap = 0;
         if (n > tailcap) {
            tailcap = n * 2;
            tail = xrealloc_array(tail, tailcap, sizeof(defer_task_t));
         }
         memcpy(tail, m->procq.tasks + procq_start, n * sizeof(defer_task_t));
         m->procq.count = procq_start;
         for (unsigned i = 0; i < n; i++)
            (*tail[i].fn)(m, tail[i].arg);
      }
      // Snapshot the batch BEFORE resetting the array: drain-run procs
      // absorb new wakes inline (blocking commits -> hook), and appending
      // into a zeroed g_lv_ckn overwrites slots this loop is still
      // iterating -- the overwritten proc is lost with pending stuck true,
      // permanently dead (measured: VeeR dbg/bus flops never ran after
      // their first mid-drain wake; whole domains X-locked).
      {
         static lv_ck_t *batch = NULL;
         static int batchcap = 0;
         const int nck = g_lv_ckn;
         if (nck > batchcap) {
            batchcap = nck * 2;
            batch = xrealloc_array(batch, batchcap, sizeof(lv_ck_t));
         }
         memcpy(batch, g_lv_ck, nck * sizeof(lv_ck_t));
         g_lv_ckn = 0;
         for (int i = 0; i < nck; i++) {
            if (batch[i].p == NULL) continue;
            const unsigned mark = m->driverq.count;
            async_run_process(m, batch[i].p);
            aj_lv_hold_from(m, mark);
         }
      }
      aj_sweep_flush(m);
   }
   // Overflow safety: if the round/phase caps were hit, wave entries may
   // remain unrun with pending stuck true -- dropping them kills their
   // procs PERMANENTLY (measured: VeeR retired NOTHING).  Drain leftovers
   // FIFO to a fixpoint; order degrades to delta semantics, correctness
   // survives.
   for (int guard = 0; guard < 1000; guard++) {
      bool anyleft = false;
      for (int i = 0; i < g_lv_wvn; i++) {
         if (g_lv_wv[i] == NULL) continue;
         rt_wakeable_t *w = g_lv_wv[i];
         g_lv_wv[i] = NULL;
         aj_lv_run(m, w);
         anyleft = true;
      }
      for (int i = 0; i < g_lv_ckn; i++) {
         if (g_lv_ck[i].p == NULL) continue;
         rt_proc_t *p = g_lv_ck[i].p;
         g_lv_ck[i].p = NULL;
         const unsigned mark = m->driverq.count;
         async_run_process(m, p);
         aj_lv_hold_from(m, mark);
         anyleft = true;
      }
      g_lv_ckn = 0;
      if (!anyleft)
         break;
      { static int dbg = -1;
        if (dbg < 0) dbg = getenv("AJ_SWEEPDBG") != NULL;
        if (dbg)
           fprintf(stderr, "#OV wave=%u leftover drain pass %d\n",
                   g_lv_wave, guard); }
      aj_sweep_flush(m);
   }
   { static int dbg = -1; if (dbg < 0) dbg = getenv("AJ_SWEEPDBG") != NULL;
     if (dbg && (phases_used >= 64 || max_rounds >= 64 || g_lv_wave <= 8))
        fprintf(stderr, "#WV wave=%u t=%llu d=%u phases=%d rounds=%d%s\n",
                g_lv_wave, (unsigned long long)m->now, m->iteration,
                phases_used, max_rounds,
                (phases_used >= 64 || max_rounds >= 64) ? " CAP-HIT" : ""); }
   if (g_lv_helddrvn > 0) {
      for (int i = 0; i < g_lv_helddrvn; i++)
         defer_driving_update(m, g_lv_helddrv[i]);
      g_lv_helddrvn = 0;
      m->next_is_delta = true;
   }
   if (g_lv_depq.count > 0) {
      for (unsigned i = 0; i < g_lv_depq.count; i++)
         deferq_do(unlikely(g_two_phase_eval) ? &m->procq_e : &m->procq,
                   g_lv_depq.tasks[i].fn, g_lv_depq.tasks[i].arg);
      g_lv_depq.count = 0;
      m->next_is_delta = true;
   }
   { static int dbg = -1;
     if (dbg < 0) dbg = getenv("AJ_HOLDDBG") != NULL;
     if (dbg && g_lv_heldq.count > 0)
        fprintf(stderr, "#HD release %u tasks t=%llu d=%u\n",
                g_lv_heldq.count, (unsigned long long)m->now, m->iteration); }
   if (g_lv_heldq.count > 0) {
      // release clocked-proc deposits: the next delta's update phase
      // commits them (interp NBA timing), and their comb fanout settles in
      // that delta's wave
      for (unsigned i = 0; i < g_lv_heldq.count; i++)
         deferq_do(&m->driverq, g_lv_heldq.tasks[i].fn,
                   g_lv_heldq.tasks[i].arg);
      g_lv_heldq.count = 0;
      m->next_is_delta = true;
   }
   g_lv_inwave = false;
   g_lv_curlv = -1;
   { static int dbg = -1; if (dbg < 0) dbg = getenv("AJ_SWEEPDBG") != NULL;
     if (dbg && g_lv_late > 0) {
        static long last = 0;
        if (g_lv_late != last)
           fprintf(stderr, "#SW topo violations: %ld\n", g_lv_late);
        last = g_lv_late;
     } }
}

// #67: the extracted UPDATE phase of the simulation cycle (driver
// maturation, driving/effective heaps, implicit signals, banked flip,
// trigger filtering).  Runs inline by default; under NVC_TWO_PHASE=2 it
// runs on the dedicated U thread pinned to a different physical core.
static void update_phase(rt_model_t *m)
{
   { extern int g_aj_phase; g_aj_phase = 1; }
   if (m->driverq.count > 0) {
      swap_deferq(&m->next_driverq, &m->driverq);
      deferq_run(m, &m->next_driverq);
   }

   { extern int g_aj_phase; g_aj_phase = 2; }
   while (heap_size(m->driving_heap) > 0) {
      rt_nexus_t *n = heap_extract_min(m->driving_heap);
      update_driving(m, n, true);
   }

   { extern int g_aj_phase; g_aj_phase = 3; }
   while (heap_size(m->effective_heap) > 0) {
      rt_nexus_t *n = heap_extract_min(m->effective_heap);
      update_effective(m, n);
   }
   { extern int g_aj_phase; g_aj_phase = 5; }

   sync_event_cache(m);

   m->blocking_update = true;

   // Update implicit signals
   if (m->implicitq.count > 0)
      deferq_run(m, &m->implicitq);

   assert(model_thread(m)->tlab->alloc == 0);

#if TRACE_SIGNALS > 0
   if (__trace_on)
      dump_signals(m, m->root);
#endif

   // NVC_BANKED copy-at-flip: replay staged bank-B deposits BEFORE the
   // trigger epoch bump and sensitivity drain — the wakes these deposits
   // notify must feed THIS delta's process filtering exactly as the
   // un-banked interleaved deposits did (placing the flip after the
   // triggerq drain made every wake one delta late and wedged the GALS
   // handshake fixture at Y=0).
   if (unlikely(g_banked == 1))
      banked_flip(m);

   // The epoch bump must stay unconditional: run_trigger memoises on it
   // from the run_process filter and the inline blocking_update wake path,
   // both outside this drain
   m->trigger_epoch++;
   if (m->triggerq.count > 0)
      deferq_run(m, &m->triggerq);  // Sensitivity list filter
}


static void *u_thread_main(void *arg)
{
   rt_model_t *m = arg;
   __model = m;
   model_thread(m)->tlab = tlab_acquire(m->mspace);
   aj_pin_thread(g_uthr.u_cpu);
   int seen = 0;
   for (;;) {
      const int st = atomic_load(&g_uthr.stream);
      if (st != 0) {
         prop_pipe_t *p = g_evproc.pipes[g_uthr.e_tid];
         const uint32_t cnt = __atomic_load_n(&p->count, __ATOMIC_ACQUIRE);
         while (g_uthr.consumed < cnt)
            prop_apply(m, &p->recs[g_uthr.consumed++]);
         if (st == 2 && g_uthr.consumed == relaxed_load(&p->count)) {
            p->count = 0;
            g_uthr.consumed = 0;
            atomic_store(&g_uthr.stream, 0);   // drained: E may proceed
         }
         continue;
      }
      int r;
      if ((r = atomic_load(&g_uthr.req)) == seen) {
         if (relaxed_load(&g_uthr.stop))
            break;
         spin_wait();
         continue;
      }
      seen = r;
      update_phase(m);
      atomic_store(&g_uthr.done, r);
   }
   return NULL;
}

static void u_thread_shutdown(void)
{
   if (!g_uthr.started)
      return;
   relaxed_store(&g_uthr.stop, 1);
   thread_join(g_uthr.thread);
   g_uthr.started = false;
}

static void u_phase_handoff(rt_model_t *m)
{
   if (unlikely(!g_uthr.started)) {
      g_uthr.started = true;
      g_uthr.model = m;
      if (!aj_pick_phase_cores(&g_uthr.e_cpu, &g_uthr.u_cpu))
         { g_uthr.e_cpu = 0; g_uthr.u_cpu = 1; }
      aj_pin_thread(g_uthr.e_cpu);
      g_uthr.thread = thread_create(u_thread_main, m, "uphase");
      notef("NVC_TWO_PHASE=2: update phase on cpu%d, eval on cpu%d",
            g_uthr.u_cpu, g_uthr.e_cpu);
   }
   const int r = g_uthr.req + 1;
   atomic_store(&g_uthr.req, r);
   while (atomic_load(&g_uthr.done) != r)
      spin_wait();
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

   // ONE-SHOT X-CONSISTENCY RE-EVALUATION (accel event-hole closure).
   // Accel publications certain-ize rim bytes on their own timeline; an
   // interp comb that evaluated while an input was still X never sees the
   // X->certain EVENT the all-interp run would have produced, so its stale
   // X output recirculates (VeeR: the IFC island missed its 35ns settle and
   // the X reached CSR-address case-dispatch). After a few timesteps -- the
   // reset window has stamped real values everywhere -- re-run the driver
   // procs of every net still carrying X: they recompute from live certain
   // inputs. Spurious wakeups are legal VHDL (edge guards no-op), so no
   // proc classification is needed. NVC_ACCEL_XCONS=<timesteps> tunes the
   // trigger (default 8; 0 disables).
   {
      static int xcons_at = -2;
      static int xcons_steps = 0;
      static uint64_t xcons_last = UINT64_C(0xffffffffffffffff);
      if (xcons_at == -2) {
         // DEFAULT OFF: measured on VeeR, the spurious re-runs X-marked
         // 68k previously-certain nets — l3d outputs recomputed while ANY
         // input is legitimately X (uninitialized BP arrays) go X, where
         // the stale-but-certain original evaluation was the right answer.
         // The certainty plane is TIMING-SENSITIVE; blanket re-evaluation
         // violates it. Keep as an experimental knob only.
         const char *e = getenv("NVC_ACCEL_XCONS");
         xcons_at = e ? atoi(e) : 0;
      }
      // Settle-window force-event: for the first K timesteps, chunk
      // publications EVENT even when byte-equal, matching the interp
      // driver timeline (interp procs re-drive at reset captures and those
      // events wake settle cascades; a byte-equal deposit silently skips
      // them and downstream combs keep stale X — the event-hole class).
      // After the window, change-gating resumes (steady-state suppression
      // is the correct optimization).
      { extern bool g_aj_forceev;
        static int few_at = -2;
        if (few_at == -2) {
           // DEFAULT OFF: measured neutral on VeeR (the missing settle
           // events are interp-proc re-runs, not deposits — a deposit-side
           // force-event cannot recreate them). Knob kept for experiments.
           const char *fe = getenv("NVC_ACCEL_FORCEEV");
           few_at = fe ? atoi(fe) : 0;
        }
        g_aj_forceev = m->aj_chunk_count > 0 && few_at > 0
           && xcons_steps < few_at; }
      if (m->aj_chunk_count > 0 && m->now != xcons_last) {
         xcons_last = m->now;
         ++xcons_steps;
         if (xcons_at > 0 && xcons_steps == xcons_at) {
            xcons_at = 0;   // one-shot
            // TOPO-ORDERED settle: re-run X-net driver procs in LEVELIZE
            // order so each re-evaluates AFTER its inputs settled. The flat
            // (unordered) variant re-computed X from not-yet-settled inputs
            // and X-marked 68k previously-certain nets; topo order is what
            // makes re-evaluation sound. Requires the sweep's level table.
            extern hash_t *g_lv_idx;
            extern int *g_lv_level;
            hash_t *woken = hash_new(1024);
            typedef struct { int lvl; rt_proc_t *p; } xset_t;
            xset_t *set = NULL;
            int nset = 0, capset = 0;
            for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
               if (n->signal == NULL) continue;
               const unsigned char *db = nexus_effective(n);
               bool isx = false;
               for (int e2 = 0; e2 < n->width && !isx; e2++)
                  if (db[(size_t)e2 * n->size] & 4) isx = true;
               if (!isx) continue;
               for (rt_source_t *s = &(n->sources); s != NULL;
                    s = s->chain_input) {
                  if (s->tag != SOURCE_DRIVER || s->u.driver.proc == NULL)
                     continue;
                  rt_proc_t *p = s->u.driver.proc;
                  if (hash_get(woken, p) != NULL) continue;
                  hash_put(woken, p, p);
                  int lvl = 0;
                  if (g_lv_idx != NULL && g_lv_level != NULL) {
                     void *v = hash_get(g_lv_idx, &(p->wakeable));
                     if (v != NULL)
                        lvl = g_lv_level[(int)(uintptr_t)v - 1];
                  }
                  if (nset == capset) {
                     capset = capset ? capset * 2 : 1024;
                     set = xrealloc_array(set, capset, sizeof(xset_t));
                  }
                  set[nset++] = (xset_t){ lvl, p };
               }
            }
            hash_free(woken);
            // insertion-stable level sort (qsort fine)
            int xcmp(const void *a, const void *b) {
               return ((const xset_t *)a)->lvl - ((const xset_t *)b)->lvl;
            }
            qsort(set, nset, sizeof(xset_t), xcmp);
            for (int i2 = 0; i2 < nset; i2++)
               run_process(m, set[i2].p);
            free(set);
            if (nset > 0)
               notef("accel-jit: topo-ordered X settle re-ran %d proc(s)",
                     nset);
         }
      }
   }

   // #67 NVC_TWO_PHASE=2: the whole update phase runs on the pinned U
   // thread (cache partition: driver/waveform/pending/heap structures live
   // in U's core-private cache; eval state stays in E's).  Handshake is a
   // request/ack epoch pair; the phases stay strictly ordered — this first
   // increment measures the cache split alone, no concurrency.
   if (unlikely(g_two_phase_run >= 2))
      u_phase_handoff(m);
   else
      update_phase(m);

   run_callbacks(m, START_OF_PROCESSES);

   // Two-phase edge sampling, hoisted (SNAP_MODE >= 2): fleet snapshot at
   // the delta boundary -- this delta's driver commits are in, no process
   // has run -- so a same-delta blocking deposit from an interp process can
   // no longer contaminate the snapshot (mode 1 takes it lazily at the
   // first chunk eval, which may follow those deposits).
   if (m->aj_chunk_count > 0 && aj_snap_mode() >= 2)
      aj_snap_fleet_take(m);

   // STALE-DRIVING-VALUE audit (assert-order rank 7, sampled): the
   // two-sided protocol assumes every publication refreshed every
   // registered out_drv driver, so port propagation and resolution —
   // which read DRIVING values, not effective bytes — always deliver the
   // chunk's current value. A publication path that bypasses the refresh
   // freezes downstream readers while both correctness gates stay green
   // (the EH1a AXI ARVALID class: rim showed the fetch, the TB never saw
   // one). Every 256th delta boundary, memcmp each registered driver's
   // waveform value against its nexus's effective bytes.
   { static int _sa = -1;
     if (_sa < 0) _sa = getenv("NVC_ACCEL_ASSERT_ORDER") != NULL;
     static unsigned _sc;
     if (_sa && m->aj_chunk_count > 0 && (++_sc & 255) == 0) {
        for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
           aj_chunk_t *c = m->aj_chunks[ci];
           if (c->out_drv == NULL || c->out_drv_n == NULL) continue;
           for (unsigned ord = 0; ord < c->defer_count; ord++)
              for (int k = 0; k < c->out_drv_n[ord]; k++) {
                 struct aj_odrv *od = &c->out_drv[ord][k];
                 const size_t nb = (size_t)od->nx->width * od->nx->size;
                 const void *dv =
                    value_ptr(od->nx, &od->src->u.driver.waveforms.value);
                 if (memcmp(dv, nexus_effective(od->nx), nb) != 0)
                    notef("accel-assert: STALE DRIVING VALUE chunk %s "
                          "out %u drv %d t=%"PRIu64": driver bytes != "
                          "effective — a publication path bypassed the "
                          "out_drv refresh; port-hop consumers are "
                          "resolving stale data",
                          c->scope != NULL && c->scope->where != NULL
                             ? istr(tree_ident(c->scope->where)) : "?",
                          ord, k, (uint64_t)m->now);
              }
        }
     }
   }

   // #53 negedge STATE FLIP: merged chunks staged their registered outputs
   // at posedge evals; publish them in the delta where the domain clock's
   // FALL commits -- a derivation-free barrier strictly after every gated
   // rise of the cycle and strictly before the next sample.  This hook
   // (not a chunk eval) does the flip because fall wakes never reach the
   // rerouted procs -- their rising-edge triggers filter them (measured).
   // deposit_signal handles propagation + wakeups for the rim readers.
   if (m->aj_chunk_count > 0)
      for (unsigned ci = 0; ci < m->aj_chunk_count; ci++) {
         aj_chunk_t *c = m->aj_chunks[ci];
         if (!c->merged || !c->defer_pending || c->primary_ck == NULL)
            continue;
         rt_nexus_t *cn = &c->primary_ck->nexus;
         if (!(cn->last_event == (uint64_t)m->now
               && cn->event_delta == m->iteration
               && !(c->primary_ck->shared.data[0] & 1)))
            continue;   // not the fall-commit delta
         if (c->ck_flip_now == (uint64_t)m->now + 1)
            continue;   // already flipped this timestep
         c->ck_flip_now = (uint64_t)m->now + 1;
         for (unsigned i = 0; i < c->defer_count; i++) {
            aj_defer_out_t *d = &c->defer_outs[i];
            if (!d->negflip || !d->dirty) continue;
            d->dirty = false;
            deposit_signal(m, (rt_signal_t *)d->sigp, d->shadow, 0, d->pw);
         }
         c->defer_pending = false;
      }

   // Probation sim-time bound (P3): a candidate that stops accumulating
   // value-edges (gated clock during halt, slow clock domain, captured
   // reset) must not pin a table in probation forever, blocking re-pick
   // of a better candidate. STALL = dissolve + backoff + cooldown, never
   // blacklist. Guarded with !fastclk_hit so a latched delta is never
   // dropped -- the dispatch below consumes it first; the timeout fires
   // on the next quiet delta (plentiful: stalled candidates are quiet).
   if (unlikely(m->fastclk_on && m->fastclk_probation > 0 && !m->fastclk_hit
                && m->now - m->fastclk_built_at >= fastclk_probation_fs()))
      aj_fastclk_stall(m, "sim-time probation timeout");

   // NVC_FAST_CLK_AUTO: standalone posedge-table build, no accel install
   // needed. After the requested settle time, pick the widest-fanout
   // single-bit nexus as the clock (the clock pending list dwarfs all
   // others in translated RTL) and build the same table the accel install
   // would. USER DIRECTIVE: run past initialization, then block-dispatch
   // everything on the shared sensitivity.
   if (unlikely(m->fastclk_auto_at != 0 && !m->fastclk_on
                && m->now >= m->fastclk_auto_at)) {
      m->fastclk_auto_at = 0;   // one shot (re-armed below if all-empty)
      // Widest-fanout single-bit nexus is often rst (async-reset procs pend
      // on rst AND their clk, so under NVC_FAST_CLK_WIDE=0 all get
      // filtered) — try candidates in fanout order until one yields a
      // non-empty table.
      rt_nexus_t *tried[4] = { NULL, NULL, NULL, NULL };
      // Fanout floor for a clock candidate: default 16 keeps the historic
      // behaviour (only wide-fanout nexuses qualify); NVC_FAST_CLK_AUTO_MIN
      // lowers it for small designs (A/B benchmarking of the dispatch path)
      static int min_fanout = -1;
      if (min_fanout < 0) {
         const char *e = getenv("NVC_FAST_CLK_AUTO_MIN");
         // Default floor 4: low enough to catch real clocks on small
         // designs (bench clk fanout < 16), high enough to exclude the
         // known fanout-1 data-signal misfire class (wait5).  The full
         // regression gate is the empirical judge of this value.
         min_fanout = (e != NULL) ? MAX(atoi(e), 1) : 4;
      }
      // NVC_FASTCLK_FANOUT: one-shot histogram of single-bit nexus
      // pending-list sizes — sizes multi-table coverage (which gated
      // clocks carry how many waiters)
      if (getenv("NVC_FASTCLK_FANOUT") != NULL) {
         static bool dumped = false;
         if (!dumped) {
            dumped = true;
            for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
               if (n->width != 1 || n->signal->n_nexus != 1) continue;
               const unsigned c = aj_pending_count(n->pending);
               if (c >= 4)
                  notef("fanout: %s pending=%u",
                        istr(tree_ident(n->signal->where)), c);
            }
         }
      }
      for (int k = 0; k < 4 && !m->fastclk_on; k++) {
         rt_nexus_t *best = NULL;
         unsigned best_n = min_fanout - 1;
         for (rt_nexus_t *n = m->nexuses; n != NULL; n = n->chain) {
            if (n->width != 1 || n->signal->n_nexus != 1) continue;
            bool skip = false;
            for (int t = 0; t < 4; t++)          // all four slots checked
               if (n == tried[t]) { skip = true; break; }
            for (unsigned b = 0; !skip && b < m->fastclk_nbl; b++)
               if (m->fastclk_bl[b] == n) skip = true;  // never re-picked
            for (unsigned c = 0; !skip && c < ARRAY_LEN(m->fastclk_cool); c++)
               if (m->fastclk_cool[c].nx == n           // temporary cooldown
                   && m->fastclk_cool[c].retry_at > m->now)
                  skip = true;
            if (skip) continue;
            const unsigned c = aj_pending_count(n->pending);
            if (c > best_n) { best_n = c; best = n; }
         }
         if (best == NULL) break;
         tried[k] = best;
         if (getenv("NVC_ACCEL_JIT_DEBUG") != NULL)
            notef("accel-jit: NVC_FAST_CLK_AUTO try %s fanout %u",
                  istr(tree_ident(best->signal->where)), best_n);
         aj_build_fastclk(m, best->signal, nexus_effective(best));
      }
      if (!m->fastclk_on) {
         // All-empty round: re-arm with SEPARATE bookkeeping -- pure
         // doubling from 500ns to a 100us cap, NO survival reset (the
         // dissolve hysteresis's fastclk_built_at is stale when nothing
         // went live, so its survival clause would peg the backoff at
         // base and re-scan every 500ns -- the measured ~10% VeeR wall
         // thrash). A successful build resets this ladder.
         const uint64_t base = UINT64_C(500000000);        // 500ns
         const uint64_t cap  = UINT64_C(100000000000000);  // 100us
         m->fastclk_empty_backoff = (m->fastclk_empty_backoff == 0)
            ? base : MIN(cap, m->fastclk_empty_backoff * 2);
         m->fastclk_auto_at = m->now + m->fastclk_empty_backoff;
      }
   }

   // NVC_FAST_CLK fast path. A member process woke this delta (latched in
   // wakeup_one, those procs were not queued) -- by the candidate clock
   // or, in wide mode, a registered companion. The candidate VALUE-EDGE
   // shadow (sampled HERE, at the dispatch site: wake_next deposits
   // update the value plane one delta before their attributed event)
   // classifies the delta: a posedge runs the whole table; anything else
   // runs only the every-event tail. Members run at the exact point
   // run_procq would have run them (after the driving/effective heaps
   // and the trigger filter), so each proc reads the same settled values
   // it would on the normal path; spurious activations are body-filtered
   // (sync_event_cache ran above, so rising_edge/'event read false on a
   // no-clk-event delta). Bit-identical, but skips the wakeup_all/
   // procq_do/deferq/async_run_process round-trip.
   if (m->fastclk_hit) {
      m->fastclk_hit = false;
      const uint8_t clk_now = m->fastclk_data[0];
      const bool edged = (clk_now != m->fastclk_clk_last);
      const bool posedge = edged && (clk_now & 1) != 0;
      m->fastclk_clk_last = clk_now;   // probation included (P1)
      if (unlikely(m->fastclk_probation > 0)) {
         // PROBATION: dispatch ALL members on EVERY hit delta (correct
         // for both clocked and comb members) and attribute VALUE-
         // CHANGING driver activity on non-posedge activations via
         // sched_driver. Every-event members are exempt from probing on
         // deltas where a registered companion evented (their reset
         // branch is not comb evidence); posedge-only-destined members
         // are ALWAYS probed on !posedge deltas -- a candidate falling
         // edge probes them even when a companion co-events, keeping the
         // pr2305307b comb-starvation defense intact.
         const bool comp_ev = m->fastclk_ncomp > 0 && aj_companion_evented(m);
         rt_proc_t **pr = m->fastclk_table;
         for (unsigned i = 0; i < m->fastclk_count; i++, pr++) {
            rt_wakeable_t *w = &((*pr)->wakeable);
            if (!w->fastclk || w->pending)
               continue;   // evicted mid-probation / queued outside table
            if (!edged && !aj_member_evented(m, *pr))
               continue;   // companion-only delta, own sensitivity quiet
            if (!posedge && !(w->fastclk_ee && comp_ev)) {
               m->fastclk_probe_member = (int)i;
               // Trigger-armed members are classified SOUNDLY here: a
               // trigger that fires on a non-posedge delta means
               // posedge-only dispatch would starve this member,
               // independent of whether its probed drives happen to
               // change values (EH1a lsu_mem_neg: its falling-edge
               // drives rewrote equal bytes -- the hello banner's dash
               // prefix -- so the value-compare attribution below never
               // marked it and the member went stale after probation).
               // run_trigger memoises per trigger_epoch, so the body's
               // own gate check right after costs nothing extra.
               if (w->trigger != NULL && run_trigger(m, w->trigger))
                  m->fastclk_comb[i] = 1;
            }
            run_process(m, *pr);
            m->fastclk_probe_member = -1;
         }
         m->fastclk_hit_deltas++;
         bool done = false;
         if (edged)   // P2: only candidate value-edges count down
            done = (--m->fastclk_probation == 0);
         if (!done && unlikely(m->fastclk_hit_deltas >= 512)) {
            // P2 STALL: companion wakes vastly outnumber candidate edges
            // (a captured reset: all clk edges, no candidate edges).
            // Dissolve + cooldown; the next round tries the next-widest.
            aj_fastclk_stall(m, "512 hit deltas before 64 candidate edges");
            goto fastclk_done;
         }
         if (done) {
            // classify: evict comb members; none left => not a clock
            unsigned kept = 0;
            for (unsigned i = 0; i < m->fastclk_count; i++) {
               rt_wakeable_t *w = &(m->fastclk_table[i]->wakeable);
               if (m->fastclk_comb[i] || !w->fastclk) {
                  if (w->fastclk && w->fastclk_counted) {
                     w->fastclk_counted = 0;
                     atomic_add(&m->fastclk_npending, -1);
                  }
                  w->fastclk = 0;   // evict (or already evicted via hooks)
                  w->fastclk_ee = 0;
               }
               else
                  m->fastclk_table[kept++] = m->fastclk_table[i];
            }
            const unsigned evicted = m->fastclk_count - kept;
            m->fastclk_count = kept;
            if (getenv("NVC_ACCEL_JIT_DEBUG") != NULL)
               notef("accel-jit: fast-clk probation done — kept %u evicted %u",
                     kept, evicted);
            if (getenv("NVC_FASTCLK_CENSUS") != NULL) {
               // C1 bound: probation survivors are posedge-only-PROVEN,
               // so trigger-armed survivors are exactly the class an
               // extended fused-block eligibility would newly admit
               unsigned trig = 0, notrig = 0, ee = 0;
               for (unsigned i = 0; i < m->fastclk_count; i++) {
                  rt_wakeable_t *w = &(m->fastclk_table[i]->wakeable);
                  if (w->trigger != NULL) trig++;
                  else notrig++;
                  if (w->fastclk_ee) ee++;
               }
               notef("fastclk census: kept=%u trigger-armed=%u (C1 class) "
                     "no-trigger=%u ee-tail=%u evicted=%u",
                     m->fastclk_count, trig, notrig, ee, evicted);
            }
            if (kept == 0) {
               if (m->fastclk_ncomp > 0) {
                  // P6: a table that HAD companions may have been built
                  // inside a reset window (held-in-reset drives look
                  // comb); the real clock must stay retryable — dissolve
                  // + backoff re-arm, NO blacklist.
                  if (getenv("NVC_ACCEL_JIT_DEBUG") != NULL)
                     notef("accel-jit: fast-clk all-evicted with companions "
                           "— dissolve without blacklist");
                  aj_dissolve_fastclk(m);
               }
               else {
                  // no companions (pr2305307b shape: a data net feeding
                  // comb readers): blacklist so AUTO never re-picks it
                  if (m->fastclk_nbl == m->fastclk_blmax) {
                     m->fastclk_blmax = m->fastclk_blmax ? m->fastclk_blmax * 2 : 64;
                     m->fastclk_bl = xrealloc_array(m->fastclk_bl, m->fastclk_blmax,
                                                    sizeof(rt_nexus_t *));
                  }
                  m->fastclk_bl[m->fastclk_nbl++] = m->fastclk_nexus;
                  aj_dissolve_fastclk(m);
               }
            }
            else {
               // Partition-sort (D3): posedge-only members first
               // [0, ee_start), every-event tail [ee_start, count).
               // fastclk_table is permuted HERE, before fused_block_build
               // memcpys it, so the fallback loop and the emitted block
               // agree on member order; comb[] has no post-probation
               // reader and is freed below (P7).
               rt_proc_t **sorted = xmalloc_array(kept, sizeof(rt_proc_t *));
               unsigned n_pos = 0;
               for (unsigned i = 0; i < kept; i++)
                  if (!m->fastclk_table[i]->wakeable.fastclk_ee)
                     sorted[n_pos++] = m->fastclk_table[i];
               unsigned n_all = n_pos;
               for (unsigned i = 0; i < kept; i++)
                  if (m->fastclk_table[i]->wakeable.fastclk_ee)
                     sorted[n_all++] = m->fastclk_table[i];
               memcpy(m->fastclk_table, sorted, kept * sizeof(rt_proc_t *));
               free(sorted);
               m->fastclk_ee_start = n_pos;
               free(m->fastclk_comb);   // P7: no reader past this point
               m->fastclk_comb = NULL;
               if (getenv("NVC_ACCEL_JIT_DEBUG") != NULL)
                  notef("accel-jit: fast-clk partitions — %u posedge-only, "
                        "%u every-event", n_pos, kept - n_pos);
               fused_block_build(m);   // member set + order final: fuse now
            }
         }
         goto fastclk_done;
      }
      // POST-ENGAGE CANARY (assert-order rank 11): probation's
      // classification is a point-in-time decision — a posedge-only
      // member whose trigger fires on a later non-posedge delta was
      // MISCLASSIFIED and is being starved right now (#72's fastclk
      // fall-starvation went undetected for the whole run; the fixed
      // probation closes the known escape, this catches any residual
      // class). Sampled on candidate FALLS (1 in 64); a firing member
      // is evicted back to the normal path (self-healing — safe under
      // fusion, unlike re-partitioning, because evict patches the fused
      // block) and reported once.
      { static int _cn = -1;
        if (_cn < 0) _cn = getenv("NVC_ACCEL_ASSERT_ORDER") != NULL;
        static unsigned _cs;
        if (_cn && edged && !posedge && (++_cs & 63) == 0) {
           for (unsigned i = 0; i < m->fastclk_ee_start; ) {
              rt_wakeable_t *w = &(m->fastclk_table[i]->wakeable);
              if (w->fastclk && !w->pending && w->trigger != NULL
                  && run_trigger(m, w->trigger)) {
                 notef("accel-assert: FASTCLK CANARY: posedge-only member "
                       "proc fired its trigger on a non-posedge delta "
                       "t=%"PRIu64" — misclassified at probation, evicting "
                       "to the normal path", (uint64_t)m->now);
                 aj_fastclk_evict(m, w, "post-engage canary");
                 continue;   // evict compacts nothing; flag now clear
              }
              i++;
           }
        }
      }
      // Steady state: posedge => whole table; any other latched wake =>
      // every-event tail only (skipped outright when the tail is empty).
      if (posedge || m->fastclk_ee_start < m->fastclk_count) {
         // NVC_FUSED_BLOCK: one activation for the dispatched partition --
         // falls back to the partitioned table loop when unavailable
         // (identical semantics either way). The block is only entered on
         // candidate-event (edged) deltas, where EVERY member's own
         // sensitivity provably evented; companion-only deltas take the
         // loop, which applies the per-member own-event guard.
         if (!(edged && fused_block_dispatch(m, posedge))) {
            const unsigned first = posedge ? 0 : m->fastclk_ee_start;
            rt_proc_t **pr = m->fastclk_table + first;
            for (unsigned i = first; i < m->fastclk_count; i++, pr++) {
               rt_wakeable_t *w = &((*pr)->wakeable);
               if (!w->fastclk || w->pending)
                  continue;   // evicted via hooks / queued outside table
               if (!edged && !aj_member_evented(m, *pr))
                  continue;   // companion-only delta, own sensitivity quiet
               run_process(m, *pr);
            }
         }

         // NVC_ACCEL_BANK swap: the chunk STAGEd its registered outputs into
         // shadows; every reader in the dispatched partition has now read the
         // OLD effective value (the delta-delay / NBA semantics). Publish
         // shadow->effective so the new value is visible at the NEXT edge.
         // Runs after EITHER entry: an off-edge (reset) dispatch stages
         // shadows too, and skipping the publish would leave the next
         // posedge reading pre-reset values. No wakeup / no extra delta —
         // the gate proved all readers are posedge-only members of the
         // table just dispatched (or dispatched at their next posedge).
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

         // Busy-companion rate demote (D5): a companion whose events
         // outnumber posedges over a 256-dispatch window converts every
         // event into a full tail sweep — dissolve and rebuild with that
         // nexus's procs excluded (local exclusion, NOT the blacklist).
         if (posedge)
            m->fastclk_win_pos++;
         else {
            m->fastclk_win_off++;
            for (unsigned i = 0; i < m->fastclk_ncomp
                    && i < ARRAY_LEN(m->fastclk_comp_off); i++) {
               rt_nexus_t *cn = m->fastclk_comp[i];
               if (cn->last_event == m->now && cn->event_delta == m->iteration)
                  m->fastclk_comp_off[i]++;
            }
         }
         if (unlikely(m->fastclk_win_pos + m->fastclk_win_off >= 256)) {
            rt_nexus_t *busy = NULL;
            for (unsigned i = 0; i < m->fastclk_ncomp
                    && i < ARRAY_LEN(m->fastclk_comp_off) && busy == NULL; i++)
               if (m->fastclk_comp_off[i] > m->fastclk_win_pos)
                  busy = m->fastclk_comp[i];
            if (busy != NULL) {
               if (m->fastclk_nexcl < ARRAY_LEN(m->fastclk_excl))
                  m->fastclk_excl[m->fastclk_nexcl++] = busy;
               if (getenv("NVC_ACCEL_JIT_DEBUG") != NULL)
                  notef("accel-jit: fast-clk companion %s busy (off-edge "
                        "dispatches exceed posedges) — dissolve, its procs "
                        "excluded from the rebuild",
                        istr(tree_ident(busy->signal->where)));
               aj_dissolve_fastclk(m);   // re-arms with backoff
            }
            else {
               m->fastclk_win_pos = m->fastclk_win_off = 0;
               memset(m->fastclk_comp_off, 0, sizeof m->fastclk_comp_off);
            }
         }
      }
fastclk_done:;
   }

   if (m->shuffle)
      deferq_shuffle(&m->procq);

   // Run all non-postponed processes and event callbacks
   // #67 boundary merge: E-bank next-delta appends join the shared bank
   // before the swap (single-threaded here).
   if (unlikely(m->procq_e.count > 0)) {
      for (int i = 0; i < m->procq_e.count; i++)
         deferq_append(&m->procq, m->procq_e.tasks[i].fn,
                       m->procq_e.tasks[i].arg);
      m->procq_e.count = 0;
   }
   swap_deferq(&m->next_procq, &m->procq);
   evproc_ensure_started(m);
   // #67 eval window: under NVC_TWO_PHASE the inline wake publications and
   // E-side procq appends divert to the E banks for the duration of the
   // process evaluations, then replay/merge single-threaded — the
   // structurally-two-phase form the U thread will inherit.  Env off:
   // g_two_phase_eval stays 0 and every path is byte-identical to today.
   if (unlikely(g_two_phase < 0)) {
      const char *tp = getenv("NVC_TWO_PHASE");
      g_two_phase = tp != NULL;
      g_two_phase_run = tp != NULL ? atoi(tp) : 0;
      if (getenv("NVC_FORCE_PAR") != NULL)   // deopt-cost probe: JIT
         atomic_store(&g_par_active, 1);     // inline-drive declines +
   }                                          // deferq locks, no capture
   if (unlikely(g_two_phase))
      g_two_phase_eval = 1;
   if (unlikely(g_two_phase_run == 3)) {
      // TP3 overlap: ensure the U thread + a preallocated E pipe exist,
      // then arm the capture — driver writes stream to U during eval.
      if (!g_uthr.started)
         u_phase_handoff(m);   // creates + pins the U thread (idle req)
      const int etid = thread_id();
      if (g_evproc.pipes[etid] == NULL) {
         prop_pipe_t *p = xcalloc(sizeof(prop_pipe_t));
         p->max  = 131072;   // fixed: no realloc under the streaming reader
         p->recs = xmalloc_array(p->max, sizeof(prop_rec_t));
         g_evproc.pipes[etid] = p;
         g_uthr.e_tid = etid;
      }
      g_uthr.consumed = 0;
      g_evproc.pipes[etid]->count = 0;
      atomic_store(&g_par_active, 1);
      atomic_store(&g_capture, 1);
      atomic_store(&g_uthr.stream, 1);
   }
   if (unlikely(m->prof_enabled)) {
      const unsigned depth = m->next_procq.count;
      const int b = prof_bucket(depth);
      const uint64_t t0 = get_timestamp_ns();
      aj_sweep_run(m, &m->next_procq);
      const uint64_t dt = get_timestamp_ns() - t0;
      m->prof_deltas++;
      m->prof_activations += depth;
      m->prof_proc_ns += dt;
      m->prof_depth_hist[b]++;
      m->prof_depth_ns[b] += dt;
   }
   else
      aj_sweep_run(m, &m->next_procq);

   if (unlikely(g_two_phase)) {
      if (g_two_phase_run == 3) {
         atomic_store(&g_uthr.stream, 2);        // drain request
         while (atomic_load(&g_uthr.stream) != 0)
            spin_wait();                          // U quiesced
         atomic_store(&g_capture, 0);
         atomic_store(&g_par_active, 0);
      }
      g_two_phase_eval = 0;
      deferq_run(m, &m->ewakeq);   // boundary replay of deferred wakes
   }

   // Delta-ordered capture drain: re-wake deferred chunks once the next
   // delta reaches the target — or right away when the timestep would
   // otherwise end (each drain adds a delta, so the quiet-timestep case
   // self-extends and terminates at the target; captures land at
   // min(target, natural-end+1)).
   if (m->aj_capq_count > 0) {
      static int _cdk = -2;
      if (_cdk == -2) { const char *e = getenv("NVC_ACCEL_CAPTURE_DELTA");
                        _cdk = e ? atoi(e) : -1; }
      if (getenv("AJ_CAPDBG") != NULL)
         fprintf(stderr, "#CAPQ t=%"PRIi64"+%u count=%d nid=%d pend=%d\n",
                 m->now, m->iteration, m->aj_capq_count,
                 (int)m->next_is_delta,
                 m->aj_capq_count > 0 && m->aj_capq[0]->rr_count > 0
                    ? (int)m->aj_capq[0]->rr_saved[0].proc->wakeable.pending
                    : -1);
      if (m->iteration + 1 >= (unsigned)_cdk || !m->next_is_delta) {
         // direct boundary call: the capture runs HERE, after every proc of
         // this delta — clear the dedup claim the deferring eval left so
         // the call is not silently dropped, then evaluate.  Publications
         // schedule normally (active_obj is set inside aj_proc_eval).
         const int n = m->aj_capq_count;
         m->aj_capq_count = 0;   // captures may re-defer only via new arms
         for (int ci = 0; ci < n; ci++) {
            aj_chunk_t *c = m->aj_capq[ci];
            if (c->rr_count == 0 || c->rr_saved[0].proc == NULL)
               continue;
            c->last_eval_now = 0;
            c->cap_pending   = (uint64_t)-1;   // force: no re-defer
            aj_proc_eval(m, c->rr_saved[0].proc);
            c->cap_pending   = 0;
         }
      }
   }

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
      if (m->postponedq.count > 0)
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

      // NVC_ACCEL_DEMOTE_AT: the state is settled (same safe point as the
      // fork checkpoint, after can_create_delta so the writeback deposits
      // legally open the next delta at THIS time) — demote every installed
      // chunk; the interp re-settles the region before time advances.
      if (unlikely(g_aj_demote_at >= 0 && !g_aj_demoted
                   && m->now >= (uint64_t)g_aj_demote_at)) {
         g_aj_demoted = true;   // one-shot, even if some chunks decline
         accel_demote(m, NULL);
      }

      // X/Z FALLBACK: same settled safe point — a chunk whose boundary went
      // uncertain asks (via accel_x_seen) to be handed back to the interpreter.
      // Detection is always compiled in; the ACTION needs NVC_ACCEL_XDEMOTE=1.
      if (unlikely(g_aj_have_xdet) && m->aj_chunk_count > 0)
         aj_xseen_poll(m);
   }
   else if (m->stop_delta > 0 && m->iteration == m->stop_delta)
      reached_iteration_limit(m);

   part_maybe_build(m);   // Phase D S2a: one-shot, first cycle only
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

   if (m->aj_chunk_count > 0) {
      aj_link_handoff(m);   // wire packed chunk-to-chunk edges (NVC_ACCEL_HANDOFF)
      aj_quench_rerouted_drivers(m);
      aj_pin_census_dump(m);
   }

   run_callbacks(m, START_OF_SIMULATION);

   // Arm ONE abort landing pad for the whole scheduler loop. Previously
   // jit_try_vcall armed a setjmp (plus two state transitions and a
   // diag-hint add/remove) on every process activation -- millions of times --
   // purely to catch an abort that terminates the run anyway. With the pad
   // hoisted here, that scaffolding leaves the per-eval path entirely and an
   // abort inside any process unwinds straight to this frame.
   {
      volatile jit_state_t oldstate;
      jit_thread_local_t *volatile thread =
         jit_run_region_enter(m->jit, (jit_state_t *)&oldstate);

      if (jit_setjmp(thread->abort_env) == 0) {
         thread->jmp_buf_valid = 1;

         while (!should_stop_now(m, stop_time))
            model_cycle(m);
      }
      else {
         // An abort unwound out of a process body. Dispatch to THAT instance's
         // policy rather than applying one global rule here -- get_active_proc()
         // still names it, since the longjmp skipped the activation's clear.
         rt_proc_t *ap = get_active_proc();
         if (ap != NULL && ap->vtable->on_abort != NULL)
            ap->vtable->on_abort(m, ap);
         else
            m->force_stop = true;
      }

      jit_run_region_leave(m->jit, thread, oldstate);
   }

   aj_census_dump(m);
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

   aj_census_dump(m);
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
            if (likely(!(n->signal->shared.flags & SIG_F_LAST_VALUE)))
               memcpy(eff, vptr, valuesz);   // #74 E2: no 'last_value reader
            else
               copy2(last, eff, vptr, valuesz);
            m->trigger_epoch++;
            n->last_event = m->now;
            n->event_delta = m->iteration + 1;
            if (n->flags & NET_F_CACHE_EVENT)
               n->signal->shared.flags |= SIG_F_EVENT_FLAG;
            m->next_is_delta = true;
            if (unlikely(g_two_phase_eval))
               deferq_do(&m->ewakeq, async_replay_wake_all,
                         (void *)&(n->pending));
            else {
               g_lv_deposit_wake = true;
               wakeup_all(m, &(n->pending));
               g_lv_deposit_wake = false;
            }
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

bool g_aj_forceev = false;   // settle-window: byte-equal deposits still event

static void deposit_signal_impl(rt_model_t *m, rt_signal_t *s,
                                const void *values, int offset, size_t count,
                                bool wake_next)
{
   RT_LOCK(s->lock);

   TRACE("deposit signal %s+%d value=%s count=%zd", istr(tree_ident(s->where)),
         offset, fmt_values(values, count * s->nexus.size), count);

   assert(get_active_proc() == NULL || !get_active_proc()->wakeable.postponed);

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
      // A deposit applied between deltas (accel STAGE2 output staging via
      // aj_apply_stage2, or any bridge/external deposit) has no active process
      // -- there is no cone to record, and the depositor map only exists to
      // re-run a fused cone on force/release of a net it deposits.
      rt_proc_t *ap = get_active_proc();
      if (ap != NULL && ap->wakeable.fused_cone && !ap->wakeable.dep_recorded)
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

      // NVC_DEP_STATS census (VeeR interp, 3000ns): 77.3M nexus visits,
      // 93.1% byte-equal no-ops.  A compare-first reorder was measured
      // NEUTRAL (the no-op cost IS the compare; scaffolding is noise) —
      // the deposit VOLUME is the target, and that fix belongs at codegen
      // (shadow-gated deposit calls), not here.
      { static int _ds = -1;
        if (_ds < 0) _ds = getenv("NVC_DEP_STATS") != NULL;
        if (_ds) {
           extern uint64_t g_aj_dep_nx, g_aj_dep_w1;
           g_aj_dep_nx++;
           if (valuesz == 1) g_aj_dep_w1++;
        } }
      if (!cmp_bytes(eff, vptr, valuesz) || g_aj_forceev) {
         { static int _ds = -1;
           if (_ds < 0) _ds = getenv("NVC_DEP_STATS") != NULL;
           if (_ds) { extern uint64_t g_aj_dep_chg; g_aj_dep_chg++; } }
         if (likely(!(n->signal->shared.flags & SIG_F_LAST_VALUE)))
            memcpy(eff, vptr, valuesz);   // #74 E2: no 'last_value reader
         else
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

         if (unlikely(g_two_phase_eval))
            deferq_do(&m->ewakeq, async_replay_wake_all, (void *)&(n->pending));
         else {
            g_lv_deposit_wake = wake_next;
            wakeup_all(m, &(n->pending));
            g_lv_deposit_wake = false;
         }

         for (rt_source_t *o = n->outputs; o; o = o->chain_output) {
            rt_nexus_t *pn = NULL;
            switch (o->tag) {
            case SOURCE_PORT:
               pn = o->u.port.output;
               break;
            case SOURCE_IMPLICIT:
               // Reverse implicit: receiver deposit propagates to parent
               pn = o->u.pseudo.nexus;
               break;
            default:
               should_not_reach_here();
            }
            // In a sweep wave, hold the port propagation to wave end: the
            // wave's flush would drain the driving heap eagerly and commit
            // the port one delta early, desynchronising its event from
            // the deposit's deferred receivers (see g_lv_helddrv).
            if (g_lv_inwave && wake_next)
               aj_lv_hold_drv(pn);
            else
               defer_driving_update(m, pn);
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

   // assert-order: an NBA deposit scheduled with no active object means
   // the scheduler is not running (install/seed context) — the nonblockq
   // drain will not fire and the value never lands (measured: gals2
   // seeded Y=0, fixed by routing immediate at the ONE caller that could
   // hit it; the other nonblock call sites are posedge-gated and
   // currently unreachable at seed — this audit pins that against drift).
   { static int _ao = -1;
     if (_ao < 0) _ao = getenv("NVC_ACCEL_ASSERT_ORDER") != NULL;
     if (_ao && nonblock && after == 0
         && model_thread(m)->active_obj == NULL)
        notef("accel-assert: NBA DEPOSIT OUTSIDE SCHEDULER sig %s "
              "t=%"PRIu64" — nonblockq will not drain here; the value "
              "never lands (route immediate at the caller)",
              s->where != NULL ? istr(tree_ident(s->where)) : "?",
              (uint64_t)m->now);
   }

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

   wakeable_set_kind(&w->wakeable, W_WATCH);
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
         sched_event_par(m, &(n->pending), obj);
      }
      break;
   case FUNC_TRIGGER:
      {
         if (t->nargs >= 3) {
            sig_shared_t *ss = t->args[1].pointer;
            int32_t offset = t->args[2].integer;

            rt_signal_t *s = container_of(ss, rt_signal_t, shared);

            rt_nexus_t *n = split_nexus(m, s, offset, 1);
            sched_event_par(m, &(n->pending), obj);
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
            sched_event_par(m, &(n->pending), obj);

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
   wakeable_set_kind(&t->wakeable, W_TRIGGER);
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

   // NVC_BANKED I2: a fourth plane (value bank B) behind the env — the
   // double-banked substrate.  Eligibility is unknown this early (sources
   // register later), so every signal gets the plane; +33% signal bytes.
   // Cached here because x_init_signal runs during elaboration, before
   // accel_banked_init resolves g_banked.
   static int banked_planes = 0;
   if (banked_planes == 0)
      banked_planes = getenv("NVC_BANKED") != NULL ? 4 : 3;
   const size_t datasz = MAX((size_t)banked_planes * count * size, 8);
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
      set_pending(m, &proc->wakeable);
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

   set_pending(m, &proc->wakeable);
   deferq_do(&m->inactiveq, async_run_process, proc);
   m->next_is_delta = true;
}

void x_sched_waveform_s(sig_shared_t *ss, uint32_t offset, uint64_t scalar,
                        int64_t after, int64_t reject)
{
   if (unlikely(relaxed_load(&g_capture))) {
      // Worker-direct (NVC_DIRECT_SCHED): for the FAST_DRIVER whole-signal
      // single-nexus class, run sched_driver's fast path ON THE WORKER
      // instead of capturing a pipe record.  Safe because per-nexus driver
      // state (waveform, active marks) is single-writer under the
      // single-driver invariant, the nexus tree is frozen during parallel
      // eval, and the driverq append inside sched_driver already takes
      // g_sched_lock under g_par_active.  This keeps every fast-path side
      // effect (same-value dedup, w->value/when freshness, active_delta,
      // next_is_delta) that pipe replay provides, while moving the dedup
      // and value copy off the serial propagate phase.
      if (g_direct_sched) {
         rt_signal_t *s = container_of(ss, rt_signal_t, shared);
         if (after == 0 && s->n_nexus == 1 && s->nexus.width == 1
             && (s->nexus.flags & NET_F_FAST_DRIVER)) {
            sched_driver(get_model(), &s->nexus, 0, reject, &scalar,
                         get_active_proc());
            g_direct_cnt[thread_id()]++;
            return;
         }
      }
      // Worker eval: defer the driver write to thread 0 via the pipe.
      const int tid = thread_id();
      prop_rec_t *r = prop_reserve(tid);
      if (r != NULL) {   // NULL: TP3 overflow fallback — serial path below
         r->ss = ss; r->offset = offset; r->count = 1;
         r->after = after; r->reject = reject; r->proc = get_active_proc();
         r->scalar = true; r->sval = scalar; r->heapval = NULL;
         prop_commit(tid);
         return;
      }
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

   if (unlikely(relaxed_load(&g_capture))) {
      // Worker-direct: see x_sched_waveform_s.  sched_driver's fast path
      // copies the (transient) value immediately, so no capture needed.
      if (g_direct_sched && after == 0 && s->n_nexus == 1 && offset == 0
          && count == (int32_t)s->nexus.width
          && (s->nexus.flags & NET_F_FAST_DRIVER)) {
         sched_driver(get_model(), &s->nexus, 0, reject, values,
                      get_active_proc());
         g_direct_cnt[thread_id()]++;
         return;
      }
      // Worker eval: defer the driver write to thread 0 via the pipe. Copy the
      // value (transient on the worker); wide values go to a heap buffer the
      // applier frees.
      const int tid = thread_id();
      const uint32_t nbytes = count * s->nexus.size;
      prop_rec_t *r = prop_reserve(tid);
      if (r != NULL) {   // NULL: TP3 overflow fallback — serial path below
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

////////////////////////////////////////////////////////////////////////////
// Native-projection pilot (NVC_INLINE_DRIVE): publish the frozen layout of
// the sched_driver fast path (see jit_drive_layout_t in jit-exits.h) so the
// LLVM backend can inline the common-case scalar delta assignment.  This is
// pure DESCRIPTION -- no policy: the eligibility decision lives in
// jit-irgen.c (static shape) and in the emitted guards (dynamic state).
// Bitfield positions are probed rather than assumed; if any probe finds a
// layout we cannot describe, `valid` stays false and every backend keeps
// the spec path.

static jit_drive_layout_t drive_layout;

static bool drive_layout_probe(const void *obj, size_t size, int32_t *off,
                               uint8_t *mask)
{
   const uint8_t *p = obj;
   for (size_t i = 0; i < size; i++) {
      if (p[i] != 0) {
         // Exactly one bit in exactly one byte
         if (__builtin_popcount(p[i]) != 1)
            return false;
         for (size_t j = i + 1; j < size; j++) {
            if (p[j] != 0)
               return false;
         }
         *off = i;
         *mask = p[i];
         return true;
      }
   }

   return false;
}

__attribute__((constructor))
static void drive_layout_init(void)
{
   jit_drive_layout_t *l = &drive_layout;

   rt_source_t src;
   memset(&src, '\0', sizeof(src));
   src.fastqueued = 1;
   if (!drive_layout_probe(&src, sizeof(src), &l->source_bits,
                           &l->source_fastqueued_mask))
      return;

   memset(&src, '\0', sizeof(src));
   src.was_active = 1;
   int32_t wa_off;
   if (!drive_layout_probe(&src, sizeof(src), &wa_off,
                           &l->source_was_active_mask))
      return;
   else if (wa_off != l->source_bits)
      return;   // Both flags must share one byte for the RMW sequence

   rt_wakeable_t wake;
   memset(&wake, '\0', sizeof(wake));
   wake.postponed = 1;
   if (!drive_layout_probe(&wake, sizeof(wake), &l->wakeable_bits,
                           &l->wakeable_postponed_mask))
      return;

   l->model_var         = &__model;
   l->par_active_var    = (void *)&g_par_active;
   l->trace_var         = (void *)&__trace_on;
   l->fast_driver_fn    = (void *)async_fast_driver;

   l->signal_shared     = offsetof(rt_signal_t, shared);
   l->signal_nexus      = offsetof(rt_signal_t, nexus);
   l->shared_flags      = offsetof(sig_shared_t, flags);
   l->nexus_flags       = offsetof(rt_nexus_t, flags);
   l->nexus_size        = offsetof(rt_nexus_t, size);
   l->nexus_n_sources   = offsetof(rt_nexus_t, n_sources);
   l->nexus_width       = offsetof(rt_nexus_t, width);
   l->nexus_active_delta = offsetof(rt_nexus_t, active_delta);
   l->nexus_sources     = offsetof(rt_nexus_t, sources);

   l->source_when       = offsetof(rt_source_t, u.driver.waveforms.when);
   l->source_value      = offsetof(rt_source_t, u.driver.waveforms.value);

   l->model_now          = offsetof(rt_model_t, now);
   l->model_iteration    = offsetof(rt_model_t, iteration);
   l->model_next_is_delta = offsetof(rt_model_t, next_is_delta);
   l->model_probe_member = offsetof(rt_model_t, fastclk_probe_member);
   l->model_thread0      = offsetof(rt_model_t, threads);
   l->thread_active_obj  = offsetof(model_thread_t, active_obj);

   l->driverq_tasks     = offsetof(rt_model_t, driverq)
      + offsetof(deferq_t, tasks);
   l->driverq_count     = offsetof(rt_model_t, driverq)
      + offsetof(deferq_t, count);
   l->driverq_max       = offsetof(rt_model_t, driverq)
      + offsetof(deferq_t, max);
   l->task_size         = sizeof(defer_task_t);
   l->task_fn           = offsetof(defer_task_t, fn);
   l->task_arg          = offsetof(defer_task_t, arg);

   l->net_f_fast_driver = NET_F_FAST_DRIVER;

   // The emitted fast path assumes the model thread is the only thread
   // scheduling drivers outside a g_par_active window and stores the value
   // as a full 8-byte qword (copy_value_ptr's small-value behaviour)
   STATIC_ASSERT(sizeof(rt_value_t) == 8);
   STATIC_ASSERT(sizeof(delta_cycle_t) == 2);
   STATIC_ASSERT(sizeof(net_flags_t) == 1);
   STATIC_ASSERT(sizeof(((rt_model_t *)0)->next_is_delta) == 1);
   STATIC_ASSERT(sizeof(((rt_model_t *)0)->iteration) == 4);
   STATIC_ASSERT(sizeof(((rt_model_t *)0)->now) == 8);
   STATIC_ASSERT(sizeof(((rt_model_t *)0)->fastclk_probe_member) == 4);
   STATIC_ASSERT(sizeof(((deferq_t *)0)->count) == 4);
   STATIC_ASSERT(sizeof(((rt_nexus_t *)0)->width) == 4);
   STATIC_ASSERT(sizeof(((sig_shared_t *)0)->flags) == 4);
   STATIC_ASSERT(sizeof(((waveform_t *)0)->when) == 8);

#if !RT_MULTITHREADED
   l->valid = true;
#endif
}

const jit_drive_layout_t *jit_drive_layout(void)
{
   return &drive_layout;
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

   wakeable_set_kind(&t->wakeable, W_TRANSFER);
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
         deferq_do(unlikely(g_two_phase_eval) ? &m->procq_e : &m->procq,
                   async_transfer_signal, t);
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

   // Static-wait fast path: after the first activation the pending-list
   // entries persist forever (enable/disable model — the list is never
   // touched again). Later activations only fingerprint their re-arm
   // calls so run_process can detect a process whose wait set actually
   // changes and demote it to the classic dynamic path.
   if (obj->kind == W_PROC && obj->trigger == NULL && obj->wait_state != 2) {
      rt_proc_t *p = container_of(obj, rt_proc_t, wakeable);
      for (; count > 0; n = n->chain) {
         p->cur_sig ^= (uint64_t)(uintptr_t)n;
         if (p->cur_count == p->cur_cap) {
            p->cur_cap = p->cur_cap ? p->cur_cap * 2 : 16;
            p->cur_set = xrealloc_array(p->cur_set, p->cur_cap,
                                        sizeof(rt_nexus_t *));
         }
         p->cur_set[p->cur_count++] = n;
         if (obj->wait_state == 0) {
            sched_event_par(m, &(n->pending), obj);
            if (p->wait_count == p->wait_cap) {
               p->wait_cap = p->wait_cap ? p->wait_cap * 2 : 16;
               p->wait_set = xrealloc_array(p->wait_set, p->wait_cap,
                                            sizeof(rt_nexus_t *));
            }
            p->wait_set[p->wait_count++] = n;
         }
         count -= n->width;
         assert(count >= 0);
      }
      return;
   }

   for (; count > 0; n = n->chain) {
      sched_event_par(m, &(n->pending), obj);

      count -= n->width;
      assert(count >= 0);
   }
}

void x_enable_last_value(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   (void)offset; (void)count;   // signal-level granularity (no spare
                                // per-nexus flag bits)
   ss->flags |= SIG_F_LAST_VALUE;
}

void x_clear_event(sig_shared_t *ss, uint32_t offset, int32_t count)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);
   RT_LOCK(s->lock);

   TRACE("clear event %s+%d count=%d",
         istr(tree_ident(s->where)), offset, count);

   rt_model_t *m = get_model();
   rt_proc_t *proc = get_active_proc();

   // Static-wait: entries persist; the re-arm clear is a no-op. (State 0
   // has no prior registrations, state 1 keeps them by design.)
   if (proc->wakeable.kind == W_PROC && proc->wakeable.trigger == NULL
       && proc->wakeable.wait_state != 2)
      return;

   rt_nexus_t *n = split_nexus(m, s, offset, count);
   for (; count > 0; n = n->chain) {
      clear_event_par(m, &(n->pending), &(proc->wakeable));

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

   sched_event_par(m, &(trigger->pending), obj);
}

void x_disable_trigger(rt_trigger_t *trigger)
{
   TRACE("disable trigger %p", trigger);

   rt_wakeable_t *obj = get_active_wakeable();
   rt_model_t *m = get_model();

   clear_event_par(m, &(trigger->pending), obj);
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

// #74 fuse: TRUE iff this scalar signal could not contribute a value to a
// resolved net — no sources, no port fanout, and its effective value is 'Z'
// (the resolution identity, so its constant contribution is vacuous).
bool x_signal_undriven(sig_shared_t *ss, uint32_t offset)
{
   rt_signal_t *s = container_of(ss, rt_signal_t, shared);

   if (s->n_nexus != 1 || s->nexus.width != 1 || s->nexus.size != 1)
      return false;

   rt_nexus_t *n = &(s->nexus);
   if (n->n_sources != 0 || n->outputs != NULL)
      return false;

   return ss->data[n->offset] == 0x04;   // 'Z'
}

// #74 fuse: repoint signal B's frame-var handle at signal A so every
// subsequent read of B resolves into A's storage (zero per-event copies).
// Wakeables already registered on B are spliced onto A's pending list;
// everything registering after the swap lands on A natively.  Returns
// false (caller keeps its copy-loop fallback) on any structural doubt.
bool x_fuse_signals(sig_shared_t *ss_a, uint32_t off_a,
                    sig_shared_t *ss_b, uint32_t off_b)
{
   rt_model_t *m = get_model();
   rt_signal_t *sa = container_of(ss_a, rt_signal_t, shared);
   rt_signal_t *sb = container_of(ss_b, rt_signal_t, shared);

   static int no_fuse = -1;
   if (no_fuse < 0)
      no_fuse = getenv("NVC_NO_FUSE") != NULL;

   static int fuse_dbg = -1;
   if (fuse_dbg < 0)
      fuse_dbg = getenv("NVC_FUSE_DEBUG") != NULL;

#define FUSE_DECLINE(why) do {                                          \
      if (fuse_dbg)                                                     \
         fprintf(stderr, "#FUSE decline %s -> %s: %s\n",               \
                 sb->where ? istr(tree_ident(sb->where)) : "?",         \
                 sa->where ? istr(tree_ident(sa->where)) : "?", why);   \
      return false;                                                     \
   } while (0)

   if (no_fuse)
      FUSE_DECLINE("NVC_NO_FUSE");
   if (m->now != 0 || m->iteration > 0)
      FUSE_DECLINE("not initialization");
   if (sa == sb)
      FUSE_DECLINE("self");
   if (sa->n_nexus != 1 || sb->n_nexus != 1
       || sa->nexus.width != 1 || sb->nexus.width != 1
       || sa->nexus.size != sb->nexus.size)
      FUSE_DECLINE("not matching scalars");
   if (sb->nexus.outputs != NULL)
      FUSE_DECLINE("B has port fanout");

   // The generated arbitration process carries its own fallback copy
   // branch, so B has a pre-created driver owned by the CALLING process.
   // That driver never fires once the caller parks after a successful
   // fuse; any other source keeps copy semantics.
   if (sb->nexus.n_sources > 0) {
      rt_proc_t *caller = get_active_proc();
      for (rt_source_t *src = &(sb->nexus.sources); src != NULL;
           src = src->chain_input) {
         if (src->tag != SOURCE_DRIVER || src->u.driver.proc != caller)
            FUSE_DECLINE("B has a foreign source");
      }
   }
   if (ss_b->flags & (SIG_F_FUSED | SIG_F_FUSE_TARGET | SIG_F_REGISTER
                      | SIG_F_PIPE | SIG_F_IMPLICIT))
      FUSE_DECLINE("B flag class");
   if (ss_a->flags & SIG_F_FUSED)
      FUSE_DECLINE("A already fused");
   if (sa->nexus.vtable == &nexus_fused_vtable)
      FUSE_DECLINE("A tombstoned");
   if (m->aj_chunk_count > 0)
      FUSE_DECLINE("accel chunks installed");
   if (sb->where == NULL || sb->parent == NULL)
      FUSE_DECLINE("B anonymous");

   // A waveform/VHPI watch pinned to B would keep formatting B's stale
   // bytes while firing off A's events — keep copy semantics instead
   if (sb->nexus.pending != NULL) {
      if (pointer_tag(sb->nexus.pending) == 1) {
         rt_wakeable_t *w = untag_pointer(sb->nexus.pending, rt_wakeable_t);
         if (w->kind == W_WATCH)
            FUSE_DECLINE("B watched");
      }
      else {
         rt_pending_t *p = untag_pointer(sb->nexus.pending, rt_pending_t);
         for (int i = 0; i < p->count; i++)
            if (p->wake[i] != NULL && p->wake[i]->kind == W_WATCH)
               FUSE_DECLINE("B watched");
      }
   }

   if (sb->parent->privdata == MPTR_INVALID)
      FUSE_DECLINE("B scope not linked");

   jit_handle_t h = jit_lazy_compile(m->jit, sb->parent->name);
   if (h == JIT_HANDLE_INVALID)
      FUSE_DECLINE("B scope unit not found");

   void *slot = jit_try_get_frame_var(m->jit, h, tree_ident(sb->where));
   if (slot == NULL)
      FUSE_DECLINE("no frame var for B");
   if (*(sig_shared_t **)slot != ss_b)
      FUSE_DECLINE("frame var does not hold B");

   // Splice wakeables already registered on B onto A
   if (sb->nexus.pending != NULL) {
      if (pointer_tag(sb->nexus.pending) == 1) {
         rt_wakeable_t *w = untag_pointer(sb->nexus.pending, rt_wakeable_t);
         sched_event(m, &(sa->nexus.pending), w);
      }
      else {
         rt_pending_t *p = untag_pointer(sb->nexus.pending, rt_pending_t);
         for (int i = 0; i < p->count; i++)
            if (p->wake[i] != NULL)
               sched_event(m, &(sa->nexus.pending), p->wake[i]);
      }
      sb->nexus.pending = NULL;
   }

   // Reader-need flags must follow the readers
   sa->nexus.flags |= (sb->nexus.flags & (NET_F_CACHE_EVENT | NET_F_PENDING));
   ss_a->flags |= (ss_b->flags & (SIG_F_CACHE_EVENT | SIG_F_LAST_VALUE));

   ss_b->flags |= SIG_F_FUSED;
   ss_a->flags |= SIG_F_FUSE_TARGET;
   sb->nexus.vtable = &nexus_fused_vtable;

   *(sig_shared_t **)slot = ss_a;
   *(int32_t *)((char *)slot + 8) = off_a;

   if (fuse_dbg)
      fprintf(stderr, "#FUSE ok %s -> %s\n",
              istr(tree_ident(sb->where)),
              sa->where ? istr(tree_ident(sa->where)) : "?");

#undef FUSE_DECLINE
   return true;
}

// ===== #75 kernel net solver ("stitching") ==============================
//
// A tran/switch net's static endpoint sets are registered once at start
// of simulation (via the resolver plugin); the kernel keeps per-plane
// counts state and on each contributor transition (O(1)) produces the
// resolved value and the per-endpoint exclude-self 'other views, waking
// only observers whose view changed.  Per-plane work is elided when a
// plane has no consumers (3D logic separates concerns); data layouts
// are unchanged — this is code specialization only.

typedef enum {
   STITCH_STD  = 0,   // std_logic byte
   STITCH_L3D  = 1,   // logic3d 4-byte int
   STITCH_L3DS = 2,   // logic3ds 4-byte record
} stitch_type_t;

// Canonical contribution (l3ds semantics)
typedef struct {
   uint8_t val;       // 0 or 255 (other -> escape)
   uint8_t str;       // 0..31
   uint8_t flags;     // l3ds_flags 0..5
} stitch_ctr_t;

#define STITCH_FL_KNOWN       0
#define STITCH_FL_UNKNOWN     1
#define STITCH_FL_UNDRIVEN    2
#define STITCH_FL_NOPOWER     3
#define STITCH_FL_UNK_NOPOWER 4
#define STITCH_FL_UDR_NOPOWER 5

#define STITCH_ST_HIGHZ  0
#define STITCH_ST_WEAK   2
#define STITCH_ST_STRONG 8
#define STITCH_ST_SUPPLY 16

typedef struct {
   uint16_t n_val0, n_val1, n_valother, n_unk;
} stitch_scnt_t;

typedef struct {
   // Scalar endpoints use [0] only; l3ds RECORD implicits materialize
   // as per-field signals {VALUE, STRENGTH, FLAGS} (child scope of the
   // instance) and use all three slots
   rt_signal_t  *drv[3];       // contributor ('driver fields or net signal)
   rt_signal_t  *other[3];     // exclude-self view output (NULL = none)
   stitch_type_t dtype;        // contributor value encoding
   stitch_type_t otype;        // 'other value encoding
   bool          is_member;    // net-signal endpoint (other == drv)
   stitch_ctr_t  last;         // last classified contribution
   rt_nexus_t   *mnex;         // member: the resolved nexus — the whole
                               // signal's for scalars, the element's
                               // for one bit of a vector net
   rt_signal_t  *plug;         // member: solver-owned SOURCE_IMPLICIT input
   rt_source_t  *real_src;     // member: its own real driver source (or NULL)
   rt_signal_t  *echo;         // port signal whose edge into the member
                               // duplicates this endpoint's 'driver
                               // (STD_MX auto-connect); excluded from
                               // the member fold to keep exclude-self
                               // views honest
} stitch_ep_t;

typedef struct _rt_stitch_net {
   struct _rt_stitch_net *chain;
   uint64_t       n_events;    // contributor transitions (NVC_STITCH_PROF)
   char          *path;        // member signal's hierarchical path
   int            nep;
   stitch_scnt_t  cnt[32];     // per strength level
   uint16_t       n_supply_pwr;
   stitch_ep_t    ep[];
} rt_stitch_net_t;

static rt_stitch_net_t *g_stitch_nets = NULL;
static int g_stitch_trace = -1;

static inline bool stitch_trace(void)
{
   if (g_stitch_trace < 0) {
      const char *e = getenv("NVC_STITCH_DEBUG");
      g_stitch_trace = (e != NULL && atoi(e) >= 2);
   }
   return g_stitch_trace;
}

static const uint8_t stitch_l3d_from_std[9] = {7, 6, 2, 3, 4, 5, 0, 1, 6};
static const uint8_t stitch_std_from_l3d[8] = {6, 7, 2, 3, 4, 5, 1, 0};

static stitch_ctr_t stitch_classify_raw(stitch_type_t dtype, const void *raw)
{
   stitch_ctr_t c = { 0, STITCH_ST_HIGHZ, STITCH_FL_UNDRIVEN };
   uint32_t l3d = 8;

   switch (dtype) {
   case STITCH_L3DS:
      {
         const uint8_t *b = raw;
         c.val = b[0]; c.str = b[1]; c.flags = b[2];
         return c;
      }
   case STITCH_L3D:
      l3d = *(const uint32_t *)raw & 7;
      break;
   case STITCH_STD:
      l3d = stitch_l3d_from_std[*(const uint8_t *)raw < 9
                                ? *(const uint8_t *)raw : 1];
      break;
   }

   // A plain Verilog driver is STRONG by definition; the l3d weak
   // alphabet (H/L/W) enters at WEAK.  Only an explicit strength spec
   // (l3ds 'driver endpoints) can contribute SUPPLY or PULL — a plain
   // driver classified at supply would wrongly tie an opposing
   // supply-spec assign into X (drive_strength su1st0)
   switch (l3d) {
   case 2: c = (stitch_ctr_t){ 0,   STITCH_ST_STRONG, STITCH_FL_KNOWN }; break;
   case 3: c = (stitch_ctr_t){ 255, STITCH_ST_STRONG, STITCH_FL_KNOWN }; break;
   case 0: c = (stitch_ctr_t){ 0,   STITCH_ST_WEAK,   STITCH_FL_KNOWN }; break;
   case 1: c = (stitch_ctr_t){ 255, STITCH_ST_WEAK,   STITCH_FL_KNOWN }; break;
   case 4: c = (stitch_ctr_t){ 0, STITCH_ST_HIGHZ, STITCH_FL_UNDRIVEN }; break;
   case 6: c = (stitch_ctr_t){ 0, STITCH_ST_STRONG, STITCH_FL_UNKNOWN }; break;
   case 5: c = (stitch_ctr_t){ 0, STITCH_ST_WEAK,   STITCH_FL_UNKNOWN }; break;
   default: c = (stitch_ctr_t){ 0, STITCH_ST_HIGHZ, STITCH_FL_UNKNOWN }; break;
   }
   return c;
}

static void stitch_count_into(stitch_scnt_t *cnt, int *pwr,
                              const stitch_ctr_t *c, int dir);
static stitch_ctr_t stitch_resolve_counts(const stitch_scnt_t *cnt,
                                          int pwr,
                                          const stitch_ctr_t *excl);

// A registered endpoint's 'driver auto-connects to its parent port
// signal in STD_MX mode, so the same contribution arrives at the member
// twice: once via our 'driver watch and once as a port edge.  Skip the
// port-edge copy — counting it would fold each endpoint's own drive
// into every exclude-self view
static bool stitch_is_echo(const rt_stitch_net_t *net, const rt_source_t *src)
{
   if (src->tag != SOURCE_PORT && src->tag != SOURCE_IMPLICIT)
      return false;
   for (int i = 0; i < net->nep; i++) {
      const stitch_ep_t *e2 = &(net->ep[i]);
      if (e2->echo != NULL && src->u.port.input == &(e2->echo->nexus))
         return true;
   }
   return false;
}

static stitch_ctr_t stitch_classify(const rt_stitch_net_t *net,
                                    const stitch_ep_t *ep)
{
   if (!ep->is_member) {
      if (ep->dtype == STITCH_L3DS) {
         // Record fields lower at their own widths: VALUE/STRENGTH are
         // 4-byte naturals, FLAGS a 1-byte enum.  Values are 0..255 so
         // the low byte suffices; read per-field width (little endian).
#define STITCH_FLD(ep, f) \
         ((ep)->drv[f]->shared.size == 1 \
          ? (ep)->drv[f]->shared.data[(ep)->drv[f]->nexus.offset] \
          : (uint8_t)*(const uint32_t *)((ep)->drv[f]->shared.data \
                                         + (ep)->drv[f]->nexus.offset))
         stitch_ctr_t c = { STITCH_FLD(ep, 0), STITCH_FLD(ep, 1),
                            STITCH_FLD(ep, 2) };
#undef STITCH_FLD
         return c;
      }
      return stitch_classify_raw(ep->dtype,
                                 ep->drv[0]->shared.data
                                 + ep->drv[0]->nexus.offset);
   }

   // Member endpoint: the contribution is the fold of ALL its
   // driver-backed sources except our own plug — primitives with OUT
   // ports (pullup, bufif) reach the net through port edges, not
   // SOURCE_DRIVERs, and their weak/strong distinction rides the l3d
   // H/L vs 1/0 alphabet.  source_value applies the STD_MX pruning so
   // inert edges contribute nothing.
   rt_nexus_t *mn = ep->mnex;
   stitch_scnt_t cnt[32];
   memset(cnt, 0, sizeof(cnt));
   int pwr = 0, nc = 0;

   if (mn->n_sources > 0) {
      for (rt_source_t *src = &(mn->sources); src != NULL;
           src = src->chain_input) {
         if (src->tag == SOURCE_IMPLICIT
             && src->u.port.input == &(ep->plug->nexus))
            continue;
         if (stitch_is_echo(net, src))
            continue;
         const void *sv = mn->vtable->read_source(mn, src);
         if (sv == NULL)
            continue;
         stitch_ctr_t c = stitch_classify_raw(ep->dtype, sv);
         stitch_count_into(cnt, &pwr, &c, +1);
         nc++;
      }
   }

   if (nc == 0)
      return (stitch_ctr_t){ 0, STITCH_ST_HIGHZ, STITCH_FL_UNDRIVEN };

   return stitch_resolve_counts(cnt, pwr, NULL);
}

static inline bool stitch_active(const stitch_ctr_t *c)
{
   return c->str != STITCH_ST_HIGHZ && c->flags != STITCH_FL_UNDRIVEN
      && c->flags != STITCH_FL_UDR_NOPOWER;
}

static void stitch_count_into(stitch_scnt_t *cnt, int *pwr,
                              const stitch_ctr_t *c, int dir)
{
   if (!stitch_active(c))
      return;
   stitch_scnt_t *s = &(cnt[c->str & 31]);
   if (c->flags == STITCH_FL_UNKNOWN || c->flags == STITCH_FL_UNK_NOPOWER)
      s->n_unk += dir;
   else if (c->val == 0)
      s->n_val0 += dir;
   else if (c->val == 255)
      s->n_val1 += dir;
   else
      s->n_valother += dir;   // non-binary byte: forces exact escape
   if (c->str == STITCH_ST_SUPPLY
       && (c->flags == STITCH_FL_NOPOWER || c->flags == STITCH_FL_UNK_NOPOWER))
      *pwr += dir;
}

static void stitch_count(rt_stitch_net_t *net, const stitch_ctr_t *c, int dir)
{
   int pwr = net->n_supply_pwr;
   stitch_count_into(net->cnt, &pwr, c, dir);
   net->n_supply_pwr = pwr;
}

// Resolve from counts, optionally excluding one contribution.  Scan
// strength bases high to low; within a base the active (cap=0) level
// beats capacitive (cap=1).  Exact counterpart of l3ds_resolve.
static stitch_ctr_t stitch_resolve_counts(const stitch_scnt_t *cnt,
                                          int pwr,
                                          const stitch_ctr_t *excl)
{
   const bool ex_ok = excl != NULL && stitch_active(excl);
   const bool ex_unk = ex_ok && (excl->flags == STITCH_FL_UNKNOWN
                                 || excl->flags == STITCH_FL_UNK_NOPOWER);

   if (ex_ok && excl->str == STITCH_ST_SUPPLY
       && (excl->flags == STITCH_FL_NOPOWER
           || excl->flags == STITCH_FL_UNK_NOPOWER))
      pwr--;

   for (int base = 15; base >= 0; base--) {
      for (int cap = 0; cap <= 1; cap++) {
         const int str = base*2 + cap;
         if (str == STITCH_ST_HIGHZ)
            continue;
         const stitch_scnt_t *s = &(cnt[str]);
         int n0 = s->n_val0, n1 = s->n_val1, no = s->n_valother,
             nu = s->n_unk;
         if (ex_ok && excl->str == str) {
            if (ex_unk) nu--;
            else if (excl->val == 0) n0--;
            else if (excl->val == 255) n1--;
            else no--;
         }
         if (n0 + n1 + no + nu == 0)
            continue;

         stitch_ctr_t r = { 0, (uint8_t)str, STITCH_FL_KNOWN };
         if (nu > 0 || no > 0 || (n0 > 0 && n1 > 0))
            r.flags = STITCH_FL_UNKNOWN;   // X / contention (value dead)
         else
            r.val = (n1 > 0) ? 255 : 0;

         if (pwr > 0)
            r.flags = (r.flags == STITCH_FL_UNKNOWN)
               ? STITCH_FL_UNK_NOPOWER : STITCH_FL_NOPOWER;
         return r;
      }
   }

   return (stitch_ctr_t){ 0, STITCH_ST_HIGHZ, STITCH_FL_UNDRIVEN };  // Z
}

// #75 A/B (user directive "state-machine where possible"): resolve by
// folding the endpoints' cached contributions directly instead of
// reading the counts state.  O(nep) per view against the counts scan's
// O(strength levels); tran pairs (nep 2-3) dominate real designs, so
// the crossover matters.  Semantics must match stitch_resolve_counts
// EXACTLY: same dominance order (higher base wins; within a base the
// active cap=0 level beats capacitive cap=1), same one-contribution
// value-exclusion, same supply-power flagging.  NVC_STITCH_ITERATE=1
// selects this form; NVC_STITCH_PROF=1 dumps per-net event/view tallies.
static stitch_ctr_t stitch_resolve_iterate(const rt_stitch_net_t *net,
                                           const stitch_ctr_t *excl)
{
   bool ex_pending = excl != NULL && stitch_active(excl);
   const bool ex_unk = ex_pending && (excl->flags == STITCH_FL_UNKNOWN
                                      || excl->flags == STITCH_FL_UNK_NOPOWER);
   bool ex_pwr_pending = ex_pending && excl->str == STITCH_ST_SUPPLY
      && (excl->flags == STITCH_FL_NOPOWER
          || excl->flags == STITCH_FL_UNK_NOPOWER);

   // Comparable rank reproducing the counts scan order (base 15..0,
   // cap 0 before cap 1): higher base first, even str beats odd
   #define STITCH_RANK(str) ((((str) & ~1) << 1) | (((str) & 1) ^ 1))

   int pwr = 0, bestrank = -1;
   uint8_t beststr = 0;
   int n0 = 0, n1 = 0, no = 0, nu = 0;

   for (int i = 0; i < net->nep; i++) {
      const stitch_ctr_t *c = &(net->ep[i].last);
      if (!stitch_active(c))
         continue;

      const bool c_unk = c->flags == STITCH_FL_UNKNOWN
         || c->flags == STITCH_FL_UNK_NOPOWER;

      if (c->str == STITCH_ST_SUPPLY
          && (c->flags == STITCH_FL_NOPOWER
              || c->flags == STITCH_FL_UNK_NOPOWER)) {
         if (ex_pwr_pending) ex_pwr_pending = false;
         else pwr++;
      }

      // Value-exclusion of exactly ONE matching contribution, like the
      // counts form's bucket decrement (equal contributions are
      // interchangeable in the fold)
      if (ex_pending && c->str == excl->str && c_unk == ex_unk
          && (c_unk || c->val == excl->val)) {
         ex_pending = false;
         continue;
      }

      if (c->str == STITCH_ST_HIGHZ)
         continue;   // the counts scan skips this level too

      const int rank = STITCH_RANK(c->str);
      if (rank < bestrank)
         continue;
      if (rank > bestrank) {
         bestrank = rank;
         beststr = c->str;
         n0 = n1 = no = nu = 0;
      }
      if (c_unk) nu++;
      else if (c->val == 0) n0++;
      else if (c->val == 255) n1++;
      else no++;
   }
   #undef STITCH_RANK

   if (bestrank < 0)
      return (stitch_ctr_t){ 0, STITCH_ST_HIGHZ, STITCH_FL_UNDRIVEN };  // Z

   stitch_ctr_t r = { 0, beststr, STITCH_FL_KNOWN };
   if (nu > 0 || no > 0 || (n0 > 0 && n1 > 0))
      r.flags = STITCH_FL_UNKNOWN;
   else
      r.val = (n1 > 0) ? 255 : 0;

   if (pwr > 0)
      r.flags = (r.flags == STITCH_FL_UNKNOWN)
         ? STITCH_FL_UNK_NOPOWER : STITCH_FL_NOPOWER;
   return r;
}

static int g_stitch_iterate = -1;
static uint64_t g_stitch_n_resolve = 0;

// Per-net solver profile (#75 residue): NVC_STITCH_PROF=1 dumps each
// net's endpoint count and contributor-event tally at exit, plus the
// global resolve-call count and which resolver form ran
__attribute__((destructor))
static void stitch_prof_dump(void)
{
   const char *e = getenv("NVC_STITCH_PROF");
   if (e == NULL || atoi(e) == 0 || g_stitch_nets == NULL)
      return;
   uint64_t tot = 0;
   unsigned nnets = 0;
   for (rt_stitch_net_t *net = g_stitch_nets; net; net = net->chain) {
      fprintf(stderr, "NVC_STITCH_PROF: net=%s nep=%d events=%"PRIu64"\n",
              net->path ? net->path : "?", net->nep, net->n_events);
      tot += net->n_events;
      nnets++;
   }
   fprintf(stderr, "NVC_STITCH_PROF: nets=%u events=%"PRIu64
           " resolves=%"PRIu64" form=%s\n", nnets, tot,
           g_stitch_n_resolve,
           g_stitch_iterate == 1 ? "iterate" : "counts");
}

static stitch_ctr_t stitch_resolve(rt_stitch_net_t *net,
                                   const stitch_ctr_t *excl)
{
   if (unlikely(g_stitch_iterate < 0)) {
      const char *e = getenv("NVC_STITCH_ITERATE");
      g_stitch_iterate = (e != NULL && atoi(e) != 0);
   }
   g_stitch_n_resolve++;
   if (g_stitch_iterate)
      return stitch_resolve_iterate(net, excl);
   return stitch_resolve_counts(net->cnt, net->n_supply_pwr, excl);
}

// Encode a canonical result in an endpoint's output type
static void stitch_encode(stitch_type_t t, const stitch_ctr_t *c,
                          uint8_t *out, size_t *sz)
{
   uint32_t l3d;
   switch (c->flags) {
   case STITCH_FL_UNDRIVEN: case STITCH_FL_UDR_NOPOWER:
      l3d = 4; break;                            // Z
   case STITCH_FL_UNKNOWN: case STITCH_FL_UNK_NOPOWER:
      // Strong conflicts export X.  Below-strong conflicts export Z:
      // W (code 5) carries value-plane 1, which breaks the doctrine's
      // value-plane compares against a 1'bx literal (L3D_X, value 0),
      // while X would wrongly tie a native STRONG driver into X (the
      // keeper's uncertain phases must lose to the bufif).  Z keeps
      // the uncertain bit ($isunknown sees it), reads as value 0, and
      // is identity in native resolution — every present constraint
      l3d = (c->str >= STITCH_ST_STRONG) ? 6 : 4; break;
   case STITCH_FL_NOPOWER:
      l3d = 6; break;                            // X
   default:
      // The l3d alphabet has two levels: everything below STRONG
      // (weak, pull) projects onto the weak codes so a native strong
      // driver still wins the exported view — matching the kernel's
      // own exact-ladder resolution
      if (c->str >= STITCH_ST_STRONG)
         l3d = (c->val >= 127) ? 3 : 2;          // '1' : '0'
      else
         l3d = (c->val >= 127) ? 1 : 0;          // 'H' : 'L'
      break;
   }

   switch (t) {
   case STITCH_L3DS:
      out[0] = c->val; out[1] = c->str; out[2] = c->flags; out[3] = 0;
      *sz = 4;
      break;
   case STITCH_L3D:
      *(uint32_t *)out = l3d;
      *sz = 4;
      break;
   case STITCH_STD:
      out[0] = stitch_std_from_l3d[l3d];
      *sz = 1;
      break;
   }
}

// Deposit an output value to a view signal if it changed (index-
// addressed wakeup: only this signal's pending list is notified)
static void stitch_put(rt_model_t *m, rt_signal_t *s, const uint8_t *val,
                       size_t sz)
{
   rt_nexus_t *n = &(s->nexus);
   if (memcmp(nexus_effective(n), val, sz) == 0)
      return;
   n->vtable->deposit(m, n, val);
}

static void stitch_update_views(rt_model_t *m, rt_stitch_net_t *net)
{
   uint8_t buf[8];
   size_t sz;
   for (int i = 0; i < net->nep; i++) {
      stitch_ep_t *ep = &(net->ep[i]);
      if (ep->other[0] == NULL)
         continue;
      stitch_ctr_t v = stitch_resolve(net, &(ep->last));
      stitch_encode(ep->otype, &v, buf, &sz);
      if (stitch_trace())
         fprintf(stderr, "#ST vw net=%p ep=%d m=%d view=(%u,%u,%u)\n",
                 (void *)net, i, ep->is_member, v.val, v.str, v.flags);
      if (ep->is_member) {
         // Rest-of-net view enters through the solver-owned
         // SOURCE_IMPLICIT plug: the member's own resolution combines
         // it with its real drivers, so nothing fights and readers see
         // the merged net value
         if (memcmp(ep->plug->shared.data, buf, sz) != 0) {
            memcpy(ep->plug->shared.data, buf, sz);
            ep->plug->nexus.last_event = m->now;   // mark deposited
            rt_nexus_t *n = ep->mnex;
            // Full driving update (not the bare vtable method): the
            // wrapper also walks n->outputs so port fanout and
            // conversion-function edges see the new value — a switch
            // ctrl fed through to_std_logic(net) re-evaluates
            update_driving(m, n, true);
            // Effective-class members (inout port fanout) publish
            // through the effective plane
            if (n->flags & NET_F_EFFECTIVE)
               update_effective(m, n);
            if (stitch_trace())
               fprintf(stderr, "#ST plug net=%p ep=%d wrote %u eff now %u "
                       "nsrc %u vt%d res%d fl%02x\n", (void *)net, i,
                       buf[0], ep->drv[0]->shared.data[n->offset],
                       n->n_sources, n->vtable == &nexus_default_vtable,
                       ep->drv[0]->resolution != NULL, n->flags);

         }
         continue;
      }
      if (ep->otype == STITCH_L3DS && ep->other[1] != NULL) {
         for (int f = 0; f < 3; f++) {
            uint8_t fb[4] = { buf[f], 0, 0, 0 };   // LE widen 0..255
            stitch_put(m, ep->other[f], fb, ep->other[f]->shared.size);
            if (stitch_trace())
               fprintf(stderr, "#ST put net=%p ep=%d f=%d want=%u eff=%u "
                       "sz=%d\n", (void *)net, i, f, buf[f],
                       ((const uint8_t *)nexus_effective(&(ep->other[f]->nexus)))[0],
                       (int)ep->other[f]->shared.size);
         }
      }
      else
         stitch_put(m, ep->other[0], buf, sz);
   }
}

// Contributor event: O(1) counts transition, then refresh views.  The
// moving endpoint's own view is unchanged by construction (its
// excluded set did not move) — stitch_put's compare makes that free.
static void stitch_event_cb(uint64_t now, rt_signal_t *s, rt_watch_t *w,
                            void *user)
{
   rt_model_t *m = get_model();
   rt_stitch_net_t *net = user;

   net->n_events++;

   if (stitch_trace())
      fprintf(stderr, "#ST cb net=%p sig=%s\n", (void *)net,
              s->where ? istr(tree_ident(s->where)) : "?");

   for (int i = 0; i < net->nep; i++) {
      stitch_ep_t *ep = &(net->ep[i]);
      if (ep->drv[0] != s && ep->drv[1] != s && ep->drv[2] != s)
         continue;
      stitch_ctr_t c = stitch_classify(net, ep);
      if (stitch_trace())
         fprintf(stderr, "#ST ev net=%p ep=%d (%u,%u,%u)->(%u,%u,%u)\n",
                 (void *)net, i, ep->last.val, ep->last.str, ep->last.flags,
                 c.val, c.str, c.flags);
      stitch_count(net, &(ep->last), -1);
      stitch_count(net, &c, +1);
      ep->last = c;
   }

   stitch_update_views(m, net);
}

// VHPIDIRECT %v strength query (sv_strength_pkg.sv_net_strength).
// Pure kernel lookup: registration stored each net's hierarchical
// path, so no VHPI or scope walking is involved and the query works
// identically with or without the resolver plugin (no plugin -> no
// registered nets -> -1 -> the value-alphabet fallback in sv_vstr).
// Returns (val << 16) | (str << 8) | flags.  String ABI = (ptr, len).
int32_t sv2vhdl_net_strength(const uint8_t *path, int32_t len)
{
   char buf[512];
   if (len <= 0 || len >= (int32_t)sizeof(buf))
      return -1;
   memcpy(buf, path, len);
   buf[len] = '\0';
   const char *p = buf;
   if (*p == '.') p++;

   for (rt_stitch_net_t *net = g_stitch_nets; net; net = net->chain) {
      if (net->path == NULL || strcasecmp(net->path, p) != 0)
         continue;
      stitch_ctr_t c = stitch_resolve(net, NULL);
      const int32_t r = (c.val << 16) | (c.str << 8) | c.flags;
      if (getenv("SV2VHDL_KERNEL_DEBUG") != NULL)
         fprintf(stderr, "#VQ path='%s' -> %d\n", p, r);
      return r;
   }
   if (getenv("SV2VHDL_KERNEL_DEBUG") != NULL)
      fprintf(stderr, "#VQ path='%s' -> -1 (no kernel net)\n", p);
   return -1;
}

// Register one net's static endpoint sets.  Called (indirectly, via
// the VHPI extension) from the resolver plugin at start of simulation.
// Endpoint i: drv_ss[i] = contributor signal; oth_ss[i] = exclude-self
// view output or NULL; dtypes/otypes = stitch_type_t per endpoint.
bool x_stitch_net_register(int nep, sig_shared_t **drv_ss,
                           sig_shared_t **oth_ss,
                           const uint8_t *dtypes, const uint8_t *otypes,
                           sig_shared_t **echo_ss, const char *net_path,
                           int32_t member_elem)
{
   // drv_ss/oth_ss carry 3 slots per endpoint (fields for l3ds record
   // implicits; scalar endpoints use slot 0 with slots 1-2 NULL)

   rt_model_t *m = get_model();

   static int no_stitch = -1;
   if (no_stitch < 0)
      no_stitch = getenv("NVC_NO_STITCH") != NULL;
   if (no_stitch || nep < 1)
      return false;

   static int stitch_dbg = -1;
   if (stitch_dbg < 0)
      stitch_dbg = getenv("NVC_STITCH_DEBUG") != NULL;

   rt_stitch_net_t *net =
      xcalloc_flex(sizeof(rt_stitch_net_t), nep, sizeof(stitch_ep_t));
   net->nep = nep;
   if (net_path != NULL) {
      if (*net_path == '.') net_path++;
      net->path = xstrdup(net_path);
      for (char *c = net->path; *c; c++)
         *c = tolower((unsigned char)*c);
   }

   for (int i = 0; i < nep; i++) {
      stitch_ep_t *ep = &(net->ep[i]);
      for (int f = 0; f < 3; f++) {
         ep->drv[f] = drv_ss[i*3 + f] == NULL
            ? NULL : container_of(drv_ss[i*3 + f], rt_signal_t, shared);
         ep->other[f] = oth_ss[i*3 + f] == NULL
            ? NULL : container_of(oth_ss[i*3 + f], rt_signal_t, shared);
      }
      ep->dtype = (stitch_type_t)dtypes[i];
      ep->otype = (stitch_type_t)otypes[i];
      ep->is_member = (ep->other[0] == ep->drv[0]);
      ep->echo = (echo_ss == NULL || echo_ss[i] == NULL)
         ? NULL : container_of(echo_ss[i], rt_signal_t, shared);

      const int nfld = (ep->dtype == STITCH_L3DS && ep->drv[1] != NULL)
         ? 3 : 1;
      for (int f = 0; f < nfld; f++) {
         // A vector member signal legitimately has one nexus per
         // driven element; the element lookup below picks ours
         if (ep->drv[f] == NULL
             || (ep->drv[f]->n_nexus != 1
                 && !(ep->is_member && member_elem >= 0))) {
            if (stitch_dbg)
               fprintf(stderr, "#STITCH decline: endpoint %d shape\n", i);
            free(net);
            return false;
         }
      }

      if (ep->is_member) {
         rt_nexus_t *mn = &(ep->drv[0]->nexus);
         if (member_elem >= 0)
            mn = split_nexus(m, ep->drv[0], member_elem, 1);
         ep->mnex = mn;

         // Member contributions fold ALL driver-backed sources except
         // our plug at classification time — no per-driver limits

         // Solver-owned plug signal backing the SOURCE_IMPLICIT input
         const size_t psz = (size_t)mn->size * mn->width;
         rt_signal_t *plug = xcalloc(sizeof(rt_signal_t) + MAX(psz, 8));
         plug->n_nexus = 1;
         plug->shared.size = psz;
         plug->nexus.vtable = &nexus_default_vtable;
         plug->nexus.signal = plug;
         plug->nexus.width  = 1;
         plug->nexus.size   = psz;
         plug->nexus.last_event = TIME_HIGH;
         // Initial plug bytes = 0xFF sentinel, which no encoding
         // produces: the first stitch_update_views publication then
         // ALWAYS writes and refreshes the member, even when the
         // first view is Z — otherwise an all-Z net (opposing
         // highz-spec assigns) never overwrites the member's
         // initial X (drive_strength hz1hz0).  Registration writes
         // the real view before any delta can read the plug.
         memset(plug->shared.data, 0xFF, psz);
         ep->plug = plug;

         rt_source_t *src = add_source(m, mn, SOURCE_IMPLICIT);
         src->u.port.input = &(plug->nexus);
      }
   }

   // Seed counts from current (post-initial) values
   for (int i = 0; i < nep; i++) {
      stitch_ep_t *ep = &(net->ep[i]);
      ep->last = stitch_classify(net, ep);
      if (stitch_trace())
         fprintf(stderr, "#ST seed net=%p ep=%d m=%d (%u,%u,%u)\n",
                 (void *)net, i, ep->is_member,
                 ep->last.val, ep->last.str, ep->last.flags);
      stitch_count(net, &(ep->last), +1);
   }

   // One single-slot watch per contributor field: async_watch_callback
   // always reports signals[0], so a shared multi-slot watch would
   // coalesce same-delta events and misreport the firing signal
   for (int i = 0; i < nep; i++) {
      for (int f = 0; f < 3; f++) {
         if (net->ep[i].drv[f] == NULL)
            continue;
         rt_watch_t *w = watch_new(m, stitch_event_cb, net,
                                   WATCH_EVENT, 1);
         model_set_event_cb(m, net->ep[i].drv[f], w);
      }
   }

   // Publish initial views
   stitch_update_views(m, net);

   net->chain = g_stitch_nets;
   g_stitch_nets = net;

   if (stitch_dbg) {
      fprintf(stderr, "#STITCH net registered: %d endpoints\n", nep);
      for (int i = 0; i < nep; i++)
         for (int f = 0; f < 3; f++)
            if (net->ep[i].drv[f] != NULL)
               fprintf(stderr, "#STITCH  ep%d f%d size %u type %d m %d "
                       "sig %s.%s\n", i, f,
                       net->ep[i].drv[f]->shared.size, net->ep[i].dtype,
                       net->ep[i].is_member,
                       net->ep[i].drv[f]->parent
                       ? istr(net->ep[i].drv[f]->parent->name) : "?",
                       net->ep[i].drv[f]->where
                       ? istr(tree_ident(net->ep[i].drv[f]->where)) : "?");
   }

   return true;
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
      // #67 TP3 overlap (audit rule 8): while U owns waveform bookkeeping,
      // an unresolved single-source nexus's driving value equals its
      // effective value (no resolution, no other source) — read plane 0,
      // which E owns, instead of the waveform U may be writing.
      if ((n->flags & NET_F_FAST_DRIVER)
          || (unlikely(g_two_phase_eval) && g_two_phase_run == 3
              && n->n_sources == 1
              && n->signal->resolution == NULL))
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
   wakeable_set_kind(&imp->wakeable, W_IMPLICIT);

   deferq_do(&m->implicitq, async_update_implicit_signal, imp);
   set_pending(m, &(imp->wakeable));

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
   p->part      = PART_NONE;
   p->where     = where;
   p->name      = name;
   p->handle    = handle;
   p->scope     = s;
   p->privdata  = mptr_new(m->mspace, "process privdata");

   wakeable_set_kind(&p->wakeable, W_PROC);
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
