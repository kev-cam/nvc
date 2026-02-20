# STD_MX VHDL Extensions for SystemVerilog Translation

## Overview

The `--std=2040` (STD_MX) mode extends VHDL to support constructs needed for
translating SystemVerilog netlists into simulatable VHDL. These extensions are
implemented in the NVC simulator fork and are not part of any IEEE VHDL standard.

The core problem: SystemVerilog has a rich wire/net model with strength-aware
resolution, bidirectional `tran` gates, and immediate (`blocking`) assignments.
Standard VHDL has none of these. STD_MX bridges the gap by adding implicit
signals, deposit assignments, and a resolution network architecture.

## Implicit Signal Attributes

Semantically, signal/port declarations create composite objects: a net connection
and drivers and receivers. A bare signal name is actually an alias for a driver
when used in the LHS of an assignment, and a receiver alias when on the RHS. The
extennsions here just make those explicitly accessible, and add extra capability
to support bidirectional modeling.

While Verilog and VHDL stanndards don't mention receivers and multi-target
resolution, it should be noted that doing something like back-annotating with SDF
breaks up a net and creates multiple receivers, so support is there already for
these constructs.

The second extension is to allow external generation of resolver code, recognizing
that the driver/receiver constructs can be treated as signals which are input/output
to an entity/architecture that is just described with regular VHDL - similar to
the connect-module construct in Verilog-AMS. Those architectures can be used as a
bridge to external simulators for federated/mixed-signal simulation.

For mixed-signal simulation the main constraint on port connnections is nature and
discipline compatibility (as well as dimensionality), types belong to the drivers
and receivers and not to the wire, so the extended VHDL largely ignores port types
in the resolution process. Resolution for electrical connections requires resolving
Voltage in one direction and current in another. 

Operations in the resolvers need to be zero-time, so we are also adding Verilog's
blocking assignment for signals, which is also needed for faithful translation of
SystemVerilog.

### 'DRIVER

Each signal in STD_MX mode has an implicit `'DRIVER` signal that represents
what this particular driver is asserting onto the net.

- **Type**: Determined by the assignment context (typically `logic3ds` for
  strength-aware gates), NOT by the port declaration type.
- **Write**: `y'driver <= value after delay;` or `y'driver := value;`
- **Read**: Via VHDL-2008 external names in the resolver:
  `<< signal .wrapper.dut.instance.port.driver : logic3ds >>`
- **External name path**: `.signal.driver` maps to internal name `signal$driver`

### 'RECEIVER

Each signal has an implicit `'RECEIVER` signal that represents the resolved
value that the outside world is providing to this signal.

- **Type**: Determined by the assignment context (typically `std_logic` for
  resolved output).
- **Write**: Only by the resolution network via deposit: `rcv := value;`
- **Read**: Implicitly -- in STD_MX mode, `x` on the RHS of an expression
  is equivalent to `x'receiver`.
- **External name path**: `.signal.receiver` maps to internal name `signal$receiver`

### 'OTHER

Each signal has an implicit `'OTHER` signal used in bidirectional (tran) gate
resolution, representing what all OTHER drivers on the net are asserting
(excluding this endpoint's own driver).

- **Type**: Same as 'DRIVER (determined by assignment context).
- **Write**: Only by the resolution network.
- **Read**: By the gate/primitive to know what others are driving.
- **External name path**: `.signal.other` maps to internal name `signal$other`

### RHS Aliasing Rules

In STD_MX mode, signal references on the right-hand side of expressions are
implicitly aliased:

| Expression | Equivalent to | Meaning |
|-----------|---------------|---------|
| `x` (RHS) | `x'receiver` | What the resolved net value is |
| `x'driver` (RHS) | `x` (the raw signal) | What this driver is asserting |

This means a gate that writes `y'driver := L3DS_1` and later reads `y` will
see the resolved value from all drivers, not its own driven value.

## Deposit Assignment (`:=` for signals)

STD_MX adds the `:=` operator for signal targets (T_DEPOSIT), providing
immediate value update without delta-cycle scheduling.

```vhdl
signal_name := value;  -- Immediate deposit, no delta delay
```

### Semantics

- Writes directly to the signal's effective value buffer (nexus_effective)
- Does NOT create a SOURCE_DRIVER on the signal's nexus
- Triggers wakeup of processes sensitive to the signal
- Propagates through output chains (SOURCE_PORT, SOURCE_IMPLICIT) via
  deferred driving update (one delta cycle propagation)
- Contrast with `<=` which schedules a waveform for the next delta cycle

### Use case

The resolution network deposits computed values into 'RECEIVER signals:
```vhdl
alias rcv is << signal .wrapper.dut.y.receiver : std_logic >>;
-- ...
rcv := resolved_value;  -- Immediate update
```

## Strength-Aware Types

### logic3d (3-bit encoded)

Compact 3-bit encoding for logic values with strength information:
- Bit 0: value (0 or 1)
- Bit 1: strength (0=weak, 1=strong)
- Bit 2: uncertain (0=known, 1=unknown/X)

Defined in `logic3d_types_pkg`.

### logic3ds (4-byte record)

Full strength-aware type for detailed resolution:

```vhdl
type logic3ds is record
    value    : integer range 0 to 255;   -- 0=low, 255=high
    strength : l3ds_strength;            -- ST_HIGHZ..ST_SUPPLY (0..10)
    flags    : l3ds_flags;               -- FL_KNOWN, FL_UNKNOWN, etc.
    reserved : integer range 0 to 255;
end record;
```

Key constants:
| Constant | value | strength | flags |
|----------|-------|----------|-------|
| L3DS_0 | 0 | ST_STRONG (8) | FL_KNOWN (0) |
| L3DS_1 | 255 | ST_STRONG (8) | FL_KNOWN (0) |
| L3DS_X | 0 | ST_STRONG (8) | FL_UNKNOWN (1) |
| L3DS_Z | 0 | ST_HIGHZ (0) | FL_KNOWN (0) |

Defined in `logic3ds_pkg` with resolution function `l3ds_resolve`.

## Resolution Network Architecture

The resolution network resolves multi-driver nets using a plugin-based
architecture that operates alongside the main simulation.

### Components

1. **VHPI Plugin** (`resolver.c`): Walks the design hierarchy at elaboration
   time, discovers nets with multiple endpoints, groups them by actual signal.

2. **Python Generator** (`sv2vhdl_resolver.py`): Generates per-net VHDL
   resolver entities and a wrapper entity.

3. **Wrapper Entity**: Top-level entity that instantiates the DUT and all
   resolver entities side by side.

### Signal Flow

```
Gate Process           Resolution Network         Parent Signal
     |                       |                        |
     |-- y'driver := val --> |                        |
     |                  [read via external name]      |
     |                       |                        |
     |                  resolve(all drivers)          |
     |                       |                        |
     |                  rcv := result  ------------> y (= y'receiver)
     |                       |                        |
```

### Per-Net Resolution

For N endpoints on the same net:

- **N=2** (common tran case): Simple swap
  ```vhdl
  rcv_0 := drv_1;  -- endpoint 0 sees what endpoint 1 drives
  rcv_1 := drv_0;  -- endpoint 1 sees what endpoint 0 drives
  ```

- **N>2**: Each receiver gets the resolution of all other drivers
  ```vhdl
  rcv_i := resolve(drv_0, drv_1, ..., drv_{i-1}, drv_{i+1}, ..., drv_{N-1});
  ```

### External Name Convention

The resolver accesses implicit signals via VHDL-2008 external names:
```vhdl
alias drv_0 is << signal .wrapper.dut.inst.port.driver : logic3ds >>;
alias rcv   is << signal .wrapper.dut.signal.receiver  : std_logic >>;
```

Path mapping: `.signal.driver` -> `signal$driver`, `.signal.other` -> `signal$other`

## STD_MX Mode Behavioral Changes

When `--std=2040` is active:

1. **Suppressed warnings**: Non-fatal standard compliance warnings are suppressed.

2. **Inout port source dropping**: Undriven inout ports don't contribute 'U'
   to resolution. SOURCE_PORT is skipped in `calculate_driving_value` when the
   input nexus has no SOURCE_DRIVER.

3. **Auto-receiver creation**: Every `T_SIGNAL_DECL` in architecture blocks
   automatically gets a 'RECEIVER implicit signal (`elab_auto_receivers`).

4. **Implicit signal creation**: 'DRIVER and 'OTHER are created as standalone
   signals during lowering (IMPLICIT_DRIVER/IMPLICIT_OTHER).

5. **Reverse implicit mapping**: 'RECEIVER uses `emit_map_implicit` with
   reversed direction (receiver=src, parent=dst), creating a SOURCE_IMPLICIT
   on the parent's nexus that reads from the receiver's nexus.

## Implementation Notes

### NVC Source Files

| File | Role |
|------|------|
| `src/parse.c` | Parser extensions for 'DRIVER/'OTHER/'RECEIVER attributes |
| `src/sem.c` | Semantic analysis; T_DEPOSIT for `:=` on signal targets |
| `src/simp.c` | Simplifier replaces attr_ref with T_REF to implicit signal |
| `src/lower.c` | Lowers implicit signals; reverse emit_map_implicit for 'RECEIVER |
| `src/elab.c` | `elab_auto_receivers`: auto-creates 'RECEIVER for all signals |
| `src/rt/model.c` | Runtime: source_value, deposit_signal, calculate_driving_value |
| `src/rt/ename.c` | External name resolution: `.driver`->`$driver`, `.other`->`$other` |

### Signal Data Buffer Layout

Every signal gets a 3*size data buffer:
- Offset 0 to size-1: **effective value** (what RHS reads see)
- Offset size to 2*size-1: **last value** (previous effective value)
- Offset 2*size to 3*size-1: **driving value** (used when NET_F_EFFECTIVE set)

### Deposit Propagation Path

1. `deposit_signal` writes directly to `nexus_effective`
2. Iterates output chain; for SOURCE_IMPLICIT outputs, calls
   `defer_driving_update` on the parent nexus
3. Parent's `calculate_driving_value` runs in next delta cycle
4. SOURCE_IMPLICIT source calls `source_value` which reads
   `nexus_effective` of the receiver (the deposited value)
5. Parent's effective value updates, visible to processes

### source_value Guard for SOURCE_IMPLICIT

The `source_value` function uses `last_event < TIME_HIGH` to decide whether
to include a receiver's value in the parent's resolution:

- **Receiver deposited to** (by resolver): `last_event` is set to simulation
  time by `deposit_signal` -- returns `nexus_effective(receiver)` and
  contributes the resolved value to the parent signal.
- **Receiver never touched** (e.g., signals without a resolver): `last_event`
  stays at `TIME_HIGH` (initial value) -- returns NULL, does not contribute,
  avoiding stale 'U' poisoning resolution.

This matters because `elab_auto_receivers` creates receivers for ALL
architecture signals, including those that don't participate in external
resolution.

### Known Limitations

- Deposit propagation takes one delta cycle (deferred driving update)
- `<=` (signal assignment) to 'RECEIVER signals is not yet disallowed but
  should be -- only `:=` (deposit) gives correct immediate-update semantics
