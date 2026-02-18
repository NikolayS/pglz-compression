/* ----------
 * pg_lzcompress_fibonacci_hash.c -
 *
 *		AI Alternative #1: Fibonacci Hash Function
 *
 *		Replaces the weak polynomial hash ((s[0]<<6)^(s[1]<<4)^(s[2]<<2)^s[3])
 *		with a Fibonacci/multiplicative hash: (read32(s) * 2654435761u) >> shift
 *
 *		This should dramatically reduce collision rates, shortening chain
 *		traversals and making all other optimizations more effective.
 *
 *		Based on stock PG18dev pg_lzcompress.c — only the hash function changed.
 *
 * Copyright (c) 1999-2026, PostgreSQL Global Development Group
 * ----------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <limits.h>
#include <string.h>

#include "common/pg_lzcompress.h"


/* ----------
 * Local definitions
 * ----------
 */
#define PGLZ_MAX_HISTORY_LISTS	8192	/* must be power of 2 */
#define PGLZ_HISTORY_SIZE		4096
#define PGLZ_MAX_MATCH			273

typedef struct PGLZ_HistEntry
{
	struct PGLZ_HistEntry *next;
	struct PGLZ_HistEntry *prev;
	int			hindex;
	const char *pos;
} PGLZ_HistEntry;

static const PGLZ_Strategy strategy_default_data = {
	32, INT_MAX, 25, 1024, 128, 10
};
const PGLZ_Strategy *const PGLZ_strategy_default = &strategy_default_data;

static const PGLZ_Strategy strategy_always_data = {
	0, INT_MAX, 0, INT_MAX, 128, 6
};
const PGLZ_Strategy *const PGLZ_strategy_always = &strategy_always_data;

static int16 hist_start[PGLZ_MAX_HISTORY_LISTS];
static PGLZ_HistEntry hist_entries[PGLZ_HISTORY_SIZE + 1];

#define INVALID_ENTRY			0
#define INVALID_ENTRY_PTR		(&hist_entries[INVALID_ENTRY])

/* ----------
 * Fibonacci/multiplicative hash — the key change
 *
 * Read 4 bytes as a uint32 (portably via memcpy to avoid alignment UB),
 * multiply by Knuth's golden ratio constant, then shift down.
 * ----------
 */
static inline uint32_t
read32(const char *p)
{
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static inline int
pglz_hist_idx(const char *s, const char *end, int mask)
{
	if ((end - s) < 4)
		return ((unsigned char)s[0]) & mask;

	/*
	 * Fibonacci hash: multiply by golden ratio, take upper bits.
	 * The mask determines how many bits we need. For mask=8191 (13 bits),
	 * we shift by 32-13=19. But since mask is always a power-of-2 minus 1,
	 * we can just AND after the multiply-shift. We use >> 19 for 8192 buckets
	 * but since mask can vary, compute the shift dynamically.
	 *
	 * Actually, for simplicity and because pglz uses mask & result anyway,
	 * we can just multiply and mask. The multiplication spreads the bits
	 * well enough that the low bits are also well-distributed.
	 */
	return (int)((read32(s) * (uint32_t)2654435761U) >> 19) & mask;
}

/* ----------
 * The rest is identical to stock PG code (macros unchanged)
 * ----------
 */
#define pglz_hist_add(_hs,_he,_hn,_recycle,_s,_e, _mask) \
do { \
	int __hindex = pglz_hist_idx((_s),(_e), (_mask)); \
	int16 *__myhsp = &(_hs)[__hindex]; \
	PGLZ_HistEntry *__myhe = &(_he)[_hn]; \
	if (_recycle) { \
		if (__myhe->prev == NULL) \
			(_hs)[__myhe->hindex] = __myhe->next - (_he); \
		else \
			__myhe->prev->next = __myhe->next; \
		if (__myhe->next != NULL) \
			__myhe->next->prev = __myhe->prev; \
	} \
	__myhe->next = &(_he)[*__myhsp]; \
	__myhe->prev = NULL; \
	__myhe->hindex = __hindex; \
	__myhe->pos  = (_s); \
	(_he)[(*__myhsp)].prev = __myhe; \
	*__myhsp = _hn; \
	if (++(_hn) >= PGLZ_HISTORY_SIZE + 1) { \
		(_hn) = 1; \
		(_recycle) = true; \
	} \
} while (0)

#define pglz_out_ctrl(__ctrlp,__ctrlb,__ctrl,__buf) \
do { \
	if ((__ctrl & 0xff) == 0) { \
		*(__ctrlp) = __ctrlb; \
		__ctrlp = (__buf)++; \
		__ctrlb = 0; \
		__ctrl = 1; \
	} \
} while (0)

#define pglz_out_literal(_ctrlp,_ctrlb,_ctrl,_buf,_byte) \
do { \
	pglz_out_ctrl(_ctrlp,_ctrlb,_ctrl,_buf); \
	*(_buf)++ = (unsigned char)(_byte); \
	_ctrl <<= 1; \
} while (0)

#define pglz_out_tag(_ctrlp,_ctrlb,_ctrl,_buf,_len,_off) \
do { \
	pglz_out_ctrl(_ctrlp,_ctrlb,_ctrl,_buf); \
	_ctrlb |= _ctrl; \
	_ctrl <<= 1; \
	if (_len > 17) { \
		(_buf)[0] = (unsigned char)((((_off) & 0xf00) >> 4) | 0x0f); \
		(_buf)[1] = (unsigned char)(((_off) & 0xff)); \
		(_buf)[2] = (unsigned char)((_len) - 18); \
		(_buf) += 3; \
	} else { \
		(_buf)[0] = (unsigned char)((((_off) & 0xf00) >> 4) | ((_len) - 3)); \
		(_buf)[1] = (unsigned char)((_off) & 0xff); \
		(_buf) += 2; \
	} \
} while (0)


static inline int
pglz_find_match(int16 *hstart, const char *input, const char *end,
				int *lenp, int *offp, int good_match, int good_drop, int mask)
{
	PGLZ_HistEntry *hent;
	int16		hentno;
	int32		len = 0;
	int32		off = 0;

	hentno = hstart[pglz_hist_idx(input, end, mask)];
	hent = &hist_entries[hentno];
	while (hent != INVALID_ENTRY_PTR)
	{
		const char *ip = input;
		const char *hp = hent->pos;
		int32		thisoff;
		int32		thislen;

		thisoff = ip - hp;
		if (thisoff >= 0x0fff)
			break;

		thislen = 0;
		if (len >= 16)
		{
			if (memcmp(ip, hp, len) == 0)
			{
				thislen = len;
				ip += len;
				hp += len;
				while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH)
				{
					thislen++;
					ip++;
					hp++;
				}
			}
		}
		else
		{
			while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH)
			{
				thislen++;
				ip++;
				hp++;
			}
		}

		if (thislen > len)
		{
			len = thislen;
			off = thisoff;
		}

		hent = hent->next;

		if (hent != INVALID_ENTRY_PTR)
		{
			if (len >= good_match)
				break;
			good_match -= (good_match * good_drop) / 100;
		}
	}

	if (len > 2)
	{
		*lenp = len;
		*offp = off;
		return 1;
	}

	return 0;
}


int32
pglz_compress(const char *source, int32 slen, char *dest,
			  const PGLZ_Strategy *strategy)
{
	unsigned char *bp = (unsigned char *) dest;
	unsigned char *bstart = bp;
	int			hist_next = 1;
	bool		hist_recycle = false;
	const char *dp = source;
	const char *dend = source + slen;
	unsigned char ctrl_dummy = 0;
	unsigned char *ctrlp = &ctrl_dummy;
	unsigned char ctrlb = 0;
	unsigned char ctrl = 0;
	bool		found_match = false;
	int32		match_len;
	int32		match_off;
	int32		good_match;
	int32		good_drop;
	int32		result_size;
	int32		result_max;
	int32		need_rate;
	int			hashsz;
	int			mask;

	if (strategy == NULL)
		strategy = PGLZ_strategy_default;

	if (strategy->match_size_good <= 0 ||
		slen < strategy->min_input_size ||
		slen > strategy->max_input_size)
		return -1;

	good_match = strategy->match_size_good;
	if (good_match > PGLZ_MAX_MATCH)
		good_match = PGLZ_MAX_MATCH;
	else if (good_match < 17)
		good_match = 17;

	good_drop = strategy->match_size_drop;
	if (good_drop < 0)
		good_drop = 0;
	else if (good_drop > 100)
		good_drop = 100;

	need_rate = strategy->min_comp_rate;
	if (need_rate < 0)
		need_rate = 0;
	else if (need_rate > 99)
		need_rate = 99;

	if (slen > (INT_MAX / 100))
		result_max = (slen / 100) * (100 - need_rate);
	else
		result_max = (slen * (100 - need_rate)) / 100;

	if (slen < 128)
		hashsz = 512;
	else if (slen < 256)
		hashsz = 1024;
	else if (slen < 512)
		hashsz = 2048;
	else if (slen < 1024)
		hashsz = 4096;
	else
		hashsz = 8192;
	mask = hashsz - 1;

	memset(hist_start, 0, hashsz * sizeof(int16));

	while (dp < dend)
	{
		if (bp - bstart >= result_max)
			return -1;

		if (!found_match && bp - bstart >= strategy->first_success_by)
			return -1;

		if (pglz_find_match(hist_start, dp, dend, &match_len,
							&match_off, good_match, good_drop, mask))
		{
			pglz_out_tag(ctrlp, ctrlb, ctrl, bp, match_len, match_off);
			while (match_len--)
			{
				pglz_hist_add(hist_start, hist_entries,
							  hist_next, hist_recycle,
							  dp, dend, mask);
				dp++;
			}
			found_match = true;
		}
		else
		{
			pglz_out_literal(ctrlp, ctrlb, ctrl, bp, *dp);
			pglz_hist_add(hist_start, hist_entries,
						  hist_next, hist_recycle,
						  dp, dend, mask);
			dp++;
		}
	}

	*ctrlp = ctrlb;
	result_size = bp - bstart;
	if (result_size >= result_max)
		return -1;

	return result_size;
}


/* Decompressor — unchanged from stock */
int32
pglz_decompress(const char *source, int32 slen, char *dest,
				int32 rawsize, bool check_complete)
{
	const unsigned char *sp;
	const unsigned char *srcend;
	unsigned char *dp;
	unsigned char *destend;

	sp = (const unsigned char *) source;
	srcend = ((const unsigned char *) source) + slen;
	dp = (unsigned char *) dest;
	destend = dp + rawsize;

	while (sp < srcend && dp < destend)
	{
		unsigned char ctrl = *sp++;
		int ctrlc;

		for (ctrlc = 0; ctrlc < 8 && sp < srcend && dp < destend; ctrlc++)
		{
			if (ctrl & 1)
			{
				int32 len;
				int32 off;

				len = (sp[0] & 0x0f) + 3;
				off = ((sp[0] & 0xf0) << 4) | sp[1];
				sp += 2;
				if (len == 18)
					len += *sp++;

				if (sp > srcend || off == 0 ||
					off > (dp - (unsigned char *) dest))
					return -1;

				len = len < (int32)(destend - dp) ? len : (int32)(destend - dp);

				while (off < len)
				{
					memcpy(dp, dp - off, off);
					len -= off;
					dp += off;
					off += off;
				}
				memcpy(dp, dp - off, len);
				dp += len;
			}
			else
			{
				*dp++ = *sp++;
			}
			ctrl >>= 1;
		}
	}

	if (check_complete && (dp != destend || sp != srcend))
		return -1;

	return (char *) dp - dest;
}

int32
pglz_maximum_compressed_size(int32 rawsize, int32 total_compressed_size)
{
	int64 compressed_size;
	compressed_size = ((int64) rawsize * 9 + 7) / 8;
	compressed_size += 2;
	compressed_size = compressed_size < total_compressed_size ? compressed_size : total_compressed_size;
	return (int32) compressed_size;
}
