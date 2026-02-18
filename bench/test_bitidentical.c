/*
 * test_bitidentical.c — Verify that a variant produces identical compressed
 * output compared to a reference. Used to validate pure refactoring steps.
 *
 * This is compiled separately against each variant.
 * It writes compressed output to stdout as raw bytes for diffing.
 *
 * Usage:
 *   gcc -O2 -DFRONTEND -I./include -o dump_baseline test_bitidentical.c pg_lzcompress_baseline.c -lm
 *   gcc -O2 -DFRONTEND -I./include -o dump_step1 test_bitidentical.c variants/pg_lzcompress_step1.c -lm
 *   ./dump_baseline > /tmp/base.bin
 *   ./dump_step1 > /tmp/step1.bin
 *   cmp /tmp/base.bin /tmp/step1.bin && echo "IDENTICAL" || echo "DIFFER"
 */

#define FRONTEND 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "common/pg_lzcompress.h"

static uint64_t rng_state = 0x123456789ABCDEF0ULL;
static uint64_t xorshift64(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x; return x;
}
static void rng_seed(uint64_t seed) { rng_state = seed ? seed : 1; }

static void gen_compressible(char *buf, int len) {
    const char p[] = "The quick brown fox jumps over the lazy dog. PostgreSQL is great. ";
    int plen = strlen(p);
    for (int i = 0; i < len; i++) buf[i] = p[i % plen];
    rng_seed(len);
    for (int i = 0; i < len / 10; i++) {
        int pos = xorshift64() % len;
        buf[pos] = 'A' + (xorshift64() % 26);
    }
}

static void gen_degenerate(char *buf, int len) {
    memset(buf, 'A', len);
}

int main(void)
{
    int sizes[] = { 5, 32, 64, 128, 256, 512, 1024, 2048, 4096, 4097, 8192, 16384, 65536 };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);
    int max_size = 65536;
    char *input = malloc(max_size);
    char *output = malloc(PGLZ_MAX_OUTPUT(max_size));

    for (int i = 0; i < nsizes; i++) {
        int len = sizes[i];

        /* Compressible data */
        gen_compressible(input, len);
        int32 clen = pglz_compress(input, len, output, PGLZ_strategy_always);
        /* Write: 4-byte length marker, then compressed data */
        fwrite(&clen, sizeof(int32), 1, stdout);
        if (clen > 0)
            fwrite(output, 1, clen, stdout);

        /* Degenerate data */
        gen_degenerate(input, len);
        clen = pglz_compress(input, len, output, PGLZ_strategy_always);
        fwrite(&clen, sizeof(int32), 1, stdout);
        if (clen > 0)
            fwrite(output, 1, clen, stdout);
    }

    free(input);
    free(output);
    return 0;
}
