#!/bin/bash
set -u
NVC="${NVC:-/usr/local/src/nvc-build/bin/nvc}"
LIB="${NVC_LIBDIR:-/usr/local/src/nvc-build/lib}"
STD=2008
A=(-M 256m -H 256m --std=$STD -L "$LIB")

test_design() {
  local dut_base="$1"
  local vhd="$1.vhd"
  local tb_vhd="${dut_base}_tb.vhd"
  local work="/tmp/accel_test_${dut_base}"
  
  rm -rf "$work"
  mkdir -p "$work"
  
  echo "=== Testing $dut_base ==="
  
  # Analyze
  $NVC "${A[@]}" --work="$work" -a "$vhd" "$tb_vhd" >/dev/null 2>&1 || { echo "  ANALYZE FAILED"; return 1; }
  
  # Elaborate
  $NVC "${A[@]}" --work="$work" -e "${dut_base}_tb" >/dev/null 2>&1 || { echo "  ELABORATE FAILED"; return 1; }
  
  # Reference run (interpreted)
  ref=$($NVC "${A[@]}" --work="$work" -r "${dut_base}_tb" 2>&1 | grep -oE 'Y=[0-9]+' | tail -1)
  
  # Accel run
  out=$(NVC_ACCEL_MIN_MODULES=1 NVC_ACCEL=1 NVC_ACCEL_JIT=1 NVC_ACCEL_FROM_VHDL=1 NVC_ACCEL_CC=cc \
        $NVC "${A[@]}" --work="$work" -r "${dut_base}_tb" 2>&1)
  acc=$(echo "$out" | grep -oE 'Y=[0-9]+' | tail -1)
  
  emitted=$(echo "$out" | grep -c 'emitted subtree' || true)
  notxl=$(echo "$out" | grep -c 'not fully translatable' || true)
  synthfail=$(echo "$out" | grep -c 'synth failed' || true)
  instm=$(echo "$out" | grep -ciE 'accel-jit:.*(reroute|active|installed)|subtree.*active' || true)
  
  printf "  ref=%-6s accel=%-6s  subtree[emit=%s notxl=%s synthfail=%s install=%s]" \
         "${ref:-?}" "${acc:-?}" "$emitted" "$notxl" "$synthfail" "$instm"
  
  if [ -n "$ref" ] && [ "$ref" = "$acc" ]; then 
    echo "  -> MATCH"
    return 0
  else 
    echo "  -> MISMATCH"
    return 1
  fi
}

fails=0
test_design churn || fails=$((fails+1))
test_design twochunk || fails=$((fails+1))
test_design pipe || fails=$((fails+1))
test_design wchurn || fails=$((fails+1))
test_design wwide || fails=$((fails+1))

echo "=== $fails failure(s) ==="
exit $fails
