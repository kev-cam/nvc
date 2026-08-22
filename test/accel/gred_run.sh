#!/bin/bash
# Adversarial generic-dedup regression: a generic AND-reducer instantiated at two
# widths whose results diverge under a wrong-width (deduped) instantiation.
# Correct accel must MATCH the non-accel gold (Y=30). Setting NVC_ACCEL_NO_VARIANT=1
# restores the old dedup-by-entity behaviour and should MISMATCH (proving the fix).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
NVC="${NVC:-/usr/local/src/nvc-build/bin/nvc}"
LIB="${NVC_LIBDIR:-/usr/local/src/nvc-build/lib}"
WORK="$HERE/gred_work"
rm -rf "$WORK"; mkdir -p "$WORK"
A=(-M 256m -H 256m --std=2008 --work="$WORK" -L "$LIB")

ana() { $NVC "${A[@]}" -a "$@" >/dev/null 2>&1; }
ana "$HERE/greduce.vhd"  || { echo "analyze greduce FAILED";  exit 2; }
ana "$HERE/gred_top.vhd" || { echo "analyze gred_top FAILED"; exit 2; }
ana "$HERE/gred_tb.vhd"  || { echo "analyze gred_tb FAILED";  exit 2; }
$NVC "${A[@]}" -e gred_tb >/dev/null 2>&1 || { echo "elaborate FAILED"; exit 2; }

echo "=== adversarial generic-dedup accel test (NVC_ACCEL_NO_VARIANT=${NVC_ACCEL_NO_VARIANT:-0}) ==="
ref=$($NVC "${A[@]}" -r gred_tb 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
out=$(NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 NVC_ACCEL_CC="${NVC_ACCEL_CC:-gcc -O2}" \
      $NVC "${A[@]}" -r gred_tb 2>&1)
acc=$(echo "$out" | grep -oE 'Y=[0-9]+' | tail -1)
inst=$(echo "$out" | grep -ciE 'rerouted to native|subtree.*active')
emit=$(echo "$out" | grep -c 'emitted subtree')
notxl=$(echo "$out" | grep -c 'not fully translatable')
sf=$(echo "$out" | grep -ci 'synth fail')

printf "  %-10s ref=%-6s accel=%-6s  subtree[emit=%s notxl=%s synthfail=%s install=%s]" \
       "gred_tb" "${ref:-?}" "${acc:-?}" "$emit" "$notxl" "$sf" "$inst"
if [ -n "$ref" ] && [ "$ref" = "$acc" ]; then echo "  -> MATCH"; rc=0; else echo "  -> MISMATCH"; rc=1; fi
exit $rc
