/*
 * test_cross_version.c — Cross-version decompression verification.
 *
 * Verifies that compressed output from any variant can be decompressed
 * by the stock PG decompressor, and vice versa.
 *
 * This is critical for rolling upgrades and logical replication.
 *
 * Compile:
 *   # Build two object files — stock and variant compressors
 *   clang -c -O2 -DFRONTEND -I../bench/include -o stock.o ../bench/pg_lzcompress_baseline.c
 *   clang -c -O2 -DFRONTEND -I../bench/include \
 *     -Dpglz_compress=pglz_compress_variant \
 *     -Dpglz_decompress=pglz_decompress_variant \
 *     -Dpglz_maximum_compressed_size=pglz_maximum_compressed_size_variant \
 *     -DPGLZ_strategy_default=PGLZ_strategy_default_variant \
 *     -DPGLZ_strategy_always=PGLZ_strategy_always_variant \
 *     -o variant.o ../bench/pg_lzcompress_combined_ai.c
 *   clang -O2 -DFRONTEND -fsanitize=address -I../bench/include \
 *     -o test_cross_version test_cross_version.c stock.o variant.o
 */

#define FRONTEND 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "common/pg_lzcompress.h"

/* Declarations for the variant (renamed via -D flags) */
extern int32 pglz_compress_variant(const char *source, int32 slen, char *dest,
                                   const PGLZ_Strategy *strategy);
extern int32 pglz_decompress_variant(const char *source, int32 slen, char *dest,
                                     int32 rawsize, bool check_complete);
extern const PGLZ_Strategy *const PGLZ_strategy_always_variant;

/* Simple PRNG */
static uint64_t rng_state;
static void rng_seed(uint64_t seed) { rng_state = seed ? seed : 1; }
static uint8_t rng_byte(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint8_t)(rng_state & 0xFF);
}

static void gen_english(char *buf, int len) {
    const char *words[] = {
        "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ",
        "PostgreSQL ", "compression ", "algorithm ", "data ", "table ",
        "and ", "then ", "runs ", "away ", "from ", "here ", "to ", "there ",
        NULL
    };
    rng_seed(42);
    int pos = 0, widx = 0;
    while (pos < len) {
        if (!words[widx]) widx = 0;
        int wlen = strlen(words[widx]);
        int tocopy = (pos + wlen > len) ? (len - pos) : wlen;
        memcpy(buf + pos, words[widx], tocopy);
        pos += tocopy;
        widx++;
        if ((rng_byte() & 0x7) == 0) widx = rng_byte() % 15;
    }
}

static int tests_passed = 0;
static int tests_failed = 0;

static void
test_cross(const char *name, char *input, int size)
{
    int out_size = PGLZ_MAX_OUTPUT(size);
    char *stock_comp = malloc(out_size);
    char *variant_comp = malloc(out_size);
    char *decompressed = malloc(size);

    if (!stock_comp || !variant_comp || !decompressed) {
        fprintf(stderr, "OOM\n");
        exit(1);
    }

    /* Compress with stock */
    int32 stock_clen = pglz_compress(input, size, stock_comp,
                                     PGLZ_strategy_always);

    /* Compress with variant */
    int32 variant_clen = pglz_compress_variant(input, size, variant_comp,
                                               PGLZ_strategy_always_variant);

    printf("  %-20s %5d bytes: stock_clen=%d, variant_clen=%d",
           name, size, stock_clen, variant_clen);

    /* Test 1: Variant-compressed → stock decompressor */
    if (variant_clen >= 0) {
        int32 dlen = pglz_decompress(variant_comp, variant_clen,
                                     decompressed, size, true);
        if (dlen != size || memcmp(input, decompressed, size) != 0) {
            printf("  FAIL: variant→stock decompress\n");
            tests_failed++;
            goto cleanup;
        }
    }

    /* Test 2: Stock-compressed → variant decompressor */
    if (stock_clen >= 0) {
        int32 dlen = pglz_decompress_variant(stock_comp, stock_clen,
                                             decompressed, size, true);
        if (dlen != size || memcmp(input, decompressed, size) != 0) {
            printf("  FAIL: stock→variant decompress\n");
            tests_failed++;
            goto cleanup;
        }
    }

    /* Test 3: Variant-compressed → variant decompressor */
    if (variant_clen >= 0) {
        int32 dlen = pglz_decompress_variant(variant_comp, variant_clen,
                                             decompressed, size, true);
        if (dlen != size || memcmp(input, decompressed, size) != 0) {
            printf("  FAIL: variant→variant decompress\n");
            tests_failed++;
            goto cleanup;
        }
    }

    int ratio_diff = 0;
    if (stock_clen > 0 && variant_clen > 0)
        ratio_diff = variant_clen - stock_clen;

    printf("  PASS (ratio diff: %+d bytes)\n", ratio_diff);
    tests_passed++;

cleanup:
    free(stock_comp);
    free(variant_comp);
    free(decompressed);
}

int main(void)
{
    printf("Cross-version decompression test\n");
    printf("================================\n\n");

    static const int sizes[] = { 5, 64, 512, 2048, 4096, 4097, 65536 };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < nsizes; s++) {
        int size = sizes[s];
        char *buf = malloc(size);
        if (!buf) { fprintf(stderr, "OOM\n"); return 1; }

        /* English text */
        gen_english(buf, size);
        test_cross("english", buf, size);

        /* Zeros */
        memset(buf, 0, size);
        test_cross("zeros", buf, size);

        /* All same high byte (UBSan edge case) */
        memset(buf, 0xAA, size);
        test_cross("0xAA-fill", buf, size);

        /* Ascending */
        for (int i = 0; i < size; i++) buf[i] = (char)(i & 0xFF);
        test_cross("ascending", buf, size);

        free(buf);
    }

    printf("\n================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
