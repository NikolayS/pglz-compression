# Makefile for pglz test infrastructure
#
# Targets:
#   regression  — deterministic round-trip tests (ASan + UBSan)
#   fuzz        — libFuzzer target (ASan + UBSan + fuzzer)
#   bench       — benchmark harness (optimized, no sanitizers)
#   all         — build all

CC = clang
INCLUDES = -I../bench/include
BASELINE_SRC = ../bench/pg_lzcompress_baseline.c

# Sanitizer flags
SANITIZE_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ_FLAGS = -fsanitize=address,undefined,fuzzer -fno-sanitize-recover=all

# Common flags
CFLAGS_COMMON = -g -O2 -DFRONTEND -Wall -Wextra -Wno-unused-parameter $(INCLUDES)

.PHONY: all regression fuzz clean run-regression run-fuzz

all: test_pglz_regression fuzz_pglz

# Deterministic regression tests with ASan + UBSan
test_pglz_regression: test_pglz_regression.c $(BASELINE_SRC)
	$(CC) $(CFLAGS_COMMON) $(SANITIZE_FLAGS) -o $@ $^

regression: test_pglz_regression

run-regression: test_pglz_regression
	./test_pglz_regression

# Fuzz target with ASan + UBSan + libFuzzer
fuzz_pglz: fuzz_pglz.c $(BASELINE_SRC)
	$(CC) $(CFLAGS_COMMON) $(FUZZ_FLAGS) -o $@ $^

fuzz: fuzz_pglz

run-fuzz: fuzz_pglz
	mkdir -p corpus
	./fuzz_pglz corpus/ -max_len=1048576 -runs=10000000

# Short fuzz run for CI
run-fuzz-short: fuzz_pglz
	mkdir -p corpus
	./fuzz_pglz corpus/ -max_len=65536 -runs=100000

clean:
	rm -f test_pglz_regression fuzz_pglz
	rm -rf corpus/
