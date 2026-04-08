#!/bin/bash
#
# build_nuitka.sh -- Compile a translated CocoTB test with Nuitka,
# then patch the generated C with #line directives mapping back to
# the original Python source, then re-link.
#
# Usage: build_nuitka.sh <test_dir>
#
# Expects the test_dir to contain:
#   test_basic.py    (already translated by translate_cocotb.py)
#   nvcb.locmap      (file index registry from translate)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="$1"

if [[ -z "$TEST_DIR" || ! -d "$TEST_DIR" ]]; then
    echo "Usage: $0 <test_dir>" >&2
    exit 1
fi

cd "$TEST_DIR"

if [[ ! -f test_basic.py ]]; then
    echo "ERROR: $TEST_DIR/test_basic.py not found" >&2
    exit 1
fi
if [[ ! -f nvcb.locmap ]]; then
    echo "ERROR: $TEST_DIR/nvcb.locmap not found" >&2
    exit 1
fi

BUILD_DIR="nuitka_out/test_basic.build"
ORIG_C="$BUILD_DIR/module.test_basic.c"
SAVED_C="$BUILD_DIR/module.test_basic.c.orig"

# Step 1: Run Nuitka only if Python source is newer than the saved C
NEED_NUITKA=1
if [[ -f "$SAVED_C" && "$SAVED_C" -nt "test_basic.py" ]]; then
    echo "=== Skipping Nuitka (saved C is up to date) ==="
    NEED_NUITKA=0
fi

if [[ $NEED_NUITKA -eq 1 ]]; then
    echo "=== Running Nuitka (generating C) ==="
    python3 -m nuitka --module --unstripped test_basic.py --output-dir=nuitka_out 2>&1 | tail -5

    if [[ ! -f "$ORIG_C" ]]; then
        echo "ERROR: Nuitka didn't produce $ORIG_C" >&2
        exit 1
    fi
    # Save Nuitka's pristine output as .orig
    mv "$ORIG_C" "$SAVED_C"
fi

# Step 2: Patch the saved C with #line directives → produces module.test_basic.c
echo "=== Patching C with #line directives ==="
cp "$SAVED_C" "$ORIG_C"
python3 "$SCRIPT_DIR/postprocess_c.py" "$BUILD_DIR" nvcb.locmap

# Step 3: Recompile via scons. Since scons-debug.sh is a link/compile
# wrapper (not a Nuitka regenerate), it just builds whatever .c files
# are present.
echo "=== Re-linking with patched C ==="
rm -f "$BUILD_DIR/module.test_basic.os"
rm -f nuitka_out/test_basic.cpython*.so
bash "$BUILD_DIR/scons-debug.sh" 2>&1 | tail -5

# Step 4: Verify the .so exists
SO_FILE=$(ls nuitka_out/test_basic.cpython*.so 2>/dev/null | head -1)
if [[ -z "$SO_FILE" ]]; then
    echo "ERROR: rebuild failed, no .so produced" >&2
    exit 1
fi

echo ""
echo "=== Done ==="
echo "  Compiled: $SO_FILE"
echo "  Size: $(stat -c%s "$SO_FILE") bytes"

# Quick sanity check: count #line directives in the patched source
N_LINES=$(grep -c '^#line' "$BUILD_DIR/module.test_basic.c" || true)
echo "  #line directives: $N_LINES"
