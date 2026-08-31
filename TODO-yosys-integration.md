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
      quiet stop after bridge emission, see finding below).  Next: the
      comb-of-clocked NBA idiom (VeeR entry), functions, dynamic
      part-selects.
      **FINDING (pre-existing, both paths): chunks whose bridge has 0
      inputs (clk-only designs like memagg/memrf) never install — the
      flow stops silently between bridge emission and the .so compile.
      Worth its own investigation; check 11 passes on probe-survival
      and check 13 on harness diffs, so neither depends on install.**
      Elaboration traps learned: concurrent assigns arrive as
      one-assign PROCESSES (mirror the text path's lone-assign→assign
      conversion); `&`-chains fold into A_CONCAT AGGREGATES; operator
      FCALL result types are UNCONSTRAINED (derive width from
      operands); folded_int on enum refs yields the enum POSITION
      ('0'=2!) — decode literal idents, never trust the low bit; and
      sigspec buffers must hold one char per BIT for wide literals.
      opt_asserts check 12 gates full-coverage engagement on wide.
      **Original walker plan (for the residual constructs):**
      lives in vhdl2vlog.c sharing its analysis helpers
      (build_reg_set/is_reg, clock_of/areset_of/edges_of, type_width,
      emitted_width, comp_inner, block_types_synth); the emission
      vocabulary to cover is catalogued in the direct-rtlil recon
      (~60 shapes; expressions are the tail — start with the wide/
      arst-fixture subset: refs, literals, binary/unary ops via the
      vlog_op set, ternary→mux, bit-select/slice, concat; DECLINE
      everything else → per-module text fallback).  Statements: the
      clocked-process forms via decision trees with `g0_<sig>` hold
      temps; expression cells use `rx<n>` temp wires (deterministic).
      Decisions taken: TEXT STAYS THE CACHE KEY + staged evidence for
      now (the parent needs the key pre-fork; the content_hash switch
      comes when the text path retires); the builder replaces only the
      read_verilog leg; differential = the 4-engine gate + an
      opt_assert running one suite design under NVC_ACCEL_RTLIL=1
      with an engagement check.  Then: $mem_v2 memories (the
      icache-monster lever).
- [x] Drive passes via `run_pass()` instead of shelling out and
      round-tripping files.  Done for the converted path; the C-out →
      gcc → .so → dlopen leg is inherent (it IS the product) and
      stays.
- [x] Licensing: Yosys is ISC. Linking carries no obligations.

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
