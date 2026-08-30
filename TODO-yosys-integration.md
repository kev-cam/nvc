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
      **Caveat found while testing (2026-08-30): this legacy non-JIT
      path is rotted independently of the conversion** — wide_n8w256
      under `NVC_ACCEL=1 NVC_ACCEL_FROM_VHDL=1` (no `NVC_ACCEL_JIT`)
      installs its chunk and then the sim produces no Y= in BOTH the
      in-process and `NVC_ACCEL_NO_GSMLIB=1` CLI variants (identical
      logs, rc=0).  The shipping config is the aj path; consider
      deleting the legacy path when aj converts.
- [ ] **The accel-jit paths (model.c ~9040, ~10040) stay CLI, for two
      load-bearing reasons — solve these before converting them:**
      1. *The exec boundary is the watchdog.*  `/usr/bin/timeout -k 5`
         is the shipped fix for the proc_dlatch 37-hour hang; a hung
         yosys pass in-process cannot be killed (and would hold the
         facade mutex forever).  Needs a cooperative interrupt in the
         pass loop (or a fork()-without-exec worker) first.
      2. *Parallel group synth vs one-global-yosys.*  Merged-group
         commands run as parallel processes today; yosys global state
         forces in-process calls to serialize on the facade mutex.
         Parallelism REQUIRES processes until yosys instances are
         isolatable — this constrains the direct-RTLIL endgame too
         (likely shape: dedicated synth worker process(es) fed
         RTLIL-construction commands, not N in-process threads).
      Also: aj cache freshness stats `aj_gen_sm()` (the CLI binary)
      at ~8936/~9972 — an in-process conversion must re-key on
      libgsm.so's mtime.
- [ ] Construct `RTLIL::Design` directly from NVC's elaborated tree.
      No Frontend/Pass plugin required — that machinery only exists to
      add `read_*` commands to the yosys CLI. NVC is the host process.
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
