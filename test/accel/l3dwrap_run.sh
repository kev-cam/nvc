#!/bin/bash
# 3D-Logic (logic3d) accel probe: the sv2vhdl WRAPPER CHAIN
# `to_l3d(unsigned_to_l3d(l3d_to_unsigned(X)), N)` used as a CONCATENATION
# ELEMENT.  See l3dwrap.vhd.
#
# Separate from run.sh because logic3d lives in the SV2VHDL library and its
# package is analysed at revision 2040, while run.sh is a --std=2008 suite.
#
# Every link of that chain is an l3dk==2 IDENTITY: it prints its operand
# verbatim and DROPS the width argument.  A concat-element guard that stops at
# the outermost node therefore never inspects what is underneath -- and that
# chain is the dominant construct sv2vhdl emits.
#
# Expected: ref == accel, with EXACTLY ONE of the two modules installed.
#   l3dwrap  -- all widths genuinely 8, but only provable by DESCENDING three
#               wrappers of unconstrained numeric_std type: must INSTALL.
#   l3dwrapx -- `to_l3d(<integer expr>, 8)` emits 32 bits into an 8-bit slot,
#               and it is the LOW concat element so the displacement is not
#               truncated away: must DECLINE.
# Without the propagation both install and Y is 393427 instead of 3135187.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
NVC="${NVC:-/usr/local/src/nvc-build/bin/nvc}"
LIB="${NVC_LIBDIR:-/usr/local/src/nvc-build/lib}"
WORK="$HERE/l3dwrap_work"
rm -rf "$WORK"; mkdir -p "$WORK"
A=(-M 256m -H 256m --std=2040 --work="$WORK" -L "$LIB")

$NVC "${A[@]}" -a "$HERE/l3dwrap.vhd" "$HERE/l3dwrap_tb.vhd" >/dev/null 2>&1 \
  || { echo "analyze l3dwrap FAILED"; exit 2; }
$NVC "${A[@]}" -e l3dwrap_tb >/dev/null 2>&1 \
  || { echo "elaborate l3dwrap_tb FAILED"; exit 2; }

ref=$($NVC "${A[@]}" -r l3dwrap_tb 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
out=$(NVC_ACCEL_MIN_MODULES=1 NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 \
      NVC_ACCEL_CC="${NVC_ACCEL_CC:-gcc -O2}" $NVC "${A[@]}" -r l3dwrap_tb 2>&1)
acc=$(echo "$out" | grep -oE 'Y=[0-9]+' | tail -1)
instm=$(echo "$out" | grep -ciE 'accel-jit:.*(reroute|active|installed)|subtree.*active')
notxl=$(echo "$out" | grep -c 'not fully translatable')

printf "  %-12s ref=%-10s accel=%-10s subtree[notxl=%s install=%s]" \
       l3dwrap_tb "${ref:-?}" "${acc:-?}" "$notxl" "$instm"
if [ -n "$ref" ] && [ "$ref" = "$acc" ]; then echo "  -> MATCH"; exit 0
else echo "  -> MISMATCH"; exit 1; fi
