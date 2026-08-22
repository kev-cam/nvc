#!/bin/bash
# Aggregate-translation accel regression: a design built from NAMED / RANGE /
# positional+others / choice-list std_logic_vector aggregates with constant
# elements. The accumulator only counts when every aggregate equals its known
# constant, so a wrong aggregate translation changes Y. Correct accel must
# MATCH the non-accel gold (Y=30). Today the aggregates are expected to decline
# (/*agg*/ -> "not fully translatable") so the subtree does not install.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
NVC="${NVC:-/usr/local/src/nvc-build/bin/nvc}"
LIB="${NVC_LIBDIR:-/usr/local/src/nvc-build/lib}"
WORK="$HERE/agg_work"
rm -rf "$WORK"; mkdir -p "$WORK"
A=(-M 256m -H 256m --std=2008 --work="$WORK" -L "$LIB")

ana() { $NVC "${A[@]}" -a "$@" >/dev/null 2>&1; }
ana "$HERE/agg_top.vhd" || { echo "analyze agg_top FAILED"; exit 2; }
ana "$HERE/agg_tb.vhd"  || { echo "analyze agg_tb FAILED";  exit 2; }
$NVC "${A[@]}" -e agg_tb >/dev/null 2>&1 || { echo "elaborate FAILED"; exit 2; }

echo "=== aggregate-translation accel test ==="
ref=$($NVC "${A[@]}" -r agg_tb 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
out=$(NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 NVC_ACCEL_CC="${NVC_ACCEL_CC:-gcc -O2}" \
      $NVC "${A[@]}" -r agg_tb 2>&1)
acc=$(echo "$out" | grep -oE 'Y=[0-9]+' | tail -1)
inst=$(echo "$out" | grep -ciE 'accel-jit:.*(reroute|active|installed)|rerouted to native|subtree.*active')
emit=$(echo "$out" | grep -c 'emitted subtree')
notxl=$(echo "$out" | grep -c 'not fully translatable')
sf=$(echo "$out" | grep -ci 'synth fail')

printf "  %-10s ref=%-6s accel=%-6s  subtree[emit=%s notxl=%s synthfail=%s install=%s]" \
       "agg_tb" "${ref:-?}" "${acc:-?}" "$emit" "$notxl" "$sf" "$inst"
if [ -n "$ref" ] && [ "$ref" = "$acc" ]; then echo "  -> MATCH"; rc=0; else echo "  -> MISMATCH"; rc=1; fi
exit $rc
