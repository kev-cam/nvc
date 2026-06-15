# Parallel (SMP) Process Scheduling — Design Note

Status: design / not yet implemented. This note captures the intended
architecture so the kernel changes can be reviewed before any code lands.

## Motivation

nvc's simulation kernel currently executes processes **single-threaded**.
The thread pool exists but is used only off the model-evaluation path:
JIT background compilation (`src/jit/jit-core.c`, `async_do(jit_async_cgen,…)`)
and the parallel garbage collector (`src/rt/mspace.c`, which suspends all
threads and scans each stack — a stop-the-world barrier). The delta-cycle
process/driver execution itself, `deferq_run()` in `src/rt/model.c`, is a
plain serial `for` loop over `tasks[]`.

We want a parallel SMP scheduler so a large RTL design can be evaluated
across cores. This is the *bottom-up* half of the `--accel` strategy
(top-down accelerates a single subtree for one thread; bottom-up splits the
design across cores), and the same partition serves both an SMP interpreted
run and an FPGA/synthesized-subtree run.

## Why it's feasible: VHDL delta semantics

A parallel *evaluate* phase is determinism-safe by construction for
well-formed RTL. Within a delta cycle:

- processes read **settled** (last-resolved) signal values;
- a process schedules updates to **its own** drivers; the new values become
  visible only after the whole delta, in the update/resolution phase.

So concurrent process execution cannot observe a partial write, and the
result is independent of execution order — exactly the property that makes
the serial loop's order irrelevant today. Parallelizing the evaluate phase
therefore needs **no extra data-coherency work** for ordinary
`process(clk)` RTL. The work is entirely in the *scheduler plumbing*, not in
the semantics.

What is *not* automatically safe (see Caveats): VHDL shared variables /
protected types (updates are not deferred → order-dependent), and
foreign/VHPI side-effecting processes.

## Measured ceiling — stage 1a (VeeR-EH1:default:hello)

The first instrumentation increment (`NVC_PROFILE_PROCS`, model-level
per-delta proc-queue depth histogram + time-in-bucket; zero overhead when
off) confirms the workload is near-ideal for the split:

| woken procs / delta | deltas | % of process-eval time |
|---|---|---|
| 2–3   |  2 948 |  0.01% |
| 4–15  |  8 189 |  0.05% |
| 16–63 | 14 728 |  2.23% |
| 64–255 | 16 147 | 12.78% |
| 256–1023 | 13 201 | **69.45%** |
| 1024+ |  1 598 | 15.48% |

61 249 proc-running deltas, 13.3 M activations, 48.4 s in process eval.
**99.9% of eval time is in deltas that wake ≥16 processes** (≈85% in deltas
waking ≥256). So the serial fraction is negligible and the Amdahl ceiling is
set by the per-delta barrier and the GC stop-the-world, not by a lack of
width. Determinism held (cycles=1033, unchanged). Conclusion: parallelism is
well worth it here; the open question stage 1c answers is whether we are
GC-bound.

## Single-core table executor + the vtable scheme-switch

Two refinements that pay off before any threads:

- **Table-driven execution beats lists even single-core** (cache locality;
  `deferq_run` already prefetches `tasks[i+1].arg` because the work is
  cache-miss bound). So the reformed flat tables (§1) are a baseline win on
  one core, not only the substrate for the split.
- **Use the vtable hack to switch schemes — nvc already does this.**
  `rt_nexus_vtable_t` (`update_driving`/`deposit`/`read_source`/`notify`) is
  swapped per nexus between `nexus_default`/`single_driver`/`memo1`/`lazy`
  schemes via `n->vtable = &…`; `rt_proc_vtable_t` swaps process eval, and the
  `--accel` path *already* injects a synthesized statemachine by setting
  `proc->vtable = &wrap->vtable` instead of interpreting. So the scheme-switch
  for the executor (list ↔ table ↔ parallel, the fast/slow swap of §5) is the
  same idiom lifted to the queue executor: a `run_procq` function pointer (or
  a tiny executor vtable) selected at the barrier. It composes orthogonally
  with the per-proc eval vtable — the **table is the iteration scheme**, the
  **proc vtable is the per-proc eval scheme** (interpret or accel), so a
  table executor and accelerated processes coexist with no special-casing.

## Current structures (grounded in the code)

- `deferq_run(m, dq)` (`rt/model.c`): serial dispatch loop; already prefetches
  `tasks[i+1].arg` because the work is cache-miss bound.
- `async_run_process()` → `run_process()`: the single site every process
  activation flows through — the natural instrumentation/dispatch hook.
- `set_pending(wake)`: asserts `!wake->pending` and sets it — a process is
  enqueued and run **once per delta** even if several of its sensitivities
  fire together. Cost is therefore one-per-activation and unambiguous.
- `rt_nexus.pending`: a **tagged-pointer union** — NULL / tag==1 a single
  `rt_wakeable_t*` inline / tag==0 an `rt_pending_t` for the multi-waiter
  case. A clock nexus with hundreds of sensitive flops is the big
  multi-waiter case.
- `procq` etc. are `deferq_t` = a flat growable array `{tasks, count, max}`
  (already a table, not a list).
- `model_thread_t` (`__attribute__((aligned(64)))`): per-thread `tlab`
  (thread-local allocation buffer), `free_waveforms`, active obj/scope. So
  per-process allocation is already thread-local; cores do **not** contend on
  a heap lock for transient allocs.
- `workq` (`src/thread.c`), worker count capped by `NVC_MAX_THREADS`
  (default = CPU count).

## Architecture

### 1. Reformed per-core sensitivity tables (not linked lists)

At (re)partition, materialize the partitioned sensitivity as **flat tables**
rather than walking the tagged-pointer / `rt_pending_t` structures:

- A per-core **active/dispatch table**: contiguous rows, scanned by that core
  with hardware prefetch (the reason the serial loop already prefetches).
- Each row carries **execution data per slot**: `{proc_ref, ewma_cost,
  activation_count, trigger-refs}`. Co-locating the cost with the dispatch
  slot means the timed executor writes cost back to the row it just ran, and
  the rebalancer sums `Σ cost` over a core's contiguous slice — both hot in
  cache, no scatter into per-proc structs.
- The per-nexus sensitivity tables become **wake triggers** (a back-reference
  to the proc's home row). Cost lives **once** per proc in its home row
  (a proc sensitive to N nexuses must not carry N costs — `set_pending`
  dedups, so it runs once/delta).
- Each core's slice lives in **core-owned, 64-byte-aligned** memory
  (`model_thread_t` is already aligned) so per-slot cost writes never
  false-share across cores.

Build cost is paid cold (at repartition, infrequently); the contiguous scan
is the hot path every delta. Build-once / scan-many. Row indices double as
the **stable proc IDs** the tuner needs, and a fixed row order gives a
canonical (reproducible) execution order.

### 2. Parallel dispatch mechanism

In `deferq_run` (or a parallel variant), when the woken-process count exceeds
a threshold, dispatch each core's table slice to the `workq` instead of
running the serial loop. Because the partition is precomputed and each core
owns its slice, the wake step **marks** into a core-owned region rather than
appending to a shared queue — which sidesteps concurrent enqueue races on the
global `procq`/`driverq`. Cross-core driver updates are accumulated into
per-core `next_*` queues and merged at the delta **barrier** (the only safe
mutation point). The update/resolution phase can stay serial initially.

### 3. Per-slot execution profiling — the timed "second processor"

A **second, instrumented** executor variant brackets `run_process` with a
cheap cycle counter and writes an EWMA cost back to the proc's table row.
- Use `rdtscp` (a few cycles, invariant TSC, also yields the core id), **not**
  `get_timestamp_ns()` → `clock_gettime` (~20–30 ns vDSO call), which would
  both add overhead and distort cheap processes.
- **Sample**, don't measure forever: run the instrumented variant only during
  bounded windows; aggregate over many activations so per-call jitter washes
  out (you need relative weights, not precise ns).
- The instrumented variant is swapped in only when profiling; steady state
  runs the lean executor with zero timing overhead (observer effect isolated).

This profile is consumed by three clients: the SMP rebalancer (per-core load
`Σ f·C`), the accel subtree selector (which subtrees are hot enough to
synthesize), and the speed work (it pinpoints the time sinks — on VeeR, the
`logic3d` alloc-churn processes — i.e. a continuous, cheap replacement for
hand gdb-sampling).

### 4. Cold-path partition tuner (callout to Python)

Policy is separated from mechanism. A planner — implemented in Python so the
partition algorithm can be iterated without recompiling nvc — receives the
communication graph and runtime profile and returns a `proc→core` (or
`scope→core`) map. **It is never on the per-delta hot path:** consulted once
after elaboration for the initial partition, then periodically/triggered at
the barrier, ideally **asynchronously** (kick it off in the background, keep
simulating on the current map, apply the new map at whatever future barrier
the answer lands).

- Inputs: the graph from `rt_nexus.sources` (driver/output edges) and
  `rt_nexus.pending` (sensitivity/input edges), plus per-proc
  `{activation_count, ewma_cost}`.
- Objective: **balanced min-cut** — minimize cross-core shared nexuses
  (barrier sync traffic) subject to balanced per-core load. Even-splitting a
  clock list balances count but scatters tightly-coupled logic and the sync
  dominates; the graph partition is the point.
- Granularity: partition at **scope/instance** level (IFU / DEC+EXU / LSU,
  core vs dma/dbg), which keeps the graph tractable and is the **same**
  boundary the accel bottom-up subtrees want — one tuner, two backends.

### 5. Two-phase lifecycle (bootstrap → fast → profile-on-drift)

Mirrors nvc's tiered JIT (profile hot → run lean):

1. **Bootstrap** from the static-graph min-cut partition (immediate, no
   profiling) and run the **fast** executor right away — short runs (e.g. a
   ~1000-cycle VeeR hello) then pay no profiling cost they can't amortize.
2. **Profile on demand**: if the run is long or the drift detector fires,
   swap in the instrumented executor for a sampling burst and repartition.
3. **"Balanced"** = the `proc→core` map stops moving **and** max-core/mean
   load stays under threshold for K consecutive windows (hysteresis — never
   declare convergence on one window).
4. **Refreeze** to the fast executor.

The fast executor is faster precisely because it sheds what the slow one
carried: no cycle-counter brackets, no EWMA write-back, no sampling gate, no
per-element policy check — just a tight contiguous scan of a fixed per-core
slice.

**Drift re-arm:** keep only a *coarse, near-free* drift detector in the fast
phase — one wall-time stamp per core per window, or the activation-count
imbalance (already free) — **not** per-call timing. Sustained drift past
threshold → re-arm a profiling burst. So it is fast-by-default, profile-on-
drift, not strictly one-way.

### 6. Instant fallback + core release

The serial `deferq_run` is always available and is the **reference**
implementation, so it is the safe fallback for any uncertainty: a small
`procq`, a sensitivity change, a detected hazard (shared variable / foreign
process in the active set), an error, or simply an idle stretch. At any
barrier the scheduler may **revert to serial list processing and release the
worker cores** back to idle / the OS. Parallel execution is thus an
optimization layered over a correct serial core and is droppable instantly
with no correctness risk and no lingering thread occupancy.

### 7. Core budget and the straggler problem

**Do not use all cores.**

- The OS needs cores (scheduler, IRQ handling), and nvc *itself* already runs
  worker threads for JIT background compilation and the GC — those compete
  with sim workers. Budget for them.
- The scheme is bulk-synchronous: a delta advances only when **every** core
  has finished its slice, so the **slowest core sets the pace**. One core that
  is preempted, contended, thermally throttled, or simply assigned too much
  work stalls all the others — making N cores slower than N−k. Oversubscribing
  past the physical core count is strictly harmful here.

Implications: default the worker count to **below** `nproc` (leave headroom,
e.g. `nproc − 2`, floored sensibly), and make the load balancer **cost- and
speed-aware** rather than count-even — a slow core should be given
proportionally less work (the per-slot cost data feeds this). Combined with
§6, if a core becomes a persistent straggler the scheduler can drop it from
the rotation (or fall back to serial) rather than let it gate the barrier.

## Correctness & determinism

- Results are invariant to the partition and to serial-vs-parallel execution
  (order-independent evaluate). Switching executors, repartitioning, or
  releasing cores mid-run does not change the answer.
- For reproducible debugging, **record the converged partition** and the
  phase transitions; a replay run loads the recorded partition and goes
  straight to the fast executor, reproducing the schedule, not just the
  result.

## GC coupling — a scaling prerequisite

The per-thread `tlab` removes allocation *lock* contention, but allocations
still feed the GC, and nvc's GC is a stop-the-world barrier across all
threads. Per-cycle allocation churn (e.g. the `logic3d` ops in
`lib/sv2vhdl/logic3d_types_pkg.vhd` that return unconstrained vectors by
value, allocating per call) would trigger frequent global GC and re-serialize
the cores. **Reducing that churn (in-place / preallocated logic3d results) is
a prerequisite for the parallel split to scale**, not an independent task — it
helps single-thread today and unblocks SMP later. Stage-1 measurement will
show whether a run is GC-bound (if locked-parallel barely helps, it is).

## Caveats / unsupported constructs

Fall back to serial for processes that touch:
- VHDL **shared variables** / protected types (non-deferred, order-dependent);
- **foreign/VHPI** side-effecting bodies;
- anything with **dynamic sensitivity** (`wait until`/changing waits) that
  would invalidate a static table — keep these in a small dynamic spill list
  rather than forcing them into the per-core tables.
Pure `process(clk)` RTL — the dominant case and the big clock-sensitivity list
we target — is safe.

## Staged implementation plan

1. **Measure the ceiling.** Gate on `procq.count > threshold` → dispatch slices
   to the `workq` with the shared queues **locked** (correct but contended),
   behind `NVC_PARALLEL_PROCS`, falling back to the serial loop. Validates
   determinism on real RTL and reveals whether we are GC-bound — one VeeR run.
   Add the timed executor + per-proc `{count, ewma_cost}` and the graph/stat
   **export** so a Python partitioner has its full input.
2. **Remove contention.** Per-core reformed sensitivity tables + per-core
   `next_*` queues merged at the barrier; drop the shared-queue locks.
3. **Tuner.** Cold-path Python callout (balanced min-cut, scope granularity),
   async at the barrier; stable IDs from table row indices.
4. **Two-phase + re-arm.** Static bootstrap → fast executor → drift/long-run →
   instrumented profiling until balanced → refreeze; cheap drift detector;
   core budget + straggler drop; record/replay.

## Configuration (proposed)

- `NVC_PARALLEL_PROCS` — master enable (default off; falls back to serial).
- `NVC_MAX_THREADS` — existing worker cap; the budget defaults below `nproc`.
- a tuner selector (e.g. `NVC_PART_TUNER=python:script.py` or a socket).
- a profiling-window / drift-threshold knob.

## Open questions

- NUMA: pin cores + partition memory by socket? (cross-socket nexus traffic
  is far costlier than the barrier itself).
- Update/resolution phase: keep serial, or partition by signal once the
  evaluate phase scales?
- Interaction of the sim worker pool with the JIT-compile and GC pools — one
  shared `workq` with priorities, or separate pools?
