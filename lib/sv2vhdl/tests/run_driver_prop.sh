#!/bin/bash
# Run 'driver -> resolver -> 'other propagation tests.
# No resolver plugin needed -- resolvers are hand-written VHDL.
set -e

NVC=/usr/local/src/nvc/build/bin/nvc
SV2VHDL=/usr/local/src/nvc/lib/sv2vhdl
TESTDIR=$(dirname "$0")
WORKDIR=$(mktemp -d /tmp/test_driver_prop.XXXXXX)

cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

cd "$WORKDIR"

echo "=== Compiling packages and primitives ==="
$NVC --std=2040 -a $SV2VHDL/logic3d_types_pkg.vhd
$NVC --std=2040 -a $SV2VHDL/logic3ds_pkg.vhd
$NVC --std=2040 -a $SV2VHDL/sv_tran.vhd

echo "=== Compiling test ==="
$NVC --std=2040 -a $TESTDIR/test_driver_prop.vhd

PASS=0
FAIL=0

run_test() {
    local name=$1
    echo ""
    echo "=== Running $name ==="
    if $NVC --std=2040 -e "$name" 2>&1; then
        if $NVC --std=2040 -r "$name" 2>&1; then
            echo "--- $name: OK ---"
            ((PASS++))
            return 0
        fi
    fi
    echo "--- $name: FAILED ---"
    ((FAIL++))
    return 1
}

run_test resolved_test_passthru || true
run_test resolved_test_chain2 || true
run_test resolved_test_tied || true

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ] && echo "ALL PASS" || echo "SOME FAILURES"
exit $FAIL
