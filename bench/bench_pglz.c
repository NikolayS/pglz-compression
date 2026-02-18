/*
 * bench_pglz.c — Standalone benchmark for pglz compression variants.
 *
 * Measures throughput (MiB/s), compression ratio, and latency per call
 * across multiple input types and sizes.
 *
 * Compile:
 *   gcc -O2 -DFRONTEND -I./include -o bench_pglz bench_pglz.c <variant>.c
 *
 * Run:
 *   taskset -c 0 ./bench_pglz
 *
 * Or use the Makefile.
 */

#define FRONTEND 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>

#include "common/pg_lzcompress.h"

/* ----------
 * Configuration
 * ----------
 */
#define WARMUP_ITERS    100
#define MIN_ITERS       1000
#define MAX_ITERS       1000000
#define MIN_BENCH_NS    500000000LL  /* Run for at least 500ms per test */

/* Test sizes */
static const int test_sizes[] = { 512, 2048, 4096, 65536, 1048576 };
#define NUM_SIZES (sizeof(test_sizes) / sizeof(test_sizes[0]))

/* ----------
 * Timing helpers (clock_gettime CLOCK_MONOTONIC)
 * ----------
 */
static inline int64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ----------
 * Comparison function for qsort (int64_t)
 * ----------
 */
static int
cmp_i64(const void *a, const void *b)
{
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

/* ----------
 * Data generators
 * ----------
 */

/* Simple xorshift64 PRNG for reproducibility */
static uint64_t rng_state = 0x123456789ABCDEF0ULL;

static uint64_t
xorshift64(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static void
rng_seed(uint64_t seed)
{
    rng_state = seed ? seed : 1;
}

/* Random (incompressible) data */
static void
gen_random(char *buf, int len)
{
    rng_seed(42);
    for (int i = 0; i < len; i++)
        buf[i] = (char)(xorshift64() & 0xFF);
}

/* English-like compressible text */
static const char *words[] = {
    "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ",
    "and ", "then ", "runs ", "away ", "from ", "here ", "to ", "there ",
    "with ", "some ", "data ", "that ", "is ", "quite ", "compressible ",
    "in ", "nature ", "because ", "it ", "contains ", "many ", "repeated ",
    "words ", "and ", "phrases ", "which ", "help ", "the ", "compression ",
    "algorithm ", "find ", "matches ", "in ", "its ", "history ", "table ",
    "PostgreSQL ", "is ", "an ", "advanced ", "open ", "source ", "relational ",
    "database ", "management ", "system ", "that ", "supports ", "both ",
    "SQL ", "and ", "JSON ", "querying ", "for ", "all ", "workloads ",
    NULL
};

static void
gen_english(char *buf, int len)
{
    int pos = 0;
    int widx = 0;
    rng_seed(42);

    while (pos < len)
    {
        /* Pick words in a semi-random but repeating pattern */
        const char *w = words[widx];
        if (w == NULL) {
            widx = 0;
            w = words[0];
        }
        int wlen = strlen(w);
        int tocopy = (pos + wlen > len) ? (len - pos) : wlen;
        memcpy(buf + pos, w, tocopy);
        pos += tocopy;
        widx++;
        /* Occasionally restart from a random word */
        if ((xorshift64() & 0x7) == 0)
            widx = xorshift64() % 60;  /* roughly number of words */
    }
}

/* Highly redundant (repeated pattern) */
static void
gen_redundant(char *buf, int len)
{
    /* 16-byte repeating pattern */
    const char pattern[] = "ABCDEFGHIJKLMNOP";
    for (int i = 0; i < len; i++)
        buf[i] = pattern[i % 16];
}

/* pgbench_accounts-like rows:
 * aid (int4) | bid (int4) | abalance (int4) | filler (char(84))
 * Typical row: ~100 bytes, mostly zero-filled filler
 */
static void
gen_pgbench(char *buf, int len)
{
    rng_seed(42);
    int pos = 0;
    int aid = 1;

    while (pos < len)
    {
        /* Simulate a simple text-like row representation */
        char row[128];
        int bid = (aid - 1) / 100000 + 1;
        int abalance = (int)(xorshift64() % 200001) - 100000;

        /* Format: "aid|bid|abalance|<84 spaces>\n" */
        int rlen = snprintf(row, sizeof(row), "%d|%d|%d|", aid, bid, abalance);
        /* Fill filler with spaces */
        int filler_len = 84;
        if (rlen + filler_len + 1 > (int)sizeof(row))
            filler_len = sizeof(row) - rlen - 2;
        memset(row + rlen, ' ', filler_len);
        rlen += filler_len;
        row[rlen++] = '\n';

        int tocopy = (pos + rlen > len) ? (len - pos) : rlen;
        memcpy(buf + pos, row, tocopy);
        pos += tocopy;
        aid++;
    }
}

/* ----------
 * Input type descriptor
 * ----------
 */
typedef struct
{
    const char *name;
    void (*generate)(char *buf, int len);
} InputType;

static const InputType input_types[] = {
    { "random",     gen_random },
    { "english",    gen_english },
    { "redundant",  gen_redundant },
    { "pgbench",    gen_pgbench },
};
#define NUM_TYPES (sizeof(input_types) / sizeof(input_types[0]))

/* ----------
 * Benchmark result
 * ----------
 */
typedef struct
{
    const char *type_name;
    int         input_size;
    int         iters;
    int32       compressed_size;
    double      ratio;          /* compressed / original */
    double      throughput_mib; /* MiB/s based on input size */
    double      median_us;      /* microseconds per call */
    double      p99_us;         /* p99 latency */
    double      mean_us;
    bool        compress_ok;    /* did compression succeed? */
} BenchResult;

/* ----------
 * Run a single benchmark: compress `iters` times, record per-call latencies.
 * Returns the number of iterations actually run.
 * ----------
 */
static int
run_bench(const char *input, int input_len, char *output, int output_len,
          int64_t *latencies, int max_iters)
{
    int iters = 0;
    int64_t total_ns = 0;

    /* Warmup */
    for (int i = 0; i < WARMUP_ITERS; i++)
    {
        pglz_compress(input, input_len, output, PGLZ_strategy_always);
    }

    /* Measured iterations */
    while (iters < max_iters)
    {
        int64_t t0 = now_ns();
        pglz_compress(input, input_len, output, PGLZ_strategy_always);
        int64_t t1 = now_ns();

        latencies[iters] = t1 - t0;
        total_ns += (t1 - t0);
        iters++;

        /* Run at least MIN_ITERS, then check if we've hit MIN_BENCH_NS */
        if (iters >= MIN_ITERS && total_ns >= MIN_BENCH_NS)
            break;
    }

    return iters;
}

/* ----------
 * Verify compression round-trip
 * ----------
 */
static bool
verify_roundtrip(const char *input, int input_len)
{
    int output_len = PGLZ_MAX_OUTPUT(input_len);
    char *compressed = malloc(output_len);
    char *decompressed = malloc(input_len);

    if (!compressed || !decompressed) {
        free(compressed);
        free(decompressed);
        return false;
    }

    int32 clen = pglz_compress(input, input_len, compressed,
                               PGLZ_strategy_always);
    if (clen < 0) {
        /* Compression failed (e.g., random data) — that's OK */
        free(compressed);
        free(decompressed);
        return true;  /* not an error, just incompressible */
    }

    int32 dlen = pglz_decompress(compressed, clen, decompressed,
                                 input_len, true);
    bool ok = (dlen == input_len && memcmp(input, decompressed, input_len) == 0);

    if (!ok) {
        fprintf(stderr, "ROUNDTRIP FAILURE: input_len=%d, clen=%d, dlen=%d\n",
                input_len, clen, dlen);
    }

    free(compressed);
    free(decompressed);
    return ok;
}

/* ----------
 * Format size nicely
 * ----------
 */
static const char *
fmt_size(int size)
{
    static char buf[4][32];
    static int idx = 0;
    char *b = buf[idx++ & 3];

    if (size >= 1048576)
        snprintf(b, 32, "%dM", size / 1048576);
    else if (size >= 1024)
        snprintf(b, 32, "%dK", size / 1024);
    else
        snprintf(b, 32, "%dB", size);
    return b;
}

/* ----------
 * Print results as a formatted table
 * ----------
 */
static void
print_results(BenchResult *results, int nresults, const char *variant_name)
{
    printf("\n");
    printf("=== %s ===\n", variant_name);
    printf("\n");
    printf("%-12s %8s %8s %10s %10s %10s %10s %8s\n",
           "Type", "Size", "CSize", "Ratio", "MiB/s", "Med(µs)", "P99(µs)", "Iters");
    printf("%-12s %8s %8s %10s %10s %10s %10s %8s\n",
           "----", "----", "-----", "-----", "-----", "------", "------", "-----");

    for (int i = 0; i < nresults; i++)
    {
        BenchResult *r = &results[i];
        if (r->compress_ok && r->compressed_size >= 0)
        {
            printf("%-12s %8s %8d %9.2f%% %10.1f %10.2f %10.2f %8d\n",
                   r->type_name,
                   fmt_size(r->input_size),
                   r->compressed_size,
                   r->ratio * 100.0,
                   r->throughput_mib,
                   r->median_us,
                   r->p99_us,
                   r->iters);
        }
        else
        {
            printf("%-12s %8s %8s %10s %10.1f %10.2f %10.2f %8d\n",
                   r->type_name,
                   fmt_size(r->input_size),
                   "FAIL",
                   "N/A",
                   r->throughput_mib,
                   r->median_us,
                   r->p99_us,
                   r->iters);
        }
    }
}

/* ----------
 * Print results in markdown table format (for RESULTS.md)
 * ----------
 */
static void
print_results_md(BenchResult *results, int nresults, const char *variant_name)
{
    printf("\n### %s\n\n", variant_name);
    printf("| Type | Size | Compressed | Ratio | MiB/s | Median µs | P99 µs | Iters |\n");
    printf("|------|------|-----------|-------|-------|-----------|--------|-------|\n");

    for (int i = 0; i < nresults; i++)
    {
        BenchResult *r = &results[i];
        if (r->compress_ok && r->compressed_size >= 0)
        {
            printf("| %-10s | %6s | %9d | %6.2f%% | %8.1f | %9.2f | %8.2f | %6d |\n",
                   r->type_name,
                   fmt_size(r->input_size),
                   r->compressed_size,
                   r->ratio * 100.0,
                   r->throughput_mib,
                   r->median_us,
                   r->p99_us,
                   r->iters);
        }
        else
        {
            printf("| %-10s | %6s | %9s | %6s | %8.1f | %9.2f | %8.2f | %6d |\n",
                   r->type_name,
                   fmt_size(r->input_size),
                   "FAIL",
                   "N/A",
                   r->throughput_mib,
                   r->median_us,
                   r->p99_us,
                   r->iters);
        }
    }
}

/* ----------
 * Main
 * ----------
 */
int
main(int argc, char **argv)
{
    bool markdown = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--md") == 0 || strcmp(argv[i], "--markdown") == 0)
            markdown = true;
    }

    const char *variant = "pglz";
    if (argc > 1 && argv[1][0] != '-')
        variant = argv[1];

    int total_tests = NUM_TYPES * NUM_SIZES;
    BenchResult *results = calloc(total_tests, sizeof(BenchResult));
    int64_t *latencies = malloc(MAX_ITERS * sizeof(int64_t));

    if (!results || !latencies) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    /* Allocate max-sized buffers */
    int max_size = test_sizes[NUM_SIZES - 1];
    char *input = malloc(max_size);
    int output_len = PGLZ_MAX_OUTPUT(max_size);
    char *output = malloc(output_len);

    if (!input || !output) {
        fprintf(stderr, "Out of memory for buffers\n");
        return 1;
    }

    int ridx = 0;
    for (int t = 0; t < (int)NUM_TYPES; t++)
    {
        for (int s = 0; s < (int)NUM_SIZES; s++)
        {
            int size = test_sizes[s];
            const InputType *itype = &input_types[t];

            /* Generate input */
            itype->generate(input, size);

            /* Verify round-trip first */
            if (!verify_roundtrip(input, size)) {
                fprintf(stderr, "ERROR: Round-trip verification failed for %s/%s!\n",
                        itype->name, fmt_size(size));
                results[ridx].type_name = itype->name;
                results[ridx].input_size = size;
                results[ridx].compress_ok = false;
                ridx++;
                continue;
            }

            /* Check if compression succeeds */
            int32 clen = pglz_compress(input, size, output,
                                       PGLZ_strategy_always);

            /* Run benchmark */
            int iters = run_bench(input, size, output, output_len,
                                  latencies, MAX_ITERS);

            /* Sort latencies for percentiles */
            qsort(latencies, iters, sizeof(int64_t), cmp_i64);

            double median_ns = latencies[iters / 2];
            double p99_ns = latencies[(int)(iters * 0.99)];
            double total_ns = 0;
            for (int i = 0; i < iters; i++)
                total_ns += latencies[i];
            double mean_ns = total_ns / iters;

            /* Throughput: based on total input bytes processed / total time */
            double throughput_mib = ((double)size * iters / (1024.0 * 1024.0)) /
                                   (total_ns / 1e9);

            BenchResult *r = &results[ridx];
            r->type_name = itype->name;
            r->input_size = size;
            r->iters = iters;
            r->compressed_size = clen;
            r->ratio = (clen >= 0) ? (double)clen / size : -1.0;
            r->throughput_mib = throughput_mib;
            r->median_us = median_ns / 1000.0;
            r->p99_us = p99_ns / 1000.0;
            r->mean_us = mean_ns / 1000.0;
            r->compress_ok = true;

            if (!markdown) {
                fprintf(stderr, "  %-12s %8s: %.1f MiB/s, ratio=%.2f%%, median=%.2f µs (%d iters)\n",
                        itype->name, fmt_size(size),
                        throughput_mib,
                        (clen >= 0) ? (double)clen / size * 100.0 : -1.0,
                        median_ns / 1000.0,
                        iters);
            }

            ridx++;
        }
    }

    /* Print results */
    if (markdown)
        print_results_md(results, ridx, variant);
    else
        print_results(results, ridx, variant);

    free(input);
    free(output);
    free(latencies);
    free(results);

    return 0;
}
