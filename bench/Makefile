# Makefile for pglz standalone benchmark and tests
#
# Usage:
#   make baseline        — Build + run benchmark with stock PG19 code
#   make test-asan       — Build + run ASan/UBSan roundtrip tests
#   make test-asan-step1 — Test step-1 variant under ASan
#   make bench-step1     — Benchmark step-1 variant
#   make all-variants    — Benchmark all available variants
#   make clean

CC = gcc
CFLAGS_COMMON = -DFRONTEND -I./include -Wall -Wextra -Wno-unused-parameter
CFLAGS_OPT = $(CFLAGS_COMMON) -O2
CFLAGS_ASAN = $(CFLAGS_COMMON) -O2 -g -fsanitize=address,undefined
LDFLAGS_ASAN = -fsanitize=address,undefined
LIBS = -lm

BASELINE_SRC = pg_lzcompress_baseline.c

# Find all variant source files
VARIANT_SRCS = $(wildcard variants/pg_lzcompress_*.c)
VARIANT_NAMES = $(patsubst variants/pg_lzcompress_%.c,%,$(VARIANT_SRCS))

.PHONY: baseline test-asan clean all-variants help

help:
	@echo "Targets:"
	@echo "  make baseline          — Benchmark baseline PG19 pglz"
	@echo "  make test-asan         — ASan roundtrip test (baseline)"
	@echo "  make bench-<variant>   — Benchmark a variant"
	@echo "  make test-asan-<var>   — ASan test a variant"
	@echo "  make all-variants      — Benchmark all variants"
	@echo "  make clean"
	@echo ""
	@echo "Available variants: baseline $(VARIANT_NAMES)"

# --- Baseline ---

bench_baseline: bench_pglz.c $(BASELINE_SRC)
	$(CC) $(CFLAGS_OPT) -o $@ $^ $(LIBS)

baseline: bench_baseline
	taskset -c 0 ./bench_baseline baseline 2>&1

test_asan_baseline: test_asan_roundtrip.c $(BASELINE_SRC)
	$(CC) $(CFLAGS_ASAN) -o $@ $^ $(LIBS) $(LDFLAGS_ASAN)

test-asan: test_asan_baseline
	ASAN_OPTIONS="detect_leaks=1" ./test_asan_baseline

# --- Variant builds (pattern rules) ---

bench_%: bench_pglz.c variants/pg_lzcompress_%.c
	$(CC) $(CFLAGS_OPT) -o $@ $^ $(LIBS)

test_asan_%: test_asan_roundtrip.c variants/pg_lzcompress_%.c
	$(CC) $(CFLAGS_ASAN) -o $@ $^ $(LIBS) $(LDFLAGS_ASAN)

bench-step%: bench_step%
	taskset -c 0 ./bench_step$* step$* 2>&1

test-asan-step%: test_asan_step%
	ASAN_OPTIONS="detect_leaks=1" ./test_asan_step$*

# --- All variants ---

all-variants: baseline $(addprefix bench-,$(VARIANT_NAMES))

# --- Compare baseline vs variant ---

compare-%: bench_baseline bench_%
	@echo "=== BASELINE ===" && taskset -c 0 ./bench_baseline baseline
	@echo ""
	@echo "=== $* ===" && taskset -c 0 ./bench_$* $*

# --- PG19 in-tree build helpers ---

PG_SRC = /tmp/postgres-pglz
PG_PGLZ = $(PG_SRC)/src/common/pg_lzcompress.c

pg-check:
	cd $(PG_SRC) && make -j$$(nproc) && make check 2>&1 | tail -10

pg-install-step:
	@if [ -z "$(STEP_SRC)" ]; then echo "Usage: make pg-install-step STEP_SRC=variants/pg_lzcompress_step1.c"; exit 1; fi
	cp $(STEP_SRC) $(PG_PGLZ)
	cd $(PG_SRC) && make -j$$(nproc) -C src/common

# --- Clean ---

clean:
	rm -f bench_baseline bench_step* test_asan_baseline test_asan_step*
	rm -f *.o
