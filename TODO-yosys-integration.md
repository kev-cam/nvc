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

- [ ] Build Yosys with `ENABLE_LIBYOSYS=1`.
- [ ] Construct `RTLIL::Design` directly from NVC's elaborated tree.
      No Frontend/Pass plugin required — that machinery only exists to
      add `read_*` commands to the yosys CLI. NVC is the host process.
- [ ] Drive passes via `run_pass()` instead of shelling out and
      round-tripping files.
- [ ] Licensing: Yosys is ISC. Linking carries no obligations.

## 2. ABI containment

- [ ] Thin `extern "C"` facade (~200 lines) in the NVC tree; `dlopen`
      libyosys with `RTLD_LOCAL` behind it.
- [ ] Rationale: RTLIL is templates/inlines/`std::map` in headers, so
      linking couples us to an exact Yosys build. The facade keeps NVC's
      own build free of Yosys headers and gives us one file to fix per
      Yosys release instead of scattered breakage.

## 3. ldx interposition (narrow — only these)

- [ ] **`exit` / `abort` — mandatory.** Yosys's `log_error()` path
      expects to own the process. Unwind back into NVC instead, or a
      malformed design kills the simulator mid-run.
- [ ] **ABC.** `abc`/`abc9` fork/exec an external `yosys-abc` and
      round-trip through `/tmp`. Options: `abc -exe` pointed at our own,
      interpose the exec, or skip ABC entirely — for RTL simulation
      codegen we likely don't need technology mapping at all. Check this
      before building anything.
- [ ] **Temp file traffic.** Some passes write to disk unconditionally.
      Interpose `open` → tmpfs/memfd.

## 4. Threading (decide early)

- [ ] Yosys keeps process-global state (global design pointer, log
      stream vector, ID string table) and is **not** thread-safe.
- [ ] Choose: lock around all Yosys entry, or confine Yosys to one
      thread. Concurrent calls alongside wandering threads will corrupt
      that state. Retrofitting this later is unpleasant.

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
