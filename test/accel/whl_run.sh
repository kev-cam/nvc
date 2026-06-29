#!/bin/bash
# Counter-controlled while-loop accel regression: a clocked process whose body is
# the `i:=init; while i<N loop ..; i:=i+1` shape sv2ghdl lowers SV `for` loops to.
# vhdl2vlog currently DECLINES this (T_WHILE -> /*?while-loop*/ -> "not fully
# translatable") so the subtree does NOT install and the design stays interpreted.
# Gold is Y=180 (popcount(x"B7")=6 added on each of 30 post-reset edges). Once a
# while->Verilog-`for` peephole lands the subtree should install (install>=1,
# notxl=0) and still MATCH this gold.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
NVC="${NVC:-/usr/local/src/nvc-build/bin/nvc}"
LIB="${NVC_LIBDIR:-/usr/local/src/nvc-build/lib}"
WORK="$HERE/whl_work"
rm -rf "$WORK"; mkdir -p "$WORK"
A=(-M 256m -H 256m --std=2008 --work="$WORK" -L "$LIB")

ana() { $NVC "${A[@]}" -a "$@" >/dev/null 2>&1; }
ana "$HERE/whl_top.vhd" || { echo "analyze whl_top FAILED"; exit 2; }
ana "$HERE/whl_tb.vhd"  || { echo "analyze whl_tb FAILED";  exit 2; }
$NVC "${A[@]}" -e whl_tb >/dev/null 2>&1 || { echo "elaborate FAILED"; exit 2; }

echo "=== counter-controlled while-loop accel test ==="
ref=$($NVC "${A[@]}" -r whl_tb 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
out=$(NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 NVC_ACCEL_CC=cc \
      $NVC "${A[@]}" -r whl_tb 2>&1)
acc=$(echo "$out" | grep -oE 'Y=[0-9]+' | tail -1)
inst=$(echo "$out" | grep -ciE 'accel-jit:.*(reroute|active|installed)|rerouted to native|subtree.*active')
emit=$(echo "$out" | grep -c 'emitted subtree')
notxl=$(echo "$out" | grep -c 'not fully translatable')
sf=$(echo "$out" | grep -ci 'synth fail')

printf "  %-10s ref=%-6s accel=%-6s  subtree[emit=%s notxl=%s synthfail=%s install=%s]" \
       "whl_tb" "${ref:-?}" "${acc:-?}" "$emit" "$notxl" "$sf" "$inst"
if [ -n "$ref" ] && [ "$ref" = "$acc" ]; then echo "  -> MATCH"; rc=0; else echo "  -> MISMATCH"; rc=1; fi
exit $rc
