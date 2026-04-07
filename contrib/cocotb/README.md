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

**Working:**
- New `nvc --cocotb=bridge.so` option loads the bridge after `model_reset`
- C bridge implements ~25 GPI functions: signal read/write, hierarchy
  navigation, callback registration (timed, value-change, phase)
- Python shim (`nvc_simulator.py`) is a drop-in replacement for the
  CocoTB simulator C extension, using ctypes to call the C bridge
- Embedded Python via `dlopen("libpython3.10.so.1.0", RTLD_GLOBAL)`
  to make `_contextvars` and other extension modules importable
- CocoTB regression manager starts and runs `@cocotb.test()` async functions
- Signal reads work (NVC byte-per-bit ↔ CocoTB binary string)
- Hierarchy iteration works (`dut.iterate()`, `dut.signal_name`)
- Clock generation via Python Timer callbacks works
- Per-test pass/fail reporting works

**Limitations:**
- Signal **writes** require either `COCOTB_TRUST_INERTIAL_WRITES=1` (which
  also needs the GpiClock fast-path) OR proper ReadWrite phase callback
  semantics (NVC's `LAST_KNOWN_DELTA_CYCLE` fires every cycle, but our
  trampoline marks the slot inactive after first dispatch — this needs
  rework so CocoTB's queued writes get applied)
- `GpiClock` (the C++ fast-clock) is a stub — currently passes through
  to the Python Timer-based path
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
