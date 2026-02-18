/*
 * test_asan_roundtrip.c — ASan-aware roundtrip correctness test for pglz.
 *
 * Tests compression/decompression roundtrip on carefully chosen boundary
 * sizes to catch buffer overreads (the v9 ASan bug was in pglz_hist_add).
 *
 * Compile:
 *   gcc -O2 -g -fsanitize=address,undefined -DFRONTEND -I./include \
 *       -o test_asan_roundtrip test_asan_roundtrip.c <variant>.c
 *
 * Run:
 *   ./test_asan_roundtrip
 */

#define FRONTEND 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "common/pg_lzcompress.h"

/* Simple xorshift64 PRNG */
static uint64_t rng_state = 0x123456789ABCDEF0ULL;
static uint64_t xorshift64(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static void rng_seed(uint64_t seed) { rng_state = seed ? seed : 1; }

/* Generate compressible data (repeated patterns with variation) */
static void gen_compressible(char *buf, int len)
{
    const char pattern[] = "The quick brown fox jumps over the lazy dog. ";
    int plen = strlen(pattern);
    for (int i = 0; i < len; i++)
        buf[i] = pattern[i % plen];
    /* Add some variation so it's not trivially repetitive */
    rng_seed(len);
    for (int i = 0; i < len / 10; i++) {
        int pos = xorshift64() % len;
        buf[pos] = 'A' + (xorshift64() % 26);
    }
}

/* Generate random data */
static void gen_random(char *buf, int len)
{
    rng_seed(42 + len);
    for (int i = 0; i < len; i++)
        buf[i] = (char)(xorshift64() & 0xFF);
}

/* Generate all-same-byte (pathological hash collision) data */
static void gen_degenerate(char *buf, int len)
{
    memset(buf, 'A', len);
}

static int test_roundtrip(const char *name, char *input, int len)
{
    int output_len = PGLZ_MAX_OUTPUT(len);
    char *compressed = malloc(output_len);
    char *decompressed = malloc(len + 1);  /* +1 to detect overwrite */

    if (!compressed || !decompressed) {
        fprintf(stderr, "  FAIL %s len=%d: malloc failed\n", name, len);
        free(compressed);
        free(decompressed);
        return 1;
    }

    /* Sentinel byte to detect overwrite */
    decompressed[len] = 0xAA;

    int32 clen = pglz_compress(input, len, compressed, PGLZ_strategy_always);

    if (clen >= 0) {
        /* Compression succeeded — verify roundtrip */
        int32 dlen = pglz_decompress(compressed, clen, decompressed, len, true);

        if (dlen != len) {
            fprintf(stderr, "  FAIL %s len=%d: decompress returned %d (expected %d)\n",
                    name, len, dlen, len);
            free(compressed);
            free(decompressed);
            return 1;
        }

        if (memcmp(input, decompressed, len) != 0) {
            fprintf(stderr, "  FAIL %s len=%d: data mismatch after roundtrip\n",
                    name, len);
            free(compressed);
            free(decompressed);
            return 1;
        }

        if ((unsigned char)decompressed[len] != 0xAA) {
            fprintf(stderr, "  FAIL %s len=%d: sentinel byte overwritten\n",
                    name, len);
            free(compressed);
            free(decompressed);
            return 1;
        }

        printf("  OK   %s len=%5d  compressed=%5d  ratio=%.1f%%\n",
               name, len, clen, (double)clen / len * 100.0);
    } else {
        /* Compression failed (incompressible) — that's fine */
        printf("  OK   %s len=%5d  (incompressible)\n", name, len);
    }

    free(compressed);
    free(decompressed);
    return 0;
}

int main(void)
{
    /* Critical boundary sizes from SPEC.md */
    int sizes[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8,
        15, 16, 17,         /* near min match threshold */
        31, 32, 33,         /* near strategy default min_input_size */
        63, 64, 65,
        127, 128, 129,
        255, 256, 257,
        511, 512, 513,
        1023, 1024, 1025,
        2047, 2048, 2049,   /* TOAST threshold */
        4093, 4094, 4095,   /* history wrap boundary */
        4096, 4097, 4098,
        8191, 8192, 8193,   /* hash table size boundary */
        16384,
        65536,
    };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);
    int failures = 0;
    int max_size = 65536;

    char *buf = malloc(max_size);
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    printf("=== pglz ASan roundtrip test ===\n\n");

    /* Test compressible data */
    printf("--- compressible ---\n");
    for (int i = 0; i < nsizes; i++) {
        if (sizes[i] > 0) {
            gen_compressible(buf, sizes[i]);
            failures += test_roundtrip("compressible", buf, sizes[i]);
        }
    }

    /* Test random data */
    printf("\n--- random ---\n");
    for (int i = 0; i < nsizes; i++) {
        if (sizes[i] > 0) {
            gen_random(buf, sizes[i]);
            failures += test_roundtrip("random", buf, sizes[i]);
        }
    }

    /* Test degenerate (all same byte) */
    printf("\n--- degenerate ---\n");
    for (int i = 0; i < nsizes; i++) {
        if (sizes[i] > 0) {
            gen_degenerate(buf, sizes[i]);
            failures += test_roundtrip("degenerate", buf, sizes[i]);
        }
    }

    /* Test zero-length */
    printf("\n--- edge cases ---\n");
    failures += test_roundtrip("zero-len", buf, 0);

    printf("\n=== %d failures ===\n", failures);

    free(buf);
    return failures ? 1 : 0;
}
