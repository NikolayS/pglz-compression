/*
 * bench_skip_threshold.c — Benchmark skip-after-match at various thresholds.
 *
 * Tests PGLZ_SKIP_THRESHOLD at 4, 8, 16, 32, 64, and "never skip" to find
 * the optimal speed/ratio tradeoff.
 *
 * Compile:
 *   clang -O2 -DFRONTEND -I./include -o bench_skip bench_skip_threshold.c
 */

#define FRONTEND 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <time.h>

/* We embed a modified pglz inline so we can change the threshold at runtime */

#define PGLZ_MAX_HISTORY_LISTS	8192
#define PGLZ_HISTORY_SIZE		4096
#define PGLZ_MAX_MATCH			273

#include "common/pg_lzcompress.h"

typedef struct PGLZ_HistEntry
{
	struct PGLZ_HistEntry *next;
	struct PGLZ_HistEntry *prev;
	int			hindex;
	const char *pos;
} PGLZ_HistEntry;

static const PGLZ_Strategy strategy_always_data = {
	0, INT_MAX, 0, INT_MAX, 128, 6
};

static int16 hist_start[PGLZ_MAX_HISTORY_LISTS];
static PGLZ_HistEntry hist_entries[PGLZ_HISTORY_SIZE + 1];

#define INVALID_ENTRY			0
#define INVALID_ENTRY_PTR		(&hist_entries[INVALID_ENTRY])

/* Fibonacci hash */
static inline uint32_t read32(const char *p) {
	uint32_t v; memcpy(&v, p, 4); return v;
}

static inline int
hist_idx(const char *s, const char *end, int mask)
{
	if ((end - s) < 4) return ((unsigned char)s[0]) & mask;
	return (int)((read32(s) * (uint32_t)2654435761U) >> 19) & mask;
}

#define hist_add(_hs,_he,_hn,_recycle,_s,_e,_mask) \
do { \
	int __hi = hist_idx((_s),(_e),(_mask)); \
	int16 *__mhsp = &(_hs)[__hi]; \
	PGLZ_HistEntry *__mhe = &(_he)[_hn]; \
	if (_recycle) { \
		if (__mhe->prev == NULL) (_hs)[__mhe->hindex] = __mhe->next - (_he); \
		else __mhe->prev->next = __mhe->next; \
		if (__mhe->next != NULL) __mhe->next->prev = __mhe->prev; \
	} \
	__mhe->next = &(_he)[*__mhsp]; \
	__mhe->prev = NULL; \
	__mhe->hindex = __hi; \
	__mhe->pos = (_s); \
	(_he)[(*__mhsp)].prev = __mhe; \
	*__mhsp = _hn; \
	if (++(_hn) >= PGLZ_HISTORY_SIZE + 1) { (_hn) = 1; (_recycle) = true; } \
} while(0)

/* 4-byte memcmp match with chain limit */
static inline int
find_match(int16 *hstart, const char *input, const char *end,
           int *lenp, int *offp, int good_match, int good_drop, int mask)
{
	int16 hentno;
	PGLZ_HistEntry *hent;
	int32 len = 0, off = 0;
	int chain = 0;

	hentno = hstart[hist_idx(input, end, mask)];
	hent = &hist_entries[hentno];
	while (hent != INVALID_ENTRY_PTR) {
		const char *ip = input, *hp = hent->pos;
		int32 thisoff = ip - hp, thislen;
		if (thisoff >= 0x0fff) break;

		if ((end - ip) >= 4) {
			if (memcmp(ip, hp, 4) != 0) goto next;
			thislen = 4; ip += 4; hp += 4;
			while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH) {
				thislen++; ip++; hp++;
			}
		} else {
			thislen = 0;
			while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH) {
				thislen++; ip++; hp++;
			}
		}
		if (thislen > len) { len = thislen; off = thisoff; }
next:
		hent = hent->next;
		if (++chain >= 256) break;
		if (hent != INVALID_ENTRY_PTR) {
			if (len >= good_match) break;
			good_match -= (good_match * good_drop) / 100;
		}
	}
	if (len > 2) { *lenp = len; *offp = off; return 1; }
	return 0;
}

static int32
compress_with_skip(const char *source, int32 slen, char *dest, int skip_threshold)
{
	unsigned char *bp = (unsigned char *)dest;
	unsigned char *bstart = bp;
	int hn = 1;
	bool recycle = false;
	const char *dp = source, *dend = source + slen;
	unsigned char ctrl_dummy = 0, *ctrlp = &ctrl_dummy;
	unsigned char ctrlb = 0, ctrl = 0;
	int32 ml, mo;
	int mask = 8191;

	if (slen < 1) return -1;

	memset(hist_start, 0, 8192 * sizeof(int16));

	while (dp < dend) {
		if (find_match(hist_start, dp, dend, &ml, &mo, 128, 6, mask)) {
			/* emit tag */
			if ((ctrl & 0xff) == 0) { *ctrlp = ctrlb; ctrlp = bp++; ctrlb = 0; ctrl = 1; }
			ctrlb |= ctrl; ctrl <<= 1;
			if (ml > 17) {
				bp[0] = (unsigned char)(((mo & 0xf00) >> 4) | 0x0f);
				bp[1] = (unsigned char)(mo & 0xff);
				bp[2] = (unsigned char)(ml - 18);
				bp += 3;
			} else {
				bp[0] = (unsigned char)(((mo & 0xf00) >> 4) | (ml - 3));
				bp[1] = (unsigned char)(mo & 0xff);
				bp += 2;
			}

			if (skip_threshold > 0 && ml >= skip_threshold) {
				int added = 0;
				while (added < 4 && ml > 0) {
					hist_add(hist_start, hist_entries, hn, recycle, dp, dend, mask);
					dp++; ml--; added++;
				}
				if (ml > 4) { dp += ml - 4; ml = 4; }
				while (ml > 0) {
					hist_add(hist_start, hist_entries, hn, recycle, dp, dend, mask);
					dp++; ml--;
				}
			} else {
				while (ml--) {
					hist_add(hist_start, hist_entries, hn, recycle, dp, dend, mask);
					dp++;
				}
			}
		} else {
			if ((ctrl & 0xff) == 0) { *ctrlp = ctrlb; ctrlp = bp++; ctrlb = 0; ctrl = 1; }
			*bp++ = (unsigned char)*dp; ctrl <<= 1;
			hist_add(hist_start, hist_entries, hn, recycle, dp, dend, mask);
			dp++;
		}
	}
	*ctrlp = ctrlb;
	return (int32)(bp - bstart);
}

/* Timing */
static inline int64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* English text generator */
static uint64_t rng_state = 42;
static uint8_t rng_byte(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint8_t)(rng_state & 0xFF);
}

static void gen_english(char *buf, int len) {
	const char *words[] = {
		"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ",
		"and ", "then ", "runs ", "away ", "from ", "here ", "to ", "there ",
		"with ", "some ", "data ", "that ", "is ", "quite ", "compressible ",
		"PostgreSQL ", "is ", "an ", "advanced ", "open ", "source ",
		"relational ", "database ", "system ", NULL
	};
	rng_state = 42;
	int pos = 0, widx = 0;
	while (pos < len) {
		if (!words[widx]) widx = 0;
		int wlen = strlen(words[widx]);
		int tc = (pos + wlen > len) ? (len - pos) : wlen;
		memcpy(buf + pos, words[widx], tc);
		pos += tc; widx++;
		if ((rng_byte() & 0x7) == 0) widx = rng_byte() % 25;
	}
}

int main(void)
{
	int thresholds[] = { 0, 4, 8, 12, 16, 32, 64, 128 };
	int sizes[] = { 2048, 4096, 65536 };
	int nthresh = sizeof(thresholds)/sizeof(thresholds[0]);
	int nsizes = sizeof(sizes)/sizeof(sizes[0]);

	printf("Skip-after-match threshold sweep (Fibonacci hash + 4-byte memcmp)\n");
	printf("==================================================================\n\n");
	printf("%-10s %8s %10s %10s %10s %12s\n",
		   "Threshold", "Size", "CSize", "Ratio", "MiB/s", "Median(µs)");
	printf("%-10s %8s %10s %10s %10s %12s\n",
		   "---------", "----", "-----", "-----", "-----", "----------");

	for (int s = 0; s < nsizes; s++) {
		int size = sizes[s];
		char *input = malloc(size);
		char *output = malloc(PGLZ_MAX_OUTPUT(size));
		gen_english(input, size);

		for (int t = 0; t < nthresh; t++) {
			int thresh = thresholds[t];
			const char *label = (thresh == 0) ? "never" : "";

			/* Get compressed size */
			int32 clen = compress_with_skip(input, size, output, thresh);

			/* Benchmark */
			int iters = 0;
			int64_t total_ns = 0;
			int64_t *lats = malloc(100000 * sizeof(int64_t));

			/* Warmup */
			for (int i = 0; i < 100; i++)
				compress_with_skip(input, size, output, thresh);

			while (iters < 100000 && total_ns < 500000000LL) {
				int64_t t0 = now_ns();
				compress_with_skip(input, size, output, thresh);
				int64_t t1 = now_ns();
				lats[iters] = t1 - t0;
				total_ns += t1 - t0;
				iters++;
			}

			/* Find median (simple: just use middle) */
			/* Sort for median */
			for (int i = 0; i < iters - 1; i++)
				for (int j = i + 1; j < iters && j < i + 20; j++)
					if (lats[j] < lats[i]) { int64_t tmp = lats[i]; lats[i] = lats[j]; lats[j] = tmp; }

			double median_us = lats[iters/2] / 1000.0;
			double mib_s = ((double)size * iters / (1024.0*1024.0)) / (total_ns / 1e9);

			if (thresh == 0)
				printf("%-10s %7dK %10d %9.2f%% %10.1f %12.2f\n",
					   "never", size/1024, clen,
					   clen > 0 ? (double)clen/size*100.0 : -1.0,
					   mib_s, median_us);
			else
				printf("%-10d %7dK %10d %9.2f%% %10.1f %12.2f\n",
					   thresh, size/1024, clen,
					   clen > 0 ? (double)clen/size*100.0 : -1.0,
					   mib_s, median_us);

			free(lats);
		}
		printf("\n");
		free(input);
		free(output);
	}

	return 0;
}
