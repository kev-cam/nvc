# Regression: SEGV in async_transfer_signal (fixed in ef0c086de)

## Bug

NVC crashed with SIGSEGV at `model.c:3437` (`sched_driver`) on the first
delta cycle when running iverilog-translated VHDL. Root cause:
`x_transfer_signal` set up an `rt_transfer_t` for a concurrent assignment
but didn't ensure a `SOURCE_DRIVER` existed on the target nexus.
`async_transfer_signal` -> `sched_driver` -> `find_driver` returned NULL
and the subsequent dereference crashed (assert compiled out by `NDEBUG`).

## Files

- `nexus_uart.vhd` — iverilog-translated nexus_uart UART module
- `tb_nvc.vhd` — minimal NVC testbench

## Reproducing the Environment

The `nexus_uart.vhd` file uses the `sv2vhdl` support library (entity
instantiations like `sv_and`, `sv_or`, `sv_display_pkg`). To rebuild:

```bash
# 1. Translate the SystemVerilog UART to VHDL via iverilog -tvhdl
iverilog -tvhdl -psv2vhdl=1 -g2012 \
    -o nexus_uart.vhd \
    /usr/local/src/isqed-challenge/duts/common/dv_common_pkg.sv \
    /usr/local/src/isqed-challenge/duts/nexus_uart/nexus_uart.sv

# 2. Compile and run with NVC (requires sv2vhdl library)
nvc --std=2040 -L /usr/local/src/nvc/build/lib -a nexus_uart.vhd tb_nvc.vhd
nvc --std=2040 -L /usr/local/src/nvc/build/lib -e tb_nvc
nvc --std=2040 -L /usr/local/src/nvc/build/lib -r tb_nvc
```

Before the fix this would SEGV in `sched_driver` during the first delta
cycle. With the fix it runs to completion at 385ns and reports `done`.

## Why not in standard testlist?

This test requires:
1. iverilog with the `-tvhdl` backend (custom build)
2. The `sv2vhdl` support library (`/usr/local/src/nvc/lib/sv2vhdl/`)
3. The isqed-challenge SystemVerilog source

Rather than vendoring all these dependencies into the standard test suite,
this directory preserves a known-good reproducer that can be re-run by
developers who have the full environment.

## Verification

```
$ nvc --std=2040 -L /usr/local/src/nvc/build/lib -r tb_nvc
...
** Note: 385ns+0: done
   Process :tb_nvc:_p1 at tb_nvc.vhd:21
** Note: 385ns+0: FINISH called
```

## Fix

Commit `ef0c086de` (src/rt/model.c): in `x_transfer_signal`, walk the
target nexus chain and add a `SOURCE_DRIVER` for the active process if
none exists, mirroring `x_drive_signal`.
