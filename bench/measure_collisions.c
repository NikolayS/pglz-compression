/*
 * measure_collisions.c — Measure hash collision rates for stock vs Fibonacci hash.
 *
 * Counts chain lengths to quantify hash quality on real-world data patterns.
 */
#define FRONTEND 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PGLZ_MAX_HISTORY_LISTS 8192

/* Stock PG hash */
static inline int
stock_hash(const char *s, const char *end, int mask)
{
    if ((end - s) < 4)
        return ((unsigned char)s[0]) & mask;
    return (((unsigned char)s[0] << 6) ^ ((unsigned char)s[1] << 4) ^
            ((unsigned char)s[2] << 2) ^ (unsigned char)s[3]) & mask;
}

/* Fibonacci hash */
static inline uint32_t read32(const char *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

static inline int
fib_hash(const char *s, const char *end, int mask)
{
    if ((end - s) < 4)
        return ((unsigned char)s[0]) & mask;
    return (int)((read32(s) * (uint32_t)2654435761U) >> 19) & mask;
}

typedef int (*hash_fn)(const char *, const char *, int);

static void
measure(const char *name, const char *data, int len, hash_fn fn, int mask)
{
    int buckets[PGLZ_MAX_HISTORY_LISTS];
    memset(buckets, 0, sizeof(buckets));
    int hashsz = mask + 1;

    int positions = len - 3; /* need 4 bytes for hash */
    if (positions <= 0) return;

    for (int i = 0; i < positions; i++)
        buckets[fn(data + i, data + len, mask)]++;

    /* Compute statistics */
    int max_chain = 0;
    int empty = 0;
    long long sum_sq = 0;
    double sum = 0;

    for (int i = 0; i < hashsz; i++) {
        if (buckets[i] > max_chain) max_chain = buckets[i];
        if (buckets[i] == 0) empty++;
        sum += buckets[i];
        sum_sq += (long long)buckets[i] * buckets[i];
    }

    double avg = sum / hashsz;
    double variance = (double)sum_sq / hashsz - avg * avg;

    /* Count how many positions land in buckets with >4 entries */
    int collided = 0;
    for (int i = 0; i < hashsz; i++)
        if (buckets[i] > 4) collided += buckets[i];

    printf("  %-15s: avg=%.2f, max=%d, empty=%d/%d (%.1f%%), var=%.2f, "
           ">4 entries: %d (%.1f%%)\n",
           name, avg, max_chain, empty, hashsz,
           (double)empty/hashsz*100.0, variance,
           collided, (double)collided/positions*100.0);
}

/* Data generators */
static void gen_english(char *buf, int len) {
    const char *words[] = {
        "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ",
        "and ", "then ", "runs ", "away ", "from ", "here ", "to ", "there ",
        "with ", "some ", "data ", "that ", "is ", "quite ", "compressible ",
        "PostgreSQL ", "is ", "an ", "advanced ", "open ", "source ", NULL
    };
    int pos = 0, widx = 0;
    while (pos < len) {
        if (!words[widx]) widx = 0;
        int wlen = strlen(words[widx]);
        int tc = (pos + wlen > len) ? (len - pos) : wlen;
        memcpy(buf + pos, words[widx], tc);
        pos += tc; widx++;
    }
}

static void gen_json(char *buf, int len) {
    const char *template = "{\"id\":%d,\"name\":\"user_%d\",\"email\":\"user%d@example.com\","
                           "\"score\":%d,\"active\":true,\"tags\":[\"pg\",\"db\"]}";
    int pos = 0, id = 1;
    while (pos < len) {
        char row[256];
        int rlen = snprintf(row, sizeof(row), template, id, id, id, id * 17 % 100);
        int tc = (pos + rlen > len) ? (len - pos) : rlen;
        memcpy(buf + pos, row, tc);
        pos += tc; id++;
    }
}

int main(void)
{
    int size = 65536;
    int mask = PGLZ_MAX_HISTORY_LISTS - 1;
    char *buf = malloc(size);

    printf("Hash collision analysis (input size: %d, hash table: %d buckets)\n\n",
           size, PGLZ_MAX_HISTORY_LISTS);

    /* English text */
    gen_english(buf, size);
    printf("English text:\n");
    measure("stock_hash", buf, size, stock_hash, mask);
    measure("fibonacci_hash", buf, size, fib_hash, mask);

    /* JSON */
    gen_json(buf, size);
    printf("\nJSON data:\n");
    measure("stock_hash", buf, size, stock_hash, mask);
    measure("fibonacci_hash", buf, size, fib_hash, mask);

    /* All zeros */
    memset(buf, 0, size);
    printf("\nAll zeros:\n");
    measure("stock_hash", buf, size, stock_hash, mask);
    measure("fibonacci_hash", buf, size, fib_hash, mask);

    /* Ascending bytes */
    for (int i = 0; i < size; i++) buf[i] = (char)(i & 0xFF);
    printf("\nAscending bytes:\n");
    measure("stock_hash", buf, size, stock_hash, mask);
    measure("fibonacci_hash", buf, size, fib_hash, mask);

    /* pgbench-like */
    int pos = 0;
    int aid = 1;
    while (pos < size) {
        char row[128];
        int rlen = snprintf(row, sizeof(row), "%d|%d|%d|%84s\n",
                           aid, (aid-1)/100000+1, aid*3%200001-100000, "");
        int tc = (pos + rlen > size) ? (size - pos) : rlen;
        memcpy(buf + pos, row, tc);
        pos += tc; aid++;
    }
    printf("\npgbench rows:\n");
    measure("stock_hash", buf, size, stock_hash, mask);
    measure("fibonacci_hash", buf, size, fib_hash, mask);

    free(buf);
    return 0;
}
