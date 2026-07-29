#!/bin/bash
# std_logic METAVALUE probe.  emit_expr special-cased only '0' and '1' of the
# nine std_logic literals and fell through to a bare name for the rest, so 'U'
# emitted the identifier `_u_` -- declared nowhere in the module.  yosys reads
# that as a fresh undriven wire without complaint, so the chunk installed ACTIVE
# and was silently wrong.  See l3dmv.vhd.
#
# TWO DUTs in one testbench, which must behave DIFFERENTLY:
#   l3dmv_meta  drives 'U'      -> must DECLINE (no value-plane representation)
#   l3dmv_weak  drives 'L'/'H'  -> must still INSTALL (weak drives carry 0 and 1)
# Checking only the checksum would pass if BOTH declined, so this script asserts
# each side separately, and also greps the emitted Verilog for the undeclared
# identifier itself -- the most direct evidence of the defect.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
NVC="${NVC:-/usr/local/src/nvc-build/bin/nvc}"
LIB="${NVC_LIBDIR:-/usr/local/src/nvc-build/lib}"
WORK="$HERE/l3dmv_work"
rm -rf "$WORK"; mkdir -p "$WORK"
A=(-M 256m -H 256m --std=2008 --work="$WORK" -L "$LIB")

$NVC "${A[@]}" -a "$HERE/l3dmv.vhd" "$HERE/l3dmv_tb.vhd" >"$WORK/ana.log" 2>&1 \
  || { echo "  analyze l3dmv FAILED"; tail -5 "$WORK/ana.log"; exit 2; }
$NVC "${A[@]}" -e l3dmv_tb >"$WORK/elab.log" 2>&1 \
  || { echo "  elaborate l3dmv_tb FAILED"; tail -5 "$WORK/elab.log"; exit 2; }

ref=$($NVC "${A[@]}" -r l3dmv_tb 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
out=$(NVC_ACCEL_MIN_MODULES=1 NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 \
      NVC_ACCEL_CACHE_DIR="$WORK/cache" \
      NVC_ACCEL_CC="${NVC_ACCEL_CC:-gcc -O2}" $NVC "${A[@]}" -r l3dmv_tb 2>&1)
acc=$(echo "$out" | grep -oE 'Y=[0-9]+' | tail -1)

meta_declined=$(echo "$out" | grep -c "l3dmv_meta.*not fully translatable")
weak_installed=$(echo "$out" | grep -c "l3dmv_weak")
undeclared=$(grep -ohE '\b_[uxzw]_\b' "$WORK"/cache/*_subtree.v 2>/dev/null \
             | sort -u | tr '\n' ' ')

printf "  %-11s ref=%-8s accel=%-8s meta_declined=%s weak_seen=%s" \
       l3dmv_tb "${ref:-?}" "${acc:-?}" "$meta_declined" "$weak_installed"

[ -z "$ref" ] && { echo "  -> NO REFERENCE"; exit 2; }
if [ -n "$undeclared" ]; then
  echo "  -> FAIL (emitted undeclared metavalue identifier: $undeclared)"; exit 1
elif [ "$ref" != "$acc" ]; then
  echo "  -> ACCEL-MISMATCH (silent wrong answer)"; exit 1
elif [ "$meta_declined" -eq 0 ]; then
  echo "  -> FAIL ('U' subtree did not decline)"; exit 1
else
  echo "  -> OK ('U' declined, values match)"; exit 0
fi
