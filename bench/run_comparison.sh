#!/bin/bash
# run_comparison.sh — Build and benchmark all pglz variants
#
# Usage: ./run_comparison.sh [--md]

set -e

CC="${CC:-clang}"
CFLAGS="-O2 -DFRONTEND -I./include"
EXTRA_ARGS="${@}"

VARIANTS=(
    "baseline:pg_lzcompress_baseline.c"
    "fibonacci_hash:pg_lzcompress_fibonacci_hash.c"
    "skip_after_match:pg_lzcompress_skip_after_match.c"
    "combined_ai:pg_lzcompress_combined_ai.c"
)

echo "=== Building variants ==="
for entry in "${VARIANTS[@]}"; do
    IFS=: read -r name src <<< "$entry"
    echo "  Building $name..."
    $CC $CFLAGS -o "bench_${name}" bench_pglz.c "$src" 2>&1
    echo "    OK"
done

echo ""
echo "=== Running benchmarks ==="
echo "  (pinned to CPU 0 via taskset)"
echo ""

for entry in "${VARIANTS[@]}"; do
    IFS=: read -r name src <<< "$entry"
    echo "--- Running $name ---"
    taskset -c 0 "./bench_${name}" "$name" $EXTRA_ARGS
    echo ""
done

echo "=== Done ==="
