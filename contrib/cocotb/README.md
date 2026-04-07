# NVC ↔ CocoTB Direct Bridge

A direct bridge that allows running [CocoTB](https://www.cocotb.org/) Python
tests against the NVC VHDL simulator without using VPI/VHPI. The Python
`cocotb.simulator` C extension is replaced with a pure-Python shim that
calls into a small C library (`nvc_cocotb_bridge.so`) which talks directly
to NVC's `rt/model.h` API.

Goals:
- **No VPI/VHPI overhead** — direct C calls to NVC's model layer
- **Source-level Python debug in gdb** (planned, requires Nuitka stage)
- **Compatible with existing CocoTB tests** — no changes to user test code

## Status

**Working (Phase 2 — sync mode):**
- `translate_cocotb.py` AST transformer converts async/await CocoTB tests
  to plain sequential Python with blocking C bridge calls
- New blocking bridge functions: `nvcb_wait_time`, `nvcb_wait_edge`,
  `nvcb_start_clock` — these synchronously advance the simulation
- Free-running clocks driven by NVC timeouts (no Python in the loop)
- `sched_deposit` (proper external API) for signal writes
- `COCOTB_TRUST_INERTIAL_WRITES=1` set automatically so writes apply
  immediately without ReadWrite phase
- **2/3 bastion_gpio tests pass** (test_reset_values, test_input_read)
- nexus_uart and other DUTs translate but fail at TL-UL bus reads

**Still working from Phase 1:**
- `--cocotb=bridge.so` NVC option (no VPI/VHPI)
- C bridge with ~25 GPI functions
- Python shim drop-in for cocotb.simulator
- Hierarchy navigation, signal read

**Known issues:**
- Some DUTs (nexus_uart) don't propagate TL-UL bus responses — likely
  needs more delta cycles between write and read, or the DUT processes
  aren't running at the right phase
- `test_output_direction` reads back wrong value — DUT register write
  doesn't show in output (timing/propagation issue)
- No Nuitka compilation yet — Python is interpreted via embedded CPython
- No gdb source-level Python debug yet (needs Nuitka stage)

## Architecture

```
test_basic.py
   |
   +-- import cocotb            (real CocoTB Python package)
   +-- import cocotb.simulator  (replaced with nvc_simulator.py shim)
                |
                v
        ctypes calls into
                |
                v
   nvc_cocotb_bridge.so (libnvc.so + libpython3.10.so)
                |
                v
        NVC rt_model_t API
        (root_scope, signal_value, deposit_signal, watch_new, ...)
```

`nvc --cocotb=bridge.so DUT` does:

1. `model_reset(model)`
2. `dlopen(bridge.so)` → `nvc_cocotb_entry(model)`
3. Bridge: `Py_Initialize()`, set up handle table from model hierarchy
4. Bridge: import `cocotb`, call `init_package_from_simulation`,
   `run_regression`
5. CocoTB scheduler registers callbacks via the simulator shim → bridge
6. Return from `nvc_cocotb_entry`
7. NVC `model_run()` advances simulation; C trampolines fire and
   dispatch back into Python via the registered dispatcher function

## Files

- `nvc_cocotb_bridge.h` — C bridge API header
- `nvc_cocotb_bridge.c` — Bridge implementation (handle table, signal
  conversion, callback trampolines, Python entry point)
- `nvc_simulator.py` — Drop-in `cocotb.simulator` replacement
- `preprocess_cocotb.py` — Copies CocoTB package, swaps simulator module
- `build_cocotb_nvc.sh` — Build orchestrator (DUT translation, NVC
  elaboration, bridge .so build, run)

## Building

```bash
# Build bridge .so against NVC libs + libpython
cd /usr/local/src/nvc/contrib/cocotb
gcc -shared -fPIC -g -O1 \
    -I/usr/local/src/nvc/src \
    -I/usr/local/src/nvc-build/src \
    -I/usr/local/src/nvc/thirdparty \
    $(python3-config --includes) \
    nvc_cocotb_bridge.c \
    -L/usr/local/src/nvc-build/lib \
    -lnvc -lthirdparty -ldl \
    $(python3-config --ldflags --embed) \
    -Wl,-rpath,/usr/local/src/nvc-build/lib \
    -o /tmp/nvc_cocotb_bridge.so
```

## Running

```bash
# Use preprocessor to set up test directory with shimmed cocotb
python3 preprocess_cocotb.py path/to/test_basic.py extras.py -o build/

# Compile DUT VHDL with NVC (already done separately)
nvc --std=2008 -L lib -a dut.vhd
nvc --std=2008 -L lib -e my_dut

# Run with the bridge
COCOTB_TEST_MODULES=test_basic \
COCOTB_TOPLEVEL=my_dut \
PYTHONPATH=build/ \
nvc --std=2008 -L lib -r my_dut --cocotb=/tmp/nvc_cocotb_bridge.so
```

## Next Steps

1. **Fix ReadWrite phase callback semantics** so signal writes propagate
   without requiring `COCOTB_TRUST_INERTIAL_WRITES=1`. NVC's
   `LAST_KNOWN_DELTA_CYCLE` fires every delta cycle; we need to track
   per-slot whether a Python callback is currently registered and only
   dispatch when it is.

2. **Implement GpiClock fast-path** using NVC's native clock support
   (or just `model_set_timeout_cb` with periodic re-registration).

3. **Replace CocoTB's Python scheduler with NVC's native scheduler**:
   the CocoTB tasks become NVC processes; CocoTB triggers (Timer,
   RisingEdge) become NVC `wait` statements. This eliminates Python
   event loop overhead entirely. Requires Nuitka pre-compilation and
   custom scheduling glue.

4. **Nuitka compilation pipeline** for source-level Python debug in gdb.

5. **Test all 21 isqed-challenge tests** end-to-end.
