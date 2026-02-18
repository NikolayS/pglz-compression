/* ----------
 * pg_lzcompress_skip_after_match.c -
 *
 *		AI Alternative #2: Skip History Adds After Long Matches
 *
 *		After emitting a match of N bytes, the stock code adds all N bytes
 *		to the history table. This is wasteful for long matches — the next
 *		useful match is likely near the end of the current match.
 *
 *		This variant skips intermediate history additions when match >= 8.
 *		Only adds first 4 bytes and last 4 bytes to history.
 *		This is what LZ4/zstd do for speed.
 *
 *		Based on stock PG18dev pg_lzcompress.c
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


#define PGLZ_MAX_HISTORY_LISTS	8192
#define PGLZ_HISTORY_SIZE		4096
#define PGLZ_MAX_MATCH			273

/* Matches shorter than this: add all bytes to history (preserve ratio) */
#define PGLZ_SKIP_THRESHOLD		8

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

#define pglz_hist_idx(_s,_e, _mask) ( \
	((((_e) - (_s)) < 4) ? (int)(unsigned char)(_s)[0] : \
	 (((unsigned char)(_s)[0] << 6) ^ ((unsigned char)(_s)[1] << 4) ^ \
	  ((unsigned char)(_s)[2] << 2) ^ (unsigned char)(_s)[3])) & (_mask) \
)

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

			/*
			 * KEY CHANGE: For long matches, skip intermediate history adds.
			 * Only add the first 4 bytes and last 4 bytes to history.
			 * For matches < PGLZ_SKIP_THRESHOLD, add all (preserve ratio).
			 */
			if (match_len >= PGLZ_SKIP_THRESHOLD)
			{
				/* Add first 4 bytes */
				int added = 0;
				while (added < 4 && match_len > 0)
				{
					pglz_hist_add(hist_start, hist_entries,
								  hist_next, hist_recycle,
								  dp, dend, mask);
					dp++;
					match_len--;
					added++;
				}

				/* Skip middle bytes (advance dp but don't add to history) */
				if (match_len > 4)
				{
					dp += match_len - 4;
					match_len = 4;
				}

				/* Add last bytes */
				while (match_len > 0)
				{
					pglz_hist_add(hist_start, hist_entries,
								  hist_next, hist_recycle,
								  dp, dend, mask);
					dp++;
					match_len--;
				}
			}
			else
			{
				/* Short match: add all bytes as before */
				while (match_len--)
				{
					pglz_hist_add(hist_start, hist_entries,
								  hist_next, hist_recycle,
								  dp, dend, mask);
					dp++;
				}
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
