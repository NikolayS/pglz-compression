#!/bin/bash
#
# build-and-test.sh — Build PG19dev and run pglz tests
#
# Usage:
#   ./scripts/build-and-test.sh baseline     # Test stock PG19 code
#   ./scripts/build-and-test.sh step1        # Test step-1 variant
#   ./scripts/build-and-test.sh step1 --bench # Also run benchmark
#
set -euo pipefail

STEP="${1:-baseline}"
DO_BENCH="${2:-}"
PG_SRC="/tmp/postgres-pglz"
PG_PGLZ="$PG_SRC/src/common/pg_lzcompress.c"
BENCH_DIR="$(cd "$(dirname "$0")/../bench" && pwd)"
PATCHES_DIR="$(cd "$(dirname "$0")/../patches" && pwd)"

echo "=== pglz build-and-test: $STEP ==="
echo "PG source: $PG_SRC"
echo "Benchmark dir: $BENCH_DIR"
echo ""

# --- Step 1: Install variant into PG tree ---
if [ "$STEP" = "baseline" ]; then
    echo "Using stock pg_lzcompress.c (baseline)"
    cp "$BENCH_DIR/pg_lzcompress_baseline.c" "$PG_PGLZ"
else
    VARIANT="$BENCH_DIR/variants/pg_lzcompress_${STEP}.c"
    if [ ! -f "$VARIANT" ]; then
        echo "ERROR: Variant file not found: $VARIANT"
        exit 1
    fi
    echo "Installing variant: $VARIANT"
    cp "$VARIANT" "$PG_PGLZ"
fi

# --- Step 2: Rebuild PG ---
echo ""
echo "--- Rebuilding PG19dev ---"
cd "$PG_SRC"
make -j$(nproc) -C src/common 2>&1 | tail -3
# Rebuild backend too (it links pg_lzcompress)
make -j$(nproc) 2>&1 | tail -3
echo "Build OK"

# --- Step 3: Run make check ---
echo ""
echo "--- Running make check ---"
make check 2>&1 | tail -5
echo ""

# --- Step 4: Standalone ASan test ---
echo "--- Running ASan roundtrip test ---"
cd "$BENCH_DIR"
if [ "$STEP" = "baseline" ]; then
    SRC="pg_lzcompress_baseline.c"
else
    SRC="variants/pg_lzcompress_${STEP}.c"
fi
gcc -O2 -g -fsanitize=address,undefined -DFRONTEND -I./include \
    -o "test_asan_${STEP}" test_asan_roundtrip.c "$SRC" -lm
ASAN_OPTIONS="detect_leaks=1" "./test_asan_${STEP}" 2>&1 | tail -5
echo ""

# --- Step 5: Benchmark (optional) ---
if [ "$DO_BENCH" = "--bench" ]; then
    echo "--- Running benchmark ---"
    gcc -O2 -DFRONTEND -I./include -o "bench_${STEP}" bench_pglz.c "$SRC" -lm
    taskset -c 0 "./bench_${STEP}" "$STEP" 2>&1
fi

echo ""
echo "=== $STEP: all tests passed ==="
