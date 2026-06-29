#!/bin/bash
# Whole-subtree --accel regression on small hierarchical designs.
# For each top: run WITHOUT accel (gold) and WITH accel; the reported Y must
# match (accel must not change results), and we report whether a subtree
# actually installed (the point of the feature).
#
# Usage: ./run.sh            # all cases
#        NVC=... ./run.sh    # override nvc binary
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
NVC="${NVC:-/usr/local/src/nvc-build/bin/nvc}"
LIB="${NVC_LIBDIR:-/usr/local/src/nvc-build/lib}"
STD=2008
WORK="$HERE/work"
rm -rf "$WORK"; mkdir -p "$WORK"
A=(-M 256m -H 256m --std=$STD --work="$WORK" -L "$LIB")

ana() { $NVC "${A[@]}" -a "$@" >/dev/null 2>&1; }
ana "$HERE/leaf_add.vhd"  || { echo "analyze leaf_add FAILED"; exit 2; }
ana "$HERE/leaf_dff.vhd"  || { echo "analyze leaf_dff FAILED"; exit 2; }
ana "$HERE/hier_top.vhd"  || { echo "analyze hier_top FAILED"; exit 2; }
ana "$HERE/hier3_top.vhd" || { echo "analyze hier3_top FAILED"; exit 2; }
ana "$HERE/hier_tb.vhd"   || { echo "analyze hier_tb FAILED"; exit 2; }
ana "$HERE/hier3_tb.vhd"  || { echo "analyze hier3_tb FAILED"; exit 2; }
# generic-instance cases (variant naming): gadder = truncation-safe adder,
# greduce = width-sensitive AND-reduce (proves a dedup bug would miscompile).
ana "$HERE/gadder.vhd"    || { echo "analyze gadder FAILED";   exit 2; }
ana "$HERE/gen_top.vhd"   || { echo "analyze gen_top FAILED";  exit 2; }
ana "$HERE/gen_tb.vhd"    || { echo "analyze gen_tb FAILED";   exit 2; }
ana "$HERE/greduce.vhd"   || { echo "analyze greduce FAILED";  exit 2; }
ana "$HERE/gred_top.vhd"  || { echo "analyze gred_top FAILED"; exit 2; }
ana "$HERE/gred_tb.vhd"   || { echo "analyze gred_tb FAILED";  exit 2; }
# aggregate forms: named / range / positional+others / choice-list slot-fill.
ana "$HERE/agg_top.vhd"   || { echo "analyze agg_top FAILED";  exit 2; }
ana "$HERE/agg_tb.vhd"    || { echo "analyze agg_tb FAILED";   exit 2; }
# counter-controlled (nested) while-loops -> Verilog for (yosys unrolls).
ana "$HERE/whl_top.vhd"   || { echo "analyze whl_top FAILED";  exit 2; }
ana "$HERE/whl_tb.vhd"    || { echo "analyze whl_tb FAILED";   exit 2; }
# Mealy combinational-boundary output (y=acc+x) — proves the bridge re-settles
# combinational outputs on input-change deltas (NVC_ACCEL_NO_SETTLE=1 -> wrong).
ana "$HERE/meal_top.vhd"  || { echo "analyze meal_top FAILED"; exit 2; }
ana "$HERE/meal_tb.vhd"   || { echo "analyze meal_tb FAILED";  exit 2; }

fails=0
run_case() {
  local tb="$1"
  $NVC "${A[@]}" -e "$tb" >/dev/null 2>&1 || { echo "  $tb: ELABORATE FAILED"; fails=$((fails+1)); return; }
  local ref acc instm
  ref=$($NVC "${A[@]}" -r "$tb" 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
  local out
  out=$(NVC_ACCEL_MIN_MODULES=1 NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 NVC_ACCEL_CC=cc \
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

echo "=== whole-subtree accel test ==="
run_case hier_tb
run_case hier3_tb
run_case gen_tb
run_case gred_tb
run_case agg_tb
run_case whl_tb
run_case meal_tb
echo "=== $fails failure(s) ==="
exit $fails
