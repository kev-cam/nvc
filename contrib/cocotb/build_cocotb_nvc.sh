#!/bin/bash
#
# build_cocotb_nvc.sh -- Build and run CocoTB tests against NVC via direct bridge.
#
# Usage:
#   build_cocotb_nvc.sh <test_dir> [--run]
#
# Example:
#   build_cocotb_nvc.sh /usr/local/src/isqed-challenge/skeleton_envs/bastion_gpio
#   build_cocotb_nvc.sh /usr/local/src/isqed-challenge/skeleton_envs/bastion_gpio --run
#
# Environment:
#   NVC          - path to nvc binary (default: /usr/local/src/nvc/build/bin/nvc)
#   NVC_LIBDIR   - path to nvc libraries (default: /usr/local/src/nvc/build/lib)
#   NVC_SRC      - path to nvc source tree (default: /usr/local/src/nvc)
#   NVC_BUILD    - path to nvc build tree (default: /usr/local/src/nvc-build)
#   NUITKA       - use Nuitka compilation (default: 0, use interpreted mode)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NVC="${NVC:-/usr/local/src/nvc/build/bin/nvc}"
NVC_LIBDIR="${NVC_LIBDIR:-/usr/local/src/nvc/build/lib}"
NVC_SRC="${NVC_SRC:-/usr/local/src/nvc}"
NVC_BUILD="${NVC_BUILD:-/usr/local/src/nvc-build}"
NUITKA="${NUITKA:-0}"
ISQED="${ISQED:-/usr/local/src/isqed-challenge}"

TEST_DIR="$1"
DO_RUN=0
[[ "$2" == "--run" ]] && DO_RUN=1

if [[ -z "$TEST_DIR" ]]; then
    echo "Usage: $0 <test_dir> [--run]"
    echo "  test_dir: directory containing test_basic.py and Makefile"
    exit 1
fi

# Derive DUT name from directory
DUT_NAME="$(basename "$TEST_DIR")"
BUILD_DIR="/tmp/cocotb_nvc_build/${DUT_NAME}"
VHDL_FILE="/tmp/${DUT_NAME}.vhd"

echo "=== CocoTB-NVC Bridge Build ==="
echo "  DUT:       $DUT_NAME"
echo "  Test dir:  $TEST_DIR"
echo "  Build dir: $BUILD_DIR"
echo ""

# ---- Step 1: Build bridge shared library ----
echo "--- Step 1: Building nvc_cocotb_bridge.so ---"
BRIDGE_SO="${BUILD_DIR}/nvc_cocotb_bridge.so"
mkdir -p "$BUILD_DIR"

gcc -shared -fPIC -g -O1 \
    -I"${NVC_SRC}/src" \
    -I"${NVC_BUILD}/src" \
    -I"${NVC_SRC}/thirdparty" \
    "${SCRIPT_DIR}/nvc_cocotb_bridge.c" \
    -L"${NVC_BUILD}/lib" \
    -lnvc -lthirdparty \
    -Wl,-rpath,"${NVC_BUILD}/lib" \
    -o "$BRIDGE_SO" 2>&1

echo "  Built: $BRIDGE_SO"

# ---- Step 2: Preprocess CocoTB ----
echo "--- Step 2: Preprocessing CocoTB ---"
TEST_PY="${TEST_DIR}/test_basic.py"
AGENT_PY="${ISQED}/skeleton_envs/tl_ul_agent.py"

EXTRA_FILES=()
[[ -f "$AGENT_PY" ]] && EXTRA_FILES+=("$AGENT_PY")

python3 "${SCRIPT_DIR}/preprocess_cocotb.py" \
    "$TEST_PY" "${EXTRA_FILES[@]}" \
    -o "$BUILD_DIR"

# ---- Step 3: Translate DUT to VHDL (if not already done) ----
echo "--- Step 3: Translating DUT to VHDL ---"
DUT_SV="${ISQED}/duts/${DUT_NAME}/${DUT_NAME}.sv"
PKG_SV="${ISQED}/duts/common/dv_common_pkg.sv"

if [[ -f "$VHDL_FILE" ]]; then
    echo "  Using existing: $VHDL_FILE"
else
    IVERILOG="${IVERILOG:-/usr/local/src/iverilog/driver/iverilog}"
    IVL_LIBDIR="${IVL_LIBDIR:-/tmp/ivl}"

    "$IVERILOG" -B"$IVL_LIBDIR" -tvhdl -psv2vhdl=1 -g2012 \
        -o "$VHDL_FILE" "$PKG_SV" "$DUT_SV" 2>/dev/null
    echo "  Translated: $VHDL_FILE"
fi

# ---- Step 4: Compile VHDL with NVC ----
echo "--- Step 4: Compiling VHDL with NVC ---"
NVC_WORK="${BUILD_DIR}/nvc_work"
mkdir -p "$NVC_WORK"
cd "$NVC_WORK"
rm -rf work

$NVC --std=2040 -L "$NVC_LIBDIR" -a "$VHDL_FILE" 2>&1
echo "  VHDL analyzed"

# Elaborate
$NVC --std=2040 -L "$NVC_LIBDIR" -e "$DUT_NAME" 2>&1
echo "  Elaborated: $DUT_NAME"

# ---- Step 5: Run test ----
if [[ $DO_RUN -eq 1 ]]; then
    echo "--- Step 5: Running CocoTB test ---"

    export NVCB_BRIDGE_SO="$BRIDGE_SO"
    export PYTHONPATH="${BUILD_DIR}:${PYTHONPATH}"

    # For interpreted mode (no Nuitka), we need a small Python driver
    # that initializes the bridge and runs the CocoTB tests
    cat > "${BUILD_DIR}/cocotb_runner.py" << 'PYEOF'
import sys
import os

# Import our shimmed cocotb
import cocotb
from cocotb import simulator

def main():
    # Get root handle
    root = simulator.get_root_handle(None)
    if root is None:
        print("ERROR: Could not get root handle")
        return 1

    print(f"Root handle: {root.get_name_string()}")
    print(f"Simulator: {simulator.get_simulator_product()} {simulator.get_simulator_version()}")

    # Run event loop
    simulator.run_event_loop()
    return 0

if __name__ == '__main__':
    sys.exit(main())
PYEOF

    # Run with NVC loading the bridge
    echo "  Running: $NVC -r --load=$BRIDGE_SO $DUT_NAME"
    cd "$NVC_WORK"
    $NVC --std=2040 -L "$NVC_LIBDIR" \
        -r "$DUT_NAME" --load="$BRIDGE_SO" 2>&1 || true

    echo ""
    echo "=== Done ==="
else
    echo ""
    echo "=== Build complete. Use --run to execute. ==="
    echo "  To run manually:"
    echo "    cd $NVC_WORK"
    echo "    NVCB_BRIDGE_SO=$BRIDGE_SO PYTHONPATH=$BUILD_DIR \\"
    echo "      $NVC --std=2040 -L $NVC_LIBDIR -r $DUT_NAME --load=$BRIDGE_SO"
fi
