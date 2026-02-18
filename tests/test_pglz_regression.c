/*
 * test_pglz_regression.c — Deterministic regression tests for pglz.
 *
 * Tests round-trip correctness across:
 *   Sizes: 0, 1, 2, 3, 4, 5, 2048, 4096, 4097
 *   Patterns: random, zeros, same-byte, ascending, repeating-4-byte
 *   Strategies: PGLZ_strategy_default, PGLZ_strategy_always
 *
 * Compile:
 *   clang -g -O2 -DFRONTEND -fsanitize=address,undefined \
 *     -I../bench/include \
 *     -o test_pglz_regression test_pglz_regression.c ../bench/pg_lzcompress_baseline.c
 *
 * Run:
 *   ./test_pglz_regression
 */

#define FRONTEND 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "common/pg_lzcompress.h"

/* ----------
 * Test sizes from SPEC.md
 * ----------
 */
static const int test_sizes[] = { 0, 1, 2, 3, 4, 5, 2048, 4096, 4097 };
#define NUM_SIZES (sizeof(test_sizes) / sizeof(test_sizes[0]))

/* ----------
 * Pattern generators
 * ----------
 */

/* Simple deterministic PRNG */
static uint64_t rng_state;

static void rng_seed(uint64_t seed)
{
    rng_state = seed ? seed : 1;
}

static uint8_t rng_byte(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint8_t)(rng_state & 0xFF);
}

typedef void (*gen_fn)(char *buf, int len);

static void gen_random(char *buf, int len)
{
    rng_seed(42);
    for (int i = 0; i < len; i++)
        buf[i] = (char)rng_byte();
}

static void gen_zeros(char *buf, int len)
{
    memset(buf, 0, len);
}

static void gen_same_byte(char *buf, int len)
{
    memset(buf, 0xAA, len);
}

static void gen_ascending(char *buf, int len)
{
    for (int i = 0; i < len; i++)
        buf[i] = (char)(i & 0xFF);
}

static void gen_repeating_4byte(char *buf, int len)
{
    /* Repeating 4-byte pattern: 0xDE 0xAD 0xBE 0xEF */
    const char pattern[4] = { (char)0xDE, (char)0xAD, (char)0xBE, (char)0xEF };
    for (int i = 0; i < len; i++)
        buf[i] = pattern[i & 3];
}

/* Pattern with matches that differ at byte 4 (tests 4-byte memcmp edge case) */
static void gen_3byte_matches(char *buf, int len)
{
    /*
     * Create data where 3-byte matches exist but 4th byte differs.
     * This is the case that Step 4 (4-byte memcmp) sacrifices for speed.
     */
    const char base[] = "ABC";
    for (int i = 0; i < len; i++)
    {
        if ((i % 4) < 3)
            buf[i] = base[i % 3];
        else
            buf[i] = (char)(i & 0xFF);  /* varying 4th byte */
    }
}

/* All hash to same bucket — pathological case */
static void gen_hash_collision(char *buf, int len)
{
    /* All identical bytes → all 4-byte windows hash the same */
    memset(buf, 'X', len);
}

/* Sparse matches at history-wrap boundary */
static void gen_boundary_4096(char *buf, int len)
{
    rng_seed(123);
    for (int i = 0; i < len; i++)
        buf[i] = (char)rng_byte();

    /* Plant identical 8-byte sequences at offset 0 and 4090 */
    if (len > 4098)
    {
        const char marker[] = "MATCHME!";
        memcpy(buf, marker, 8);
        memcpy(buf + 4090, marker, 8);
    }
}

typedef struct
{
    const char *name;
    gen_fn generate;
} Pattern;

static const Pattern patterns[] = {
    { "random",            gen_random },
    { "zeros",             gen_zeros },
    { "same-byte",         gen_same_byte },
    { "ascending",         gen_ascending },
    { "repeating-4byte",   gen_repeating_4byte },
    { "3byte-matches",     gen_3byte_matches },
    { "hash-collision",    gen_hash_collision },
    { "boundary-4096",     gen_boundary_4096 },
};
#define NUM_PATTERNS (sizeof(patterns) / sizeof(patterns[0]))

/* ----------
 * Test runner
 * ----------
 */
static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;

static void
test_roundtrip(const char *pattern_name, int size, gen_fn generate,
               const PGLZ_Strategy *strategy, const char *strategy_name)
{
    char *input = NULL;
    char *compressed = NULL;
    char *decompressed = NULL;
    int32 clen, dlen;
    int output_size;

    if (size == 0)
    {
        /* pglz_compress with size 0 should fail gracefully */
        compressed = malloc(16);
        if (!compressed) goto oom;

        clen = pglz_compress("", 0, compressed, strategy);
        if (clen < 0)
        {
            /* Expected: 0 bytes → compression fails */
            tests_skipped++;
            free(compressed);
            return;
        }
        /* If it succeeded, verify round-trip */
        dlen = pglz_decompress(compressed, clen, NULL, 0, true);
        if (dlen == 0)
        {
            tests_passed++;
        }
        else
        {
            fprintf(stderr, "FAIL: %s/%d/%s: decompress returned %d (expected 0)\n",
                    pattern_name, size, strategy_name, dlen);
            tests_failed++;
        }
        free(compressed);
        return;
    }

    input = malloc(size);
    output_size = PGLZ_MAX_OUTPUT(size);
    compressed = malloc(output_size);
    decompressed = malloc(size);

    if (!input || !compressed || !decompressed)
        goto oom;

    generate(input, size);

    clen = pglz_compress(input, size, compressed, strategy);

    if (clen < 0)
    {
        /* Compression failed (incompressible or too small) — OK */
        printf("  SKIP: %-18s %5d bytes  %-10s  (compression returned -1)\n",
               pattern_name, size, strategy_name);
        tests_skipped++;
        goto cleanup;
    }

    /* Verify compressed size is reasonable */
    if (clen > output_size)
    {
        fprintf(stderr, "FAIL: %s/%d/%s: compressed size %d > output buffer %d!\n",
                pattern_name, size, strategy_name, clen, output_size);
        tests_failed++;
        goto cleanup;
    }

    dlen = pglz_decompress(compressed, clen, decompressed, size, true);

    if (dlen != size)
    {
        fprintf(stderr, "FAIL: %s/%d/%s: decompress returned %d bytes (expected %d)\n",
                pattern_name, size, strategy_name, dlen, size);
        tests_failed++;
        goto cleanup;
    }

    if (memcmp(input, decompressed, size) != 0)
    {
        fprintf(stderr, "FAIL: %s/%d/%s: decompressed data differs from input!\n",
                pattern_name, size, strategy_name);
        /* Find first difference */
        for (int i = 0; i < size; i++)
        {
            if (input[i] != decompressed[i])
            {
                fprintf(stderr, "  First difference at byte %d: 0x%02x vs 0x%02x\n",
                        i, (unsigned char)input[i], (unsigned char)decompressed[i]);
                break;
            }
        }
        tests_failed++;
        goto cleanup;
    }

    printf("  PASS: %-18s %5d bytes  %-10s  (ratio: %.1f%%)\n",
           pattern_name, size, strategy_name,
           (double)clen / size * 100.0);
    tests_passed++;

cleanup:
    free(input);
    free(compressed);
    free(decompressed);
    return;

oom:
    fprintf(stderr, "FAIL: %s/%d/%s: out of memory\n",
            pattern_name, size, strategy_name);
    tests_failed++;
    free(input);
    free(compressed);
    free(decompressed);
}

/* ----------
 * Main
 * ----------
 */
int
main(int argc, char **argv)
{
    printf("pglz deterministic regression test\n");
    printf("===================================\n\n");

    printf("Testing with PGLZ_strategy_always:\n");
    for (int p = 0; p < (int)NUM_PATTERNS; p++)
    {
        for (int s = 0; s < (int)NUM_SIZES; s++)
        {
            test_roundtrip(patterns[p].name, test_sizes[s],
                           patterns[p].generate,
                           PGLZ_strategy_always, "always");
        }
    }

    printf("\nTesting with PGLZ_strategy_default:\n");
    for (int p = 0; p < (int)NUM_PATTERNS; p++)
    {
        for (int s = 0; s < (int)NUM_SIZES; s++)
        {
            test_roundtrip(patterns[p].name, test_sizes[s],
                           patterns[p].generate,
                           PGLZ_strategy_default, "default");
        }
    }

    printf("\n===================================\n");
    printf("Results: %d passed, %d failed, %d skipped\n",
           tests_passed, tests_failed, tests_skipped);

    if (tests_failed > 0)
    {
        fprintf(stderr, "\n*** %d TEST(S) FAILED ***\n", tests_failed);
        return 1;
    }

    printf("\nAll tests passed.\n");
    return 0;
}
