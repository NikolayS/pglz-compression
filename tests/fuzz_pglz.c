/*
 * fuzz_pglz.c — libFuzzer target for pglz round-trip verification.
 *
 * Tests that compress → decompress round-trips correctly for arbitrary
 * inputs. Uses malloc (not VLAs!) with a 1 MiB cap per SPEC.md.
 *
 * Compile (clang with ASan + UBSan + libFuzzer):
 *   clang -g -O2 -DFRONTEND \
 *     -fsanitize=address,undefined,fuzzer \
 *     -I../bench/include \
 *     -o fuzz_pglz fuzz_pglz.c ../bench/pg_lzcompress_baseline.c
 *
 * Run:
 *   ./fuzz_pglz -max_len=1048576 -runs=10000000
 */

#define FRONTEND 1

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "common/pg_lzcompress.h"

/* 1 MiB cap — pglz inputs are bounded; prevents OOM in fuzzer */
#define MAX_INPUT_SIZE (1024 * 1024)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > MAX_INPUT_SIZE)
        return 0;

    int32 output_size = PGLZ_MAX_OUTPUT((int32)size);
    char *compressed = malloc(output_size);
    char *decompressed = malloc(size);

    if (!compressed || !decompressed)
    {
        free(compressed);
        free(decompressed);
        return 0;
    }

    /* Test with PGLZ_strategy_always — always enters the hot loop */
    int32 clen = pglz_compress((const char *)data, (int32)size,
                               compressed, PGLZ_strategy_always);
    if (clen >= 0)
    {
        int32 dlen = pglz_decompress(compressed, clen,
                                     decompressed, (int32)size, true);
        assert(dlen == (int32)size);
        assert(memcmp(data, decompressed, size) == 0);
    }

    /* Also test with PGLZ_strategy_default for coverage of early-bailout path */
    int32 clen2 = pglz_compress((const char *)data, (int32)size,
                                compressed, PGLZ_strategy_default);
    if (clen2 >= 0)
    {
        int32 dlen2 = pglz_decompress(compressed, clen2,
                                      decompressed, (int32)size, true);
        assert(dlen2 == (int32)size);
        assert(memcmp(data, decompressed, size) == 0);
    }

    free(compressed);
    free(decompressed);
    return 0;
}
