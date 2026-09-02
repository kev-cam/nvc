# TODO: tighten NVC ↔ Yosys coupling

**Decision:** use Yosys as-is. Link `libyosys.so`, interpose with ldx where
needed. No Cameron EDA fork of the Yosys repo.

## 0. Identify the seam first (blocker for everything below)

Determine where the current looseness actually is:

- [x] Confirm whether the existing RTL codegen path is `write_cxxrtl`.
      **DETERMINED (2026-08-22): it is NOT.**  `gen_statemachine.cpp`
      already links libyosys IN-PROCESS (`yosys_setup()` +
      `run_pass("read_verilog …; hierarchy; proc; flatten; opt -keepdc;
      dffunmap; opt_clean")`) and then walks RTLIL cells itself to emit
      its own C (the state-machine form).  `write_cxxrtl` exists only
      as the GSM_CXXRTL A/B side-mode, and `techmap; simplemap;
      write_json` only feed the scan/certify (ATPG) path.  **No `abc`
      anywhere** — the §3 ABC concern is moot for sim codegen, as
      suspected.
- [x] If the seam is CXXRTL: scope getting NVC's scheduler to drive
      eval, rather than CXXRTL's.  N/A — NVC's scheduler already owns
      eval; gsm-generated chunks are `.so`s driven by the accel bridge.
- [x] If the seam is the Yosys API: proceed with §1.  **The actual
      seam is neither: it is the NVC ↔ gen_statemachine PROCESS
      boundary** (model.c fork/execs gsm per chunk/merge, round-
      tripping emitted Verilog in and generated C out, then gcc →
      `.so` → dlopen).  §1's remaining work is therefore "move gsm's
      libyosys usage into the nvc process" — with the caveat that
      today's process isolation is what makes a yosys `log_error`
      non-fatal to the simulator: in-process linking makes §3's
      exit/abort interposition MANDATORY on day one, and §4's
      threading confinement real (nvc is multithreaded; gsm today is
      one process, one thread).  "Construct RTLIL::Design directly
      from NVC's elaborated tree" additionally bypasses vhdl2vlog +
      read_verilog — note vhdl2vlog carries the Verilator-match
      translation semantics and width-identity fixes, so that step
      moves those obligations into the RTLIL builder.

## 1. In-process Yosys

- [x] Build Yosys with `ENABLE_LIBYOSYS=1`.  Already done:
      gen_statemachine dynamically links
      `/usr/local/src/yosys-build/libyosys.so`.
- [x] **(2026-08-30) gsm is now a library.**  `gen_statemachine.cpp`
      builds two ways: the CLI (unchanged behaviour, byte-identical
      output) and `yosys/libgsm.so` (`-DGSM_LIB`), whose one export is
      `gsm_generate(nargs, args, log_path)`.  The CLI main() is a
      wrapper over the same entry, so the two clients cannot drift.
      Run-once → reentrant: `design -reset` + a reset of all ~20
      file-scope globals per call; `yosys_setup()` once, never shut
      down.  Proven: 3 calls in one process (accept / yosys-log_error
      decline / accept), call 3 byte-identical to call 1.
- [x] **(2026-08-30) nvc calls it in-process** on the
      `accel_bg_compile` path (the vhdl2vlog-leaf path, which had NO
      synth timeout to lose): dlopen `RTLD_LOCAL`, rc contract
      0=generated / 1=clean decline (no CLI retry — deterministic) /
      2=contained yosys error (CLI retried as a safety net).
      `NVC_ACCEL_NO_GSMLIB=1` forces the CLI; `NVC_GSM_LIB` overrides
      the .so path.
      **Caveat found while testing (2026-08-30): the legacy non-JIT
      path was rotted independently of the conversion** — wide_n8w256
      under `NVC_ACCEL=1 NVC_ACCEL_FROM_VHDL=1` (no `NVC_ACCEL_JIT`)
      installed its chunk and then produced no Y= in both variants.
      **DELETED later the same day** (~440 lines: accel_bg_thread /
      accel_bg_compile with its smak submission leg / accel_load /
      accel_binding_t / the sm_init_mapped .so contract / the
      NVC_USE_ACCEL prebuilt-.so load).  `NVC_ACCEL=1` alone now runs
      the aj engine (`NVC_ACCEL_JIT` is a harmless no-op), and the
      formerly-dead config produces correct values through aj.  This
      also removed the bg-compile in-process call site — its purpose
      (proving the facade) was served; the aj fork-worker is the sole
      client now.
- [x] **(2026-08-30, same day) The accel-jit paths converted via the
      fork()-without-exec worker** (`aj_gsm_spawn`), which resolves
      both constraints at once:
      1. *Watchdog:* the CHILD arms its own `alarm()` (SIG_DFL,
         unblocked) — SIGALRM terminates even a compute-bound yosys
         pass, `aj_synth_timed_out()` counts SIGALRM as a timeout, so
         the `exceeded Nds` degrade-to-decline contract survives
         without `/usr/bin/timeout`.  Proven: `GSM_TEST_SLEEP` hangs
         killed at the 2s deadline (opt_asserts check 8b gates it).
      2. *Parallelism:* the merge pool keeps its fork-per-group shape
         (spawn returns a pid; the blocking `waitpid(-1)` reap loop is
         unchanged) — each child gets its own copy-on-write yosys, so
         nothing serializes on the facade mutex.  The parent-side
         mutex is NOT taken; a child forked while another thread held
         it deadlocks its copy and the alarm reaps it (self-healing).
      `GEN_STATEMACHINE` (an explicit generator binary) forces the
      exec'd CLI everywhere — that env now means "use this binary",
      and it is what the sleepy gate test exercises.  Cache freshness
      re-keyed: `aj_synth_tool()` stats the dladdr'd libgsm.so path in
      fork mode, the CLI binary otherwise.
      *Residue for direct-RTLIL:* a child can inherit parent-constructed
      RTLIL by fork, so the fork-worker shape stays valid when the
      vhdl2vlog round-trip goes away — but anything the parent wants
      BACK from synthesis must come through files or a pipe, not
      memory.
- [ ] Construct `RTLIL::Design` directly from NVC's elaborated tree.
      No Frontend/Pass plugin required — that machinery only exists to
      add `read_*` commands to the yosys CLI. NVC is the host process.
      **STARTED (2026-08-30): the construction facade exists** —
      `gsm_rtlil_*` in gen_statemachine.cpp (begin/module/wire/
      cell_bin/cell_un/cell_mux/connect/proc/sync/sync_assign/
      content_hash/synth/abort; string sigspecs, typed-helper widths).
      Sessions hold the facade mutex begin→synth; errors poison the
      session instead of unwinding into the host; synth reuses gsm_run
      with read_verilog/chparam skipped.  PROVEN on the rtoy fixture
      (yosys/rtlil-selftest/, wired into accel-gate): in-process
      two-session byte determinism (a builder-mode canonicalization
      renames pass-invented `$auto`/autoidx names — the read_verilog
      path structurally cannot offer this), and 64-cycle driven
      behavioral equality with the text path.  Key learnings: async
      reset is a LEVEL sync (ST0/ST1) paired with one edge sync, NOT a
      second edge; `gsm_rtlil_content_hash` (FNV over the call stream)
      replaces the .v file bytes in the vhash cache key.
      **Increment A (same session): decision trees** — switch/case
      API (read_verilog's hold pattern: root action `temp = reg`,
      branch overrides, sync commits `reg <= temp`); canonicalization
      means NO client-side mux pre-lowering is needed — `proc` may
      invent names freely.  Selftest covers an enable-gated hold
      register through the tree form.
      **Increment B plumbing (same session):** `src/gsm_rtlil.h`
      function-table header + model.c `accel_gsm_rtlil_api()` (probe
      resolves the full surface or returns NULL → text fallback).
      **WALKER LANDED (same session, v1 subset): `vhdl2rtlil_module`**
      in vhdl2vlog.c + `aj_rtlil_subtree`/`aj_rtlil_spawn` in model.c
      behind `NVC_ACCEL_RTLIL=1` (folded into vhash — separate cache
      namespace).  The fork child walks the CoW tree, constructs via
      the builder, synthesizes — NO Verilog parse; any walker decline
      exits 3 and the parent falls back to the text path.  **wide and
      deep synthesize FULLY through the builder with checksums equal
      to interp and byte-deterministic output across runs**; fsm and
      arst joined them the next day (T_CASE via multi-compare
      switch cases; conversion-stripping in the const extractor —
      reset values arrive as std_logic_vector(to_unsigned(C,W))).
      regf turned out to be constant SLICE
      targets + `when others => null` (its 512-bit vector is flat, not
      an array) — landed next: slice/bit lhs on the hold temp, T_NULL.
      **ALL 5 suite shapes now FULLY parse-free.**
      **$mem_v2 MEMORIES LANDED (2026-08-31):** true array memories
      construct directly — RTLIL::Memory + async $memrd reads +
      MemWriteAction writes on the edge sync (only the ENABLE threads
      the decision tree; addr/data are unconditional comb gated by EN;
      later same-mem writes get priority — VHDL sequential order).
      Whole-array positional writes expand per word (element i → word
      size-1-i, the text path's validated convention).  Proven by the
      selftest's dynamic write/read memory (64-cycle trace equality,
      real traffic) and opt_asserts 13's memrf fixture — where the
      oracle is a 100-cycle HARNESS DIFF of the two paths' generated C
      (clk-only chunks do not install into the sim: a pre-existing
      quiet stop after bridge emission, see finding below).  **NBA IDIOM LANDED (2026-08-31):** the tgt-vhdl register form
      (shadow pre-copy / edge-if / wait-for-0 / commit / trailing
      wait) walks parse-free — the shadow var is an ALIAS for the
      hold temp (pre-copy IS the root action, commit IS the sync
      assign, both elided); reset-edge sensitivity (falling_edge(rst)
      or rising_edge(clk)) resolves via areset_of with the reset edge
      subsumed by the level sync.  With it landed: T_DEPOSIT (comb
      deposits), straight-line process-local variables by PURE
      EXPRESSION SUBSTITUTION (the read_verilog technique; branch
      writes decline), constant-range T_FOR unrolling with the index
      substituted per iteration (sv_and's reduction loop walks), the
      l3d vocabulary via the text path's own vlog_l3d_op table (+
      l3d_bit_read/part_read as shr+select, ternary_*→mux, signed
      relationals), cell_inst brace-aware conns (concat actuals), and
      LOGIC3D CONSTANT DECODE: logic3d is natural 0..7 with bit0 =
      value plane — a folded literal renders as (v&1), exactly the
      text path's emit_lit; metavalue init bits render 0 ({N{1'b0}}
      parity).  translated.sh asserts bchunk walks decline-free under
      NVC_ACCEL_RTLIL=1.  **VeeR EH1a CENSUS RUN (2026-08-31, whole-subtree admission,
      SRAM-skip): TEST_PASSED cycles=1113 with the walker in the loop
      — correctness invariant held at 19,093 subtree attempts.**
      92.8% walked clean; the walked-clean chunks then hit gsm's OWN
      admission declines (17,717 × rc=1, overwhelmingly sv_* comb-only
      glue — correct), and a wasted text retry per clean decline was
      removed (rc==1 from the rtlil child is FINAL — deterministic).
      ZERO rtlil installs yet, and the census shows why in one number:
      1,177 of 1,376 walker declines are `var-assign@if-cond` — the
      rvclkhdr-family LATCH shape.  Every register-bearing subtree
      contains a clock header, so THE unlock for VeeR is the
      **persistent-variable/latch increment**: VHDL process variables
      persist across activations, so a branch-written variable is
      latch STATE — encode it as a pseudo-target (own wire + hold
      temp + `always` sync → proc infers $dlatch exactly as
      read_verilog does), with the trailing `sig <= var` copy becoming
      sync_assign(sig, g0_var) (the hold temp IS the post-tree value).
      That also puts walker-built designs in reach of GSM_ICG2EN's
      latch→enable rewrite (shared pipeline) — the proven whole-core
      path.  **LATCH INCREMENT LANDED (2026-08-31): persistent variables as
      pseudo-targets** — pv wire + hold temp rooted at pv (or at the
      last straight-line value when written unconditionally first —
      then it is a plain temp, nothing persists), branch writes into
      the hold, always-sync commit → proc infers the $dlatch; the
      trailing `sig <= var` copy shares the var's hold temp as the
      signal's SYNC SOURCE (a root action would read the pre-branch
      value: case actions evaluate before switches).  Results:
      translated fixture walks with ZERO declines; EH1a walker
      declines 1,376 → 532; plain census PASSES (cycles=1113) with
      454 installs, mostly rtlil-built — THE FIRST RTLIL INSTALLS ON
      VeeR.  Under GSM_ICG2EN the walker and text arms behave
      IDENTICALLY including an identical watchdog at cycles=99992 —
      the per-chunk + GSM_ICG2EN composition diverges PRE-EXISTINGLY
      on the text path too (icg2en shipped via the MERGE flow) —
      filed as its own item, not a walker defect.
      **TAIL SWEEP (2026-08-31 late): walker declines 532 → 64
      (99.66% of 19,093 EH1a walks clean).**  The 468-strong
      multi-edge class fell to the READ_VERILOG ENCODING: when the
      reset-if nests INSIDE the two-edge guard (rvdff shape), emit
      BOTH edge syncs carrying the same actions and let yosys's
      proc_arst resolve the inner reset switch — an unmatched shape
      becomes a contained proc_dff error → rc 2 → text retry (safe).
      Also: bit decode resolves refs through NAMED CONSTANTS
      (r2_bit_of_tree); portmap conns buffer 16K; funnel reasons.
      Remaining 64: 30 process@seq-assign, 19 odd OTHERS literals
      (still — not const-refs; needs a sample), 6 unfunneled, small
      residue; dynamic vector part-select writes; **MERGE POOL RTLIL LANDED (2026-09-01):** phase 1 captures a
      wrapper PLAN alongside its text emission (per-member connection
      lists + internal wire decls — no decision duplication; text
      stays authoritative), and the merge child walks every member's
      subtree with shared dedup, constructs the wrapper via
      cell_inst using the WALKER'S OWN module names (the text path's
      _h%08x content-hash renames belong to its file dedup), and
      synthesizes — no Verilog parse for the group.  GALS merged
      chunk MERGE-ACTIVE with Y==interp; translated merged pass
      engagement-gated ("via rtlil merge builder"); EH1a MERGE census
      in FULL PARITY with the fork A/B baseline (TEST_PASSED 1113,
      2 merges, the known ICG mergefail, 129 installs) with only 21
      walker declines end to end.  TRAP fixed en route: the plan must
      be freed AFTER the group stash, not at the ports/body free —
      stashing freed pointers gave the child CoW garbage.
      With this, BOTH synthesis paths construct directly; the text
      round-trip survives as cache key, staged evidence and fallback.
      **DYNAMIC BIT-WRITES (2026-08-31):** a non-constant single-bit
      index write on a vector signal lowers to a masked whole-target
      compose from ordinary cells — `g0 = (pre & ~(1<<idx)) |
      (val<<idx)`.  The compose reads the PRE-activation value (the
      SIGNAL — reading the post-mux g0 wire from module-level cells
      is a combinational loop), so a second dynamic write to the same
      target in one process declines (`dyn-multi`); the NBA-shadow
      sites are single-write.  Validated three ways (rtlil == text ==
      interp on the dynw fixture, now opt_asserts check 14) and
      EH1a census 6 TEST_PASSED 1113 with declines steady at 64 —
      the VeeR dynamic sites live inside already-walked processes.
      **ICACHE FRONTIER CLEARED (2026-08-31):** probing ic_data/ic_tag
      (the 8704-bit way_data monster — text synth FAILS on these, so
      the walker is the only viable acceleration route) peeled six
      stacked blockers, each a generality win: (1) `(others => ref)`
      replication expands to a literal concat of total/element copies;
      (2) continuous assigns to slice / const-indexed targets connect
      via the target's sigspec (RTLIL connections take any LHS);
      (3) the libgsm sigspec parser now splits concats at DEPTH-0
      commas and recurses — nested `{{a,b},c}` used to shear into
      garbage names (`tmp_ivl_182}`) and poison the session; (4)
      uniform `(others => ...)` memory initializers DROP, matching the
      text path's bare-reg-array emission; (5) the walker target table
      grew 64→256 (the cap silently skipped collection → target-miss);
      (6) memories qualified through the NBA-SHADOW idiom on the text
      path's exact census (shadow_find/shadow_scan) — the walker's
      alias machinery already routed shadow writes to memwr, only the
      qualifier refused.  Both icache chunks now walk CLEAN end to
      end; the remaining blocker is the gsm derived-clock protocol
      (rvclkhdr gated clocks internal to the chunk — same decline on
      both backends, owned by the merge/GSM_GATED flow, not the
      walker).  EH1a census 7: declines 64 → 20 (99.9% of 19,093
      walks), TEST_PASSED 1113.  Residue now: 9 stmt-kind@var-subst,
      6 target-miss TMP_IVL_* (named funnels), 3 array-ref, 2 misc.
      **ICACHE INSTALLED (2026-09-01):** with `NVC_ACCEL_ICG2EN=
      ic_data,ic_tag` (NEW: chunk-scoped ICG→enable — child setenv's
      GSM_ICG2EN under an ONLY-style token match; "+icg2en=scoped"
      vhash fold keys it apart from plain and global configs; merge
      groups stay global-only) both icache chunks go ACTIVE inside
      the full EH1a run: walker-built RTLIL memories + icg2en
      reclocking flops AND $memwr ports (`mem port reclocked ...
      en&=`), TEST_PASSED 1113, declines still 20.  Scoping exists
      because global per-chunk GSM_ICG2EN has the known pre-existing
      composition watchdog.  MEASUREMENT NOTE: EH1a wall clock is
      DECLINE-STORM dominated (~57k synth notes vs ~2.2k reuse per
      run — the ~17k comb-only glue declines re-attempt EVERY run,
      warm or cold; the 1113-cycle sim is seconds).  The icache
      kernel-time payoff needs the whole-core / longer-program
      vehicle; the next pipeline wall-clock lever is NEGATIVE-RESULT
      CACHING for clean declines (rc=1) keyed on the same vhash.
      **DECLINE CACHE LANDED (2026-09-01):** a clean decline (exit 1)
      writes `<dutc>.decline` beside the would-be output — same vhash
      key, so source/tool/env changes invalidate automatically; the
      marker stores the GSM_ALLOW_COMB value (it flips comb declines
      but is NOT in the vhash) and a mismatch ignores the marker;
      timeouts/abnormal exits never write one.  EH1a: cold 758s
      writes 86 markers; the next run resolves 18,814 decline hits
      from them with ZERO synth forks — wall 58s, a 13x pipeline
      improvement, byte-identical outcome (TEST_PASSED 1113, 451
      ACTIVE).  opt_asserts 15 gates it: marker fires + honored +
      GSM_ALLOW_COMB bypass re-attempts (comb chunks still don't
      install in-run — the 0-register install stall, pre-existing).
      **CONST INTERPRETER + LOOP UNROLL (2026-09-01):** the walker
      gained `r2_eval_int` — a pure-try constant interpreter over the
      substitution environment (l3dv const vectors decode by value
      plane, signed by width; To_Integer/resize/l3d_index identity
      forms; l3d_lt_s-class signed compares; quoted-operator
      arithmetic).  On it: T_WHILE counting loops UNROLL at walk time
      (yosys rejects procedural while outright — walker-only ground);
      T_LOOP sensitivity wrappers (`<init>; loop <body>; wait; end`)
      splice inline exactly as the text path emits them; var-subst
      stores constant values as INTEGERS (keeps induction evaluable;
      negative values keep no sigspec but stay computable — the OOB
      guard idiom needs exactly that); statically-constant if-conds
      PRUNE (true arm inline in the current case scope, false arm
      vanishes); every index/bound fold site takes an eval fallback.
      Also: the rtlil vhash now mixes /proc/self/exe mtime — the
      walker lives in the nvc binary, and without the fold a walker
      fix was MASKED by stale cached synths and decline markers.
      EH1a census: declines 20 → 16 (pic_ctrl + lsu_bus_intf clear),
      TEST_PASSED 1113.  Remaining 16 across 10 modules: local
      FUNCTIONS (lsu_bus_buffer f_Enc8to3/Ternary_*), shared-variable
      tmp_ivl targets (target-miss class), indexed writes to local
      vector variables (var-assign class), + singles.
      **RESIDUE SWEEP 2 (2026-09-01): declines 16 → 9 (99.95% of
      19,093 walks).**  Landed: USER-FUNCTION INLINING (straight-line
      pure bodies — var-assigns + one trailing valued return — bind
      params as substitutions, walk, render the return; per-bit
      result builds compose via a bit table; whole subst table
      snapshot/restored around the call since generated code reuses
      i/j across scopes); DYNAMIC BIT-READS of plain vectors lower to
      shr + [0] (the l3d_bit_read lowering, now for direct indexing);
      BITS-MODE SUBSTITUTIONS (a local vector built per-bit/per-slice
      becomes a bit table; constant whole values seed it, dynamic
      whole values land on a temp wire and seed as indexes; reads
      compose an MSB-first concat); RECURSIVE operand-width fallback
      (unconstrained operator chains carry width arbitrarily deep);
      target table now 4096 entries in a static backing store (the
      giant decode/tlu modules carry THOUSANDS of tmp_ivl deposit
      targets — the cap surfaced as target-miss until named).
      Remaining 9 (6 modules): lsu_bus_buffer 256-wide bit-built
      locals (>128 cap; compose would also breach R2_SPEC),
      per-bit OOB signal writes needing width coercion at
      case_assign (dec_gpr g0[0:0] vs 32-bit rhs), unconstrained
      target widths (dec_decode g0_cam_in), dynamic part-select
      slice bounds (dma_ctrl k3/k3), two l3d-bin poisons downstream
      of the same shapes.  All decline→text, correctness unaffected.
      **POISON ELIMINATION + ALIAS SOUNDNESS (2026-09-01, same day):**
      every gsm-session poison is gone (census: 9 declines, 0
      poisons, TEST_PASSED 1113).  The fixes: (1) locals can NEVER
      render as bare wire names — an unresolved variable read/element
      declines cleanly (var-read/var-elem funnels) instead of
      poisoning the session with 'no wire v_...'; (2) NBA-shadow
      alias READS are gated on a per-alias `wrote` flag — before any
      write through the alias the signal IS the value (pure pre-copy
      semantics) and the read renders as the signal; after a write it
      must see the WRITTEN value, which the signal does not carry, so
      it declines (alias-raw).  The unsound render-as-signal-always
      variant existed for one probe cycle and never landed; walk
      order makes the flag exact ("any write before this read").
      (3) slice case_assign values wider than the slice coerce
      through a temp (connect + low slice); (4) g0/pv hold wires are
      PROCESS-SCOPED (g0p<idx>_) — two processes driving one signal
      collided on wire g0_<sig> and poisoned the walk of the second.
      Remaining 9 (6 modules): comb-process shadow locals read at
      dynamic indices, 256-wide bit tables, dynamic part-selects,
      one unconstrained temp width — all clean named declines.
      **EH2 GENERALIZATION (2026-09-01):** first walker run on
      VeeR-EH2 (dual-thread core, fresh nw lib from the standard
      recipe; interp anchor TEST_PASSED cycles=2519): **7 walker
      declines against 738 installed chunks, zero poisons** — the
      decline classes are exactly the EH1 residue (3 slice-bounds
      k3/k3, 2 var-elem shadow reads, 1 var-assign under case depth,
      1 concat-size).  The walker generalizes.  Full-core EH2 accel
      correctness is separately blocked by the PRE-EXISTING (July
      2026, pre-rtlil) ICG-of-clk divergence — watchdog at
      cycles=99992 with the core held in reset, identical to the
      documented text-path behaviour; the unlanded fix is per-group
      ICG-of-clk detection (veer_eh2_rerun_recipe memory).  Not a
      walker defect; both backends meet the same fate.
      **ICG-OF-CLK AUTO-DETECT — refreshed design (2026-09-01, next
      opener):** the July line numbers are stale; at HEAD the pieces
      are: extra-clock decision + input gate at gen_statemachine.cpp
      ~1776-1808 (extra_clocks[] + clk_group per register); the
      advance-mode machinery in model.c ~7325-7400 (value-edge
      default, NVC_ACCEL_CK_COINCIDENT global fold, CK_LATE snapshot
      commits, ck_last posedge-clear with CK_KEEPLAST escape) — the
      comment there already names per-group auto-detect as the
      general landing.  KEY STRUCTURAL FACT: per-chunk extra clocks
      are boundary INPUTS (internal gated clocks decline), so the
      consumer chunk CANNOT see the ICG cone — the flag must come
      from the PRODUCER side.  The icg2en matcher already recognizes
      both the internal cone (reclock path) and the EXPORTED gated
      clock ('icg2en: exported gated clock X held via Y', the
      Moore-ize path) — reuse it in DETECT-ONLY mode: producer gsm
      emits `sm_icg_clock_outputs[]` naming its exported ICG-of-clk
      pins; the parent records net→flag at install; consumers get a
      per-group coincident/late choice at THEIR install by mapping
      sm_extra_clocks[] pins back to flagged nets.  Validation
      vehicle ready: /home/claude/eh2_rtlil (warm cache + markers,
      interp gold cycles=2519; accel currently watchdogs 99992).
      Blanket CK_COINCIDENT measured no-effect on free_l2clk's group
      (July) — the per-group mapping is the fix, not the env.
      **#43 THIRD+FOURTH HALVES LANDED (2026-09-02):** the t=0 seed
      now runs a MULTI-PASS FIXPOINT (default 3, NVC_ACCEL_SEED_PASSES
      knob): the topo sweep cannot order cyclic chunk meshes (EH2
      DEC↔LSU: 36 audited unseeded t=0 reads), but after pass 1 every
      producer has seeded, so pass 2's back-fill memcpy hands every
      consumer real bytes — NOT the July destructive drain.  And both
      AO maps register PER NEXUS with rim offsets (first-nexus-only
      registration width-skipped every sliced pin's back-fill; port
      sources are width-aligned per nexus, so per-nexus entries make
      the identity check complete).  Gated: opt_asserts 16 — a
      chunk-level-cyclic bit-acyclic fixture (seedcyc) that NO single
      seed pass can converge in any install order; SEED_PASSES=1 is
      the biting negative control.  seedord + all 12 seed-required
      reproducers pass.  EH2 VERDICT: cycle-0 lsu_arv STILL diverges
      (2 vs 6) → the remaining defect is NOT chunk→chunk seeding.
      WORKING THEORY (next EH2 stretch): seed deposits leave no
      DRIVER, so the first scheduler drain recomputes port hops from
      'U' and DESTROYS seed values on their way to INTERP readers
      (the TB reads lsu_arv through interp glue — the memcpy
      back-fill cannot cover that path; the July correction documents
      exactly this destructive recompute).  Candidate fixes: seed
      via a driver-backed deposit (survives update_driving), or
      re-deposit outputs after the init drain; sequencing is
      scheduler-delicate.  Diagnose with NVC_ACCEL_OUT_TRACE (seed
      pass sentinel d=4294967295) on the lsu_arv cone.
      **EH2 DIVERGENCE LOCALIZED (2026-09-02, bisection complete):**
      the tick "2-vs-6" was a STRENGTH-PLANE artifact (l3d position
      print; both logic-0) — the t=0 theory was wrong (the #43
      fixpoint stands on its own reproducers).  Warm-cache ONLY
      bisection: eh2_exu alone PASSES exactly (2519); eh2_dec alone
      "passes" at 2450 — a 69-cycle stall-timing SKEW, second defect;
      **eh2_lsu alone DIVERGES** — the minimal reproducer.  First
      value-plane diff at CYCLE 400: `lsu_axi_arvalid` NEVER rises
      under accel (interp issues its first data-side bus read there;
      accel: zero assertions in 17,500 cycles → the core stalls on
      its first load forever).  NOT the ck_last/LATE class (CK_LATE=1
      changes neither unit).  CK_TRACE bookkeeping is textbook: the
      chunk has THREE gated extra clocks (active_thread_l2clk__b0/
      __b1 + active_clk); thread clocks rise in the SAME delta as the
      main posedge detect (mask 0xb), active_clk one delta later
      (mask 0x4); pend always 0 (late off).  With everything else
      interpreted, chunk inputs are interp-correct, and VERIFY proves
      the model computes — so the defect is DELTA-LEVEL SAMPLING
      SEMANTICS of the gated-group advance (which `in` snapshot the
      thread-gated flops read at the coincident delta vs interp's
      NBA at the gated clock's own delta).  Next: dump the chunk's
      bus-buffer state-machine inputs (lsu_bus_clk_en, lsu_busreq
      cone) at cycles 395-405 in both arms (NVC_ACCEL_OUT_TRACE_FROM
      ~3.95ms); compare against interp per-delta.  Vehicle logs:
      /home/claude/eh2_rtlil/eh2_bis_eh2_lsu.log, eh2_cktrace.log.
      MERGE-flow experiment queued (the shipped EH1 gated-clock
      answer — if EH2 passes under MERGE, per-chunk lsu drops in
      priority and merge becomes the EH2 recipe).
      **MERGE VERDICT: also diverges** (14 groups, same
      arvalid-never-rises signature) — the gated-group sampling
      defect is FLOW-INDEPENDENT; the delta-sampling fix is the only
      EH2 path.
      **RESIDUE SWEEP 3 (2026-09-02): declines 9 → 7, 4 modules.**
      DYNAMIC PART-SELECT READS land: `sig(x+K downto x)` — bounds
      dynamic, span constant — recognized structurally (left renders
      as right + literal K with identical sub-specs) and lowered to
      shr + [K:0], the l3d_part_read lowering for direct slicing
      (dma_ctrl clears).  SPEC-ELEMENT READS land: element access of
      a whole-substituted local indexes the spec — bare wire names
      directly, sized literals via a width-VERIFIED temp connect
      (an unverifiable spec width DECLINES: an RTLIL connect width
      mismatch throws and poisons the session — caught in probe,
      fixed before landing; poisons stay ZERO) (dec_gpr clears).
      Remaining 4: lsu_stbuf spec-elem (non-literal spec), lsu_bus_
      buffer 256-wide bit tables, ifu_aln var-elem under dynamic
      guards, dec_decode unconstrained temp width.

## 2. ABI containment

- [x] **(2026-08-30)** The facade is libgsm.so's `extern "C"` surface:
      nvc dlopens it `RTLD_LOCAL` and sees one function; nvc's build
      touches no yosys header.  One file to fix per yosys release
      (gen_statemachine.cpp), which was already true.
- [x] Rationale: RTLIL is templates/inlines/`std::map` in headers, so
      linking couples us to an exact Yosys build. The facade keeps NVC's
      own build free of Yosys headers and gives us one file to fix per
      Yosys release instead of scattered breakage.

## 3. ldx interposition (narrow — only these)

- [x] **`exit` / `abort` — mandatory.**  Done WITHOUT ldx: yosys calls
      `log_error_atexit()` immediately before its `_exit(1)`, and the
      facade points that at a thrower; `log_cmd_error_throw=true`
      covers pass errors; gsm's own decline-exits became `throw
      GsmBail`.  All caught at the facade → error return → chunk stays
      interpreted.  Proven live (hierarchy-pass ERROR contained).
      ldx interposition remains the fallback if a yosys path turns up
      that exits without passing through log_error().
- [ ] **ABC.** `abc`/`abc9` fork/exec an external `yosys-abc` and
      round-trip through `/tmp`. Options: `abc -exe` pointed at our own,
      interpose the exec, or skip ABC entirely — for RTL simulation
      codegen we likely don't need technology mapping at all. Check this
      before building anything.
- [ ] **Temp file traffic.** Some passes write to disk unconditionally.
      Interpose `open` → tmpfs/memfd.

## 4. Threading (decide early)

- [x] Yosys keeps process-global state (global design pointer, log
      stream vector, ID string table) and is **not** thread-safe.
- [x] **(2026-08-30) Chose: lock around all Yosys entry.**  A static
      mutex inside `gsm_generate()` — owned by the facade, so every
      client is confined regardless of which nvc thread calls.
      Nothing else in nvc touches yosys.  (Note the flip side under
      §1: the same global state is what forbids parallel in-process
      synth.)

## 5. Semantic gaps (not solvable by interposition)

RTLIL is structurally one driver per bit, four values. These are data
model limits, not hookable policy. Carry alongside the design in RTLIL
attributes and reconstruct on the return path.

- [ ] **Arena resolution.** RTLIL has no resolution functions; two
      drivers on a wire is an error, not a resolved net. `$tribuf`
      covers the Z case only. Design the attribute encoding *before*
      writing the mapping — this is where the differentiation lives and
      discovering the loss later is expensive.
- [ ] **Nine-value logic.** `std_logic`'s U/W/L/H and `-` must collapse
      into RTLIL's `0/1/x/z`. Decide where the information loss goes.

## Rejected

- **Presenting Verific's netlist-DB API shape to Yosys.** Yosys's
  `verific` frontend targets YosysHQ's *private modified* Verific
  source, not the shipped product — no stable interface to match. It
  also consumes the post-RTL-elaboration netlist DB, which NVC does not
  produce (would require register inference, Boolean extraction, an
  operator netlist). And the shim rationale doesn't apply: unlike
  ngspice/KiCad or IBIS-AMI, Yosys is open and accepts our output
  natively. Header-compatibility would additionally require taking the
  Verific eval license — a provenance problem we don't want.
