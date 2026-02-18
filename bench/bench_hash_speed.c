/*
 * bench_hash_speed.c — Microbenchmark of hash function speed.
 */
#define FRONTEND 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static inline int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Stock PG hash */
static inline int
stock_hash(const char *s, int mask)
{
    return (((unsigned char)s[0] << 6) ^ ((unsigned char)s[1] << 4) ^
            ((unsigned char)s[2] << 2) ^ (unsigned char)s[3]) & mask;
}

/* Fibonacci hash */
static inline uint32_t read32(const char *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

static inline int
fib_hash(const char *s, int mask)
{
    return (int)((read32(s) * (uint32_t)2654435761U) >> 19) & mask;
}

int main(void)
{
    int mask = 8191;
    int size = 65536;
    char *data = malloc(size);

    /* English-like data */
    const char *words = "the quick brown fox jumps over the lazy dog and then "
                        "PostgreSQL database compression algorithm data table ";
    int wlen = strlen(words);
    for (int i = 0; i < size; i++)
        data[i] = words[i % wlen];

    int iters = 10000000;
    volatile int sink = 0;

    /* Benchmark stock hash */
    int64_t t0 = now_ns();
    for (int i = 0; i < iters; i++) {
        int pos = i % (size - 4);
        sink += stock_hash(data + pos, mask);
    }
    int64_t t1 = now_ns();
    double stock_ns = (double)(t1 - t0) / iters;

    /* Benchmark Fibonacci hash */
    int64_t t2 = now_ns();
    for (int i = 0; i < iters; i++) {
        int pos = i % (size - 4);
        sink += fib_hash(data + pos, mask);
    }
    int64_t t3 = now_ns();
    double fib_ns = (double)(t3 - t2) / iters;

    printf("Hash function microbenchmark (%d iterations)\n", iters);
    printf("  stock_hash:     %.2f ns/call\n", stock_ns);
    printf("  fibonacci_hash: %.2f ns/call\n", fib_ns);
    printf("  ratio: %.2f×\n", stock_ns / fib_ns);
    printf("  sink=%d (prevent optimization)\n", sink);

    free(data);
    return 0;
}
