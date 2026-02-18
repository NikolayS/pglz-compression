/*
 * bench_hash_speed2.c — Extended hash microbenchmark including CRC32C.
 */
#define FRONTEND 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif

static inline int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline uint32_t read32(const char *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

/* Stock PG hash */
static inline int stock_hash(const char *s, int mask) {
    return (((unsigned char)s[0] << 6) ^ ((unsigned char)s[1] << 4) ^
            ((unsigned char)s[2] << 2) ^ (unsigned char)s[3]) & mask;
}

/* Fibonacci hash */
static inline int fib_hash(const char *s, int mask) {
    return (int)((read32(s) * (uint32_t)2654435761U) >> 19) & mask;
}

/* XOR-fold hash (simpler than stock, no dependency chain) */
static inline int xor_fold_hash(const char *s, int mask) {
    uint32_t v = read32(s);
    v ^= v >> 16;
    v ^= v >> 8;
    return (int)(v & mask);
}

/* CRC32C hardware hash */
#ifdef __SSE4_2__
static inline int crc32c_hash(const char *s, int mask) {
    return (int)(_mm_crc32_u32(0, read32(s)) & mask);
}
#endif

/* Multiply-and-add hash (different constant) */
static inline int murmur_like_hash(const char *s, int mask) {
    uint32_t h = read32(s);
    h *= 0xcc9e2d51;
    h ^= h >> 16;
    return (int)(h & mask);
}

int main(void)
{
    int mask = 8191;
    int size = 65536;
    char *data = malloc(size);

    const char *words = "the quick brown fox jumps over the lazy dog and then "
                        "PostgreSQL database compression algorithm data table ";
    int wlen = strlen(words);
    for (int i = 0; i < size; i++)
        data[i] = words[i % wlen];

    int iters = 50000000;
    volatile int sink = 0;

    struct { const char *name; int (*fn)(const char *, int); } tests[] = {
        { "stock (4 loads+shifts+xors)", stock_hash },
        { "fibonacci (mul+shift)", fib_hash },
        { "xor-fold (load+2 shifts+2 xors)", xor_fold_hash },
        { "murmur-like (mul+shift+xor)", murmur_like_hash },
#ifdef __SSE4_2__
        { "crc32c (hardware)", crc32c_hash },
#endif
    };
    int ntests = sizeof(tests) / sizeof(tests[0]);

    printf("Hash function speed comparison (%d M iterations)\n\n", iters/1000000);
    printf("%-40s %10s\n", "Hash Function", "ns/call");
    printf("%-40s %10s\n", "----", "----");

    for (int t = 0; t < ntests; t++) {
        int64_t t0 = now_ns();
        for (int i = 0; i < iters; i++) {
            int pos = i % (size - 4);
            sink += tests[t].fn(data + pos, mask);
        }
        int64_t t1 = now_ns();
        printf("%-40s %10.2f\n", tests[t].name, (double)(t1 - t0) / iters);
    }

    printf("\nsink=%d\n", sink);
    free(data);
    return 0;
}
