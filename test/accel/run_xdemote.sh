#!/bin/bash
# Positive test for the accel X/Z fallback (GAP 1 detection + GAP 2 demote).
#
# Runs xdemo_tb three ways and requires ONE Y value from all three:
#   1. interpreted reference
#   2. accel, detection compiled in, action off   -> must not change anything
#   3. accel + NVC_ACCEL_XDEMOTE=1                -> must demote at the X and
#                                                    finish in the interpreter
# XVAL=X by default; pass "Z" as $1 to drive high-Z instead.
set -u
NVC="${NVC:-/usr/local/src/nvc-build/bin/nvc}"
LIB="${NVC_LIBDIR:-/usr/local/src/nvc-build/lib}"
XVAL="${1:-X}"
A=(-M 256m -H 256m --std=2008 -L "$LIB")
ACC=(NVC_ACCEL_MIN_MODULES=1 NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1
     NVC_ACCEL_CC=cc)
work="/tmp/accel_test_xdemo_$XVAL"
cd "$(dirname "$0")" || exit 1

rm -rf "$work"; mkdir -p "$work"
$NVC "${A[@]}" --work="$work" -a xdemo.vhd xdemo_tb.vhd >/dev/null 2>&1 \
  || { echo "ANALYZE FAILED"; exit 1; }
$NVC "${A[@]}" --work="$work" -e -gXVAL="'$XVAL'" xdemo_tb >/dev/null 2>&1 \
  || { echo "ELABORATE FAILED"; exit 1; }

ref=$($NVC "${A[@]}" --work="$work" -r xdemo_tb 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)

det=$(env "${ACC[@]}" NVC_ACCEL_JIT_DEBUG=1 \
      $NVC "${A[@]}" --work="$work" -r xdemo_tb 2>&1)
det_y=$(echo "$det" | grep -oE 'Y=[0-9]+' | tail -1)

dem=$(env "${ACC[@]}" NVC_ACCEL_XDEMOTE=1 \
      $NVC "${A[@]}" --work="$work" -r xdemo_tb 2>&1)
dem_y=$(echo "$dem" | grep -oE 'Y=[0-9]+' | tail -1)

echo "=== xdemo (XVAL=$XVAL) ==="
echo "  interp        : ${ref:-?}"
echo "  accel+detect  : ${det_y:-?}   $(echo "$det" | grep -c '#AJX') sighting(s)"
echo "$det" | grep -m1 '#AJX'                       | sed 's/^/    /'
echo "$det" | grep -m1 'X/Z at accel boundary'      | sed 's/^/    /'
echo "  accel+XDEMOTE : ${dem_y:-?}"
echo "$dem" | grep -m1 'DEMOTED'                    | sed 's/^/    /'

demoted=$(echo "$dem" | grep -c 'DEMOTED')
if [ -n "$ref" ] && [ "$ref" = "$det_y" ] && [ "$ref" = "$dem_y" ] \
   && [ "$demoted" -ge 1 ]; then
  echo "  -> PASS (all three agree, chunk demoted on the uncertain input)"
  exit 0
fi
echo "  -> FAIL"
exit 1
