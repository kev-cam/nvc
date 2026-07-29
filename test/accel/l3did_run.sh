#!/bin/bash
# 3D-Logic accel probe: a ONE-PARAMETER l3dk==2 identity (unsigned_to_l3d_bit)
# used as a CONCATENATION ELEMENT -- the width guard cannot fire because nw is
# read only from parameter 1.  See l3did.vhd.
# ELEMENT -- the sv2vhdl register-file write-mask idiom.  See l3did.vhd.
#
# Separate from run.sh because logic3d lives in the SV2VHDL library and its
# package is analysed at revision 2040, while run.sh is a --std=2008 suite.
#
# Expected: ref == accel, and the subtree INSTALLS (install>0).  With the
# self-determined-width defect present the chunk still installs but reports
# Y=4369 instead of Y=15619583 (every write masked with 8'h11).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
NVC="${NVC:-/usr/local/src/nvc-build/bin/nvc}"
LIB="${NVC_LIBDIR:-/usr/local/src/nvc-build/lib}"
WORK="$HERE/l3did_work"
rm -rf "$WORK"; mkdir -p "$WORK"
A=(-M 256m -H 256m --std=2040 --work="$WORK" -L "$LIB")

$NVC "${A[@]}" -a "$HERE/l3did.vhd" "$HERE/l3did_tb.vhd" >/dev/null 2>&1 \
  || { echo "analyze l3did FAILED"; exit 2; }
$NVC "${A[@]}" -e l3did_tb >/dev/null 2>&1 \
  || { echo "elaborate l3did_tb FAILED"; exit 2; }

ref=$($NVC "${A[@]}" -r l3did_tb 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
out=$(NVC_ACCEL_MIN_MODULES=1 NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 \
      NVC_ACCEL_CC="${NVC_ACCEL_CC:-gcc -O2}" $NVC "${A[@]}" -r l3did_tb 2>&1)
acc=$(echo "$out" | grep -oE 'Y=[0-9]+' | tail -1)
instm=$(echo "$out" | grep -ciE 'accel-jit:.*(reroute|active|installed)|subtree.*active')
notxl=$(echo "$out" | grep -c 'not fully translatable')

printf "  %-12s ref=%-10s accel=%-10s subtree[notxl=%s install=%s]" \
       l3did_tb "${ref:-?}" "${acc:-?}" "$notxl" "$instm"

# A matching checksum with install=0 proves NOTHING about the accel path -- the
# run fell back to the interpreter, so of course it agrees.  Label the two
# outcomes differently or this test reads as "accelerated and correct" when it
# is really "declined". Today the expected result is DECLINED-SAFE: the width
# guard cannot show the one-parameter identity is 1 bit wide, so it refuses the
# chunk. ACCEL-MATCH would mean someone taught it to emit the scalar correctly
# and is strictly better. Only ACCEL-MISMATCH is a failure.
if [ -z "$ref" ]; then echo "  -> NO REFERENCE"; exit 2; fi
if [ "$ref" != "$acc" ]; then echo "  -> ACCEL-MISMATCH (silent wrong answer)"; exit 1; fi
if [ "${instm:-0}" -eq 0 ]; then echo "  -> DECLINED-SAFE (guard held; not accelerated)"; exit 0; fi
echo "  -> ACCEL-MATCH (installed and correct)"; exit 0
