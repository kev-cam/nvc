#!/bin/bash
# 3D-Logic (logic3d) accel probe: a scalar l3d_bit_read used as a CONCATENATION
# ELEMENT -- the sv2vhdl register-file write-mask idiom.  See l3dcat.vhd.
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
WORK="$HERE/l3dcat_work"
rm -rf "$WORK"; mkdir -p "$WORK"
A=(-M 256m -H 256m --std=2040 --work="$WORK" -L "$LIB")

$NVC "${A[@]}" -a "$HERE/l3dcat.vhd" "$HERE/l3dcat_tb.vhd" >/dev/null 2>&1 \
  || { echo "analyze l3dcat FAILED"; exit 2; }
$NVC "${A[@]}" -e l3dcat_tb >/dev/null 2>&1 \
  || { echo "elaborate l3dcat_tb FAILED"; exit 2; }

ref=$($NVC "${A[@]}" -r l3dcat_tb 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
out=$(NVC_ACCEL_MIN_MODULES=1 NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 \
      NVC_ACCEL_CC="${NVC_ACCEL_CC:-gcc -O2}" $NVC "${A[@]}" -r l3dcat_tb 2>&1)
acc=$(echo "$out" | grep -oE 'Y=[0-9]+' | tail -1)
instm=$(echo "$out" | grep -ciE 'accel-jit:.*(reroute|active|installed)|subtree.*active')
notxl=$(echo "$out" | grep -c 'not fully translatable')

printf "  %-12s ref=%-10s accel=%-10s subtree[notxl=%s install=%s]" \
       l3dcat_tb "${ref:-?}" "${acc:-?}" "$notxl" "$instm"
if [ -n "$ref" ] && [ "$ref" = "$acc" ]; then echo "  -> MATCH"; exit 0
else echo "  -> MISMATCH"; exit 1; fi
