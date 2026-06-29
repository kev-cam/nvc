#!/bin/bash
# Whole-subtree --accel regression for a generic module instantiated at
# multiple widths (gadder @ W=8 and W=4 inside gen_top).
# Runs gen_tb WITHOUT accel (gold) and WITH accel; the reported Y must match
# (accel must not change results), and we report whether a subtree actually
# installed (the point of the feature).
#
# Usage: ./gen_run.sh            # the generic-width case
#        NVC=... ./gen_run.sh    # override nvc binary
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
NVC="${NVC:-/usr/local/src/nvc-build/bin/nvc}"
LIB="${NVC_LIBDIR:-/usr/local/src/nvc-build/lib}"
STD=2008
WORK="$HERE/gen_work"
rm -rf "$WORK"; mkdir -p "$WORK"
A=(-M 256m -H 256m --std=$STD --work="$WORK" -L "$LIB")

ana() { $NVC "${A[@]}" -a "$@" >/dev/null 2>&1; }
ana "$HERE/gadder.vhd"  || { echo "analyze gadder FAILED"; exit 2; }
ana "$HERE/gen_top.vhd" || { echo "analyze gen_top FAILED"; exit 2; }
ana "$HERE/gen_tb.vhd"  || { echo "analyze gen_tb FAILED"; exit 2; }

fails=0
run_case() {
  local tb="$1"
  $NVC "${A[@]}" -e "$tb" >/dev/null 2>&1 || { echo "  $tb: ELABORATE FAILED"; fails=$((fails+1)); return; }
  local ref acc instm
  ref=$($NVC "${A[@]}" -r "$tb" 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
  local out
  out=$(NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 NVC_ACCEL_CC=cc \
        $NVC "${A[@]}" -r "$tb" 2>&1)
  acc=$(echo "$out" | grep -oE 'Y=[0-9]+' | tail -1)
  instm=$(echo "$out" | grep -ciE 'accel-jit:.*(reroute|active|installed)|subtree.*active')
  local emitted notxl synthfail
  emitted=$(echo "$out" | grep -c 'emitted subtree')
  notxl=$(echo "$out" | grep -c 'not fully translatable')
  synthfail=$(echo "$out" | grep -c 'synth failed')
  printf "  %-12s ref=%-6s accel=%-6s  subtree[emit=%s notxl=%s synthfail=%s install=%s]" \
         "$tb" "${ref:-?}" "${acc:-?}" "$emitted" "$notxl" "$synthfail" "$instm"
  if [ -n "$ref" ] && [ "$ref" = "$acc" ]; then echo "  -> MATCH"; else echo "  -> MISMATCH"; fails=$((fails+1)); fi
}

echo "=== generic-width subtree accel test ==="
run_case gen_tb
echo "=== $fails failure(s) ==="
exit $fails
