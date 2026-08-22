# TODO: tighten NVC ↔ Yosys coupling

**Decision:** use Yosys as-is. Link `libyosys.so`, interpose with ldx where
needed. No Cameron EDA fork of the Yosys repo.

## 0. Identify the seam first (blocker for everything below)

Determine where the current looseness actually is:

- [ ] Confirm whether the existing RTL codegen path is `write_cxxrtl`.
      If so, the generated C++ depends only on the header-only CXXRTL
      runtime (`cxxrtl.h`), **not** on libyosys — meaning linking
      libyosys does not tighten anything, and the real seam is the
      CXXRTL object boundary (object model, signal naming, who owns the
      eval loop).
- [ ] If the seam is CXXRTL: scope getting NVC's scheduler to drive
      eval, rather than CXXRTL's.
- [ ] If the seam is the Yosys API: proceed with §1.

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
