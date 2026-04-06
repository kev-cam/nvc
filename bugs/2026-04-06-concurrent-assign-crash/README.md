# SEGV in sched_driver / async_transfer_signal

## Summary

NVC crashes with SIGSEGV in `sched_driver` at model.c:3437 during the first
simulation delta cycle when running VHDL translated from SystemVerilog via
`iverilog -tvhdl`.

## Reproduction

The minimal reproducer in crash_min.vhd does NOT trigger the crash — the
pattern is more complex. To reproduce with the full translated VHDL:

```bash
# 1. Translate UART
iverilog -tvhdl -psv2vhdl=1 -o nexus_uart.vhd -s nexus_uart -g2012 \
  isqed-challenge/duts/common/dv_common_pkg.sv \
  isqed-challenge/duts/nexus_uart/nexus_uart.sv

# 2. Write minimal testbench (see tb_nvc.vhd)
# 3. Analyze, elaborate, run
nvc --std=2040 -L /usr/local/lib/nvc -a nexus_uart.vhd tb_nvc.vhd
nvc --std=2040 -L /usr/local/lib/nvc -e tb_nvc
nvc --std=2040 -L /usr/local/lib/nvc -r tb_nvc   # SEGV here
```

The crash_min.vhd shows the concurrent assignment feedback pattern but
does not reproduce the crash. The actual trigger involves the full
iverilog-translated VHDL with ~90 processes and array-type signals.

## Backtrace

```
#0  sched_driver (...) at model.c:3437
#1  async_transfer_signal (...) at model.c:4038
#2  deferq_run (...) at model.c:438
#3  model_cycle (...) at model.c:4284
#4  model_run (...) at model.c:4375
```

## Analysis

The crash occurs when concurrent signal assignments form a combinational
feedback path through signal transfers:

```
tl_req_valid <= input_a and ready_reg;     -- concurrent assign (transfer)
ready_reg    <= f(pending);                 -- concurrent assign (transfer)
-- pending is set in clocked process that reads tl_req_valid
```

This is legal VHDL — the feedback breaks at the clock edge. But the
transfer scheduling in `async_transfer_signal` appears to create an
invalid driver chain.

## Affected designs

- nexus_uart (from iverilog -tvhdl translation)
- warden_timer
- aegis_aes
- rampart_i2c

Works fine: bastion_gpio, citadel_spi (simpler concurrent assignment patterns)

## Version

nvc 1.19-devel (1.18.0.r203.g8df1d9635)
