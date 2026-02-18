# Baseline Benchmark Results

**PG version:** PG19dev (master, commit 623a90c)
**Build:** `gcc -O2`, no ASan
**Platform:** Linux 6.8.0-94-generic x86_64
**Method:** `taskset -c 0`, min 500ms per test, 100 warmup iterations

## Compression Throughput

| Type | Size | Compressed | Ratio | MiB/s | Median µs | P99 µs | Iters |
|------|------|-----------|-------|-------|-----------|--------|-------|
| random | 512B | FAIL | N/A | 81.0 | 4.06 | 5.22 | 82988 |
| random | 2K | FAIL | N/A | 95.2 | 16.17 | 25.30 | 24363 |
| random | 4K | FAIL | N/A | 89.9 | 33.35 | 46.31 | 11508 |
| random | 64K | FAIL | N/A | 39.7 | 1264.02 | 4095.41 | 1000 |
| random | 1M | FAIL | N/A | 38.0 | 26177.76 | 31473.65 | 1000 |
| english | 512B | 327 | 63.87% | 96.9 | 3.57 | 4.32 | 99299 |
| english | 2K | 502 | 24.51% | 127.1 | 11.83 | 20.18 | 32546 |
| english | 4K | 601 | 14.67% | 128.8 | 23.32 | 33.78 | 16483 |
| english | 64K | 4446 | 6.78% | 85.8 | 580.18 | 3222.04 | 1000 |
| english | 1M | 67832 | 6.47% | 77.3 | 12947.04 | 15953.65 | 1000 |
| redundant | 512B | 25 | 4.88% | 151.6 | 2.47 | 2.92 | 155202 |
| redundant | 2K | 43 | 2.10% | 157.4 | 9.41 | 17.34 | 40386 |
| redundant | 4K | 65 | 1.59% | 158.3 | 18.53 | 28.01 | 20267 |
| redundant | 64K | 768 | 1.17% | 145.3 | 341.36 | 2350.61 | 1163 |
| redundant | 1M | 12022 | 1.15% | 146.0 | 6992.02 | 8989.28 | 1000 |
| pgbench | 512B | 83 | 16.21% | 131.4 | 2.76 | 3.23 | 134531 |
| pgbench | 2K | 253 | 12.35% | 116.9 | 12.72 | 21.43 | 29936 |
| pgbench | 4K | 452 | 11.04% | 106.1 | 28.81 | 39.47 | 13585 |
| pgbench | 64K | 6774 | 10.34% | 82.4 | 600.34 | 2951.22 | 1000 |
| pgbench | 1M | 107903 | 10.29% | 76.6 | 12948.95 | 20270.36 | 1000 |

## ASan/UBSan Roundtrip Tests

Baseline passes all 132 roundtrip tests across 44 sizes × 3 data types under ASan.

**UBSan findings (pre-existing):** Left shift of negative value in `pglz_hist_idx` macro
(line 410: `((_s)[0] << 6)` when `_s[0]` is negative). This is a pre-existing UB in stock
PG19 code — the hash function does `char << 6` where `char` may be signed. Step 1 will
fix this by casting to `unsigned char` in the inline function.

## Notes

- "FAIL" in random data means compression was abandoned (incompressible) — this is correct behavior
- `pglz_strategy_always` used for all tests (no early bailout)
- Degenerate (all-same-byte) data compresses extremely well as expected
- P99 latency on 64K+ inputs shows high variance — likely cache/TLB effects
