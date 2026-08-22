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
# signed comparison: vhdl2vlog wraps signed operands in $signed(...) so yosys
# sets A_SIGNED and gen_statemachine picks the signed compare (signed_expr / wslt).
# scmin = narrow (8b); wscmp = wide (96b, limb path). An unsigned compare miscounts.
ana "$HERE/scmin.vhd"     || { echo "analyze scmin FAILED";    exit 2; }
ana "$HERE/scmin_tb.vhd"  || { echo "analyze scmin_tb FAILED"; exit 2; }
ana "$HERE/wscmp.vhd"     || { echo "analyze wscmp FAILED";    exit 2; }
ana "$HERE/wscmp_tb.vhd"  || { echo "analyze wscmp_tb FAILED"; exit 2; }
# design-declared VHDL FUNCTIONS -- emit_function had NO coverage here at all.
# fnret   = straight-line, single trailing return: must translate and install.
# fnearly = same design with an EARLY return.  Verilog functions have no early
# exit, so the lowering `return x` -> `<name> = x` falls through and the trailing
# return overwrites the saturating one; vhdl2vlog must DECLINE rather than
# install that.  Both must reproduce the interpreter's Y.
ana "$HERE/fnret.vhd"      || { echo "analyze fnret FAILED";      exit 2; }
ana "$HERE/fnret_tb.vhd"   || { echo "analyze fnret_tb FAILED";   exit 2; }
ana "$HERE/fnearly.vhd"    || { echo "analyze fnearly FAILED";    exit 2; }
ana "$HERE/fnearly_tb.vhd" || { echo "analyze fnearly_tb FAILED"; exit 2; }
# CONCATENATION-ELEMENT WIDTH.  Verilog resizes operands everywhere except inside
# {}, so an element whose emitted self-determined width differs from its VHDL
# width silently shreds the vector.  mulcat puts a numeric_std `*` (VHDL wa+wb
# bits, Verilog max(wa,wb)) in a concat: vhdl2vlog must DECLINE.  This is the
# std_logic sibling of l3dcat_run.sh's logic3d bit-read case.
ana "$HERE/mulcat.vhd"     || { echo "analyze mulcat FAILED";     exit 2; }
ana "$HERE/mulcat_tb.vhd"  || { echo "analyze mulcat_tb FAILED";  exit 2; }
# ...and the SAME defect NESTED under a wrapper.  mulcat puts the `*` straight
# into the concat, so a one-node-deep guard sees it.  rszcat wraps it in
# `resize(...,16)`, which vhdl2vlog emits as a verbatim IDENTITY -- the guard
# only ever sees it if that identity hands the width sensitivity DOWN.  It must
# DECLINE (measured without the propagation: installs, Y=136746172 instead of
# 1768636092).  rszok is the control: the two resize-in-concat forms that ARE
# expressible ({N'b0,a}, and an identity whose widths really match) must still
# INSTALL -- otherwise the guard has just taken the accelerator away from
# sv2vhdl's commonest construct and nothing else here would notice.
ana "$HERE/rszcat.vhd"     || { echo "analyze rszcat FAILED";     exit 2; }
ana "$HERE/rszcat_tb.vhd"  || { echo "analyze rszcat_tb FAILED";  exit 2; }
ana "$HERE/rszok.vhd"      || { echo "analyze rszok FAILED";      exit 2; }
ana "$HERE/rszok_tb.vhd"   || { echo "analyze rszok_tb FAILED";   exit 2; }

fails=0
run_case() {
  local tb="$1"
  $NVC "${A[@]}" -e "$tb" >/dev/null 2>&1 || { echo "  $tb: ELABORATE FAILED"; fails=$((fails+1)); return; }
  local ref acc instm
  ref=$($NVC "${A[@]}" -r "$tb" 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
  local out
  out=$(NVC_ACCEL_MIN_MODULES=1 NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 NVC_ACCEL_CC="${NVC_ACCEL_CC:-gcc -O2}" \
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
run_case scmin_tb
run_case wscmp_tb
run_case fnret_tb
run_case fnearly_tb
run_case mulcat_tb
run_case rszcat_tb
run_case rszok_tb
echo "=== $fails failure(s) ==="
exit $fails
