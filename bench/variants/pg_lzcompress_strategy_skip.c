/* ----------
 * pg_lzcompress_strategy_skip.c -
 *
 *		Sprint 3: PGLZ_Strategy skip_after_match flag (issue #24).
 *
 *		Builds on step 5 (Fibonacci hash, singly-linked history, memcmp
 *		fast-reject).  Does NOT include SIMD — that is a separate variant.
 *
 *		New field in PGLZ_Strategy:
 *		  bool skip_after_match
 *
 *		When false (default): after emitting a match of length L, advance
 *		  dp one byte at a time, calling hist_add for every byte (identical
 *		  to step5 — BIT-IDENTICAL output).
 *
 *		When true: after emitting a match of length L, advance dp by L
 *		  in one step, calling hist_add only for the first byte of the
 *		  matched region (identical to step6_skipafter — BIT-IDENTICAL
 *		  output for that mode).
 *
 *		The standard strategies (PGLZ_strategy_default, PGLZ_strategy_always)
 *		have skip_after_match=false for full backward compatibility.
 *
 *		Design notes:
 *		  - Adding bool at the end of PGLZ_Strategy is ABI-compatible with
 *		    existing callers that initialise the struct with positional
 *		    initialisers: they simply don't set the new field and get the
 *		    zero-value (false).  Callers that use designated initialisers
 *		    are unaffected.  No existing field offsets change.
 *		  - The flag is checked once per match, not once per byte, so the
 *		    branch predictor handles it with zero overhead on the common
 *		    (false) path.
 *		  - A custom strategy with skip_after_match=true can be constructed
 *		    by extending the initialiser:
 *		        static const PGLZ_Strategy my_fast = {
 *		            32, INT_MAX, 25, 1024, 128, 10, true
 *		        };
 *
 * Copyright (c) 1999-2026, PostgreSQL Global Development Group
 *
 * src/common/pg_lzcompress.c
 * ----------
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <limits.h>

#include "common/pg_lzcompress.h"


/* ----------
 * Local definitions
 * ----------
 */
#define PGLZ_MAX_HISTORY_LISTS	8192	/* must be power of 2 */
#define PGLZ_HISTORY_SIZE		4096
#define PGLZ_MAX_MATCH			273


/*
 * Maximum chain traversal length in pglz_find_match.
 */
#define PGLZ_MAX_CHAIN			256

/* ----------
 * PGLZ_HistEntry — identical to step5.
 * ----------
 */
typedef struct PGLZ_HistEntry
{
	const char *pos;			/* my input position — 8 bytes */
	int16		next;			/* index of next entry in chain, -1 = end */
	uint16		hindex;			/* hash bucket this entry belongs to */
	/* 4 bytes tail padding */
} PGLZ_HistEntry;

#define PGLZ_STATIC_ASSERT(cond, msg) \
	typedef char pglz_static_assert_##msg[(cond) ? 1 : -1]

PGLZ_STATIC_ASSERT(PGLZ_MAX_HISTORY_LISTS <= 65535,
					max_history_lists_fits_uint16);
PGLZ_STATIC_ASSERT(PGLZ_HISTORY_SIZE <= 32767,
					history_size_fits_int16);

#define PGLZ_INVALID_ENTRY		(-1)


/* ----------
 * Standard strategies — skip_after_match=false for backward compatibility.
 *
 * NOTE: These are LOCAL definitions for the standalone bench build.
 * In the PG tree, pg_lzcompress.h declares the externals and
 * pg_lzcompress.c provides the definitions.  Here we define them
 * directly so the file compiles standalone.
 * ----------
 */
static const PGLZ_Strategy strategy_default_data = {
	32,							/* min_input_size */
	INT_MAX,					/* max_input_size */
	25,							/* min_comp_rate */
	1024,						/* first_success_by */
	128,						/* match_size_good */
	10,							/* match_size_drop */
	false						/* skip_after_match — off by default */
};
const PGLZ_Strategy *const PGLZ_strategy_default = &strategy_default_data;


static const PGLZ_Strategy strategy_always_data = {
	0,							/* min_input_size */
	INT_MAX,					/* max_input_size */
	0,							/* min_comp_rate */
	INT_MAX,					/* first_success_by */
	128,						/* match_size_good */
	6,							/* match_size_drop */
	false						/* skip_after_match — off by default */
};
const PGLZ_Strategy *const PGLZ_strategy_always = &strategy_always_data;


/*
 * Convenience strategy with skip_after_match enabled.
 * Useful for benchmarks; not exported as a public symbol.
 */
static const PGLZ_Strategy strategy_skip_data = {
	32,							/* min_input_size */
	INT_MAX,					/* max_input_size */
	25,							/* min_comp_rate */
	1024,						/* first_success_by */
	128,						/* match_size_good */
	10,							/* match_size_drop */
	true						/* skip_after_match — skip enabled */
};
const PGLZ_Strategy *const PGLZ_strategy_skip = &strategy_skip_data;


/* ----------
 * Statically allocated work arrays for history.
 * ----------
 */
static int16 hist_start[PGLZ_MAX_HISTORY_LISTS];
static PGLZ_HistEntry hist_entries[PGLZ_HISTORY_SIZE + 1];


/* ----------
 * pglz_hist_idx — Fibonacci multiply-shift hash (identical to step5).
 * ----------
 */
static inline int
pglz_hist_idx(const char *s, const char *end, int mask)
{
	uint32		h;

	if ((end - s) < 4)
		return ((int) (unsigned char) s[0]) & mask;

	h = ((uint32) (unsigned char) s[0]) |
		((uint32) (unsigned char) s[1] << 8) |
		((uint32) (unsigned char) s[2] << 16) |
		((uint32) (unsigned char) s[3] << 24);
	h *= 2654435761u;

	return (int) (h >> 19) & mask;
}


/* ----------
 * pglz_hist_unlink / pglz_hist_add — identical to step5.
 * ----------
 */
static inline void
pglz_hist_unlink(int16 *hstart, PGLZ_HistEntry *hentries, int16 entry_idx)
{
	PGLZ_HistEntry *entry = &hentries[entry_idx];
	int16	   *pp = &hstart[entry->hindex];

	while (*pp != PGLZ_INVALID_ENTRY)
	{
		if (*pp == entry_idx)
		{
			*pp = entry->next;
			return;
		}
		pp = &hentries[*pp].next;
	}

#ifdef USE_ASSERT_CHECKING
	Assert(false);
#endif
}

static inline void
pglz_hist_add(int16 *hstart, PGLZ_HistEntry *hentries,
			  int *hist_next, bool *recycle,
			  const char *s, const char *end, int mask)
{
	int			hindex = pglz_hist_idx(s, end, mask);
	int16		entry_idx = (int16) *hist_next;
	PGLZ_HistEntry *myhe = &hentries[entry_idx];

	if (*recycle)
		pglz_hist_unlink(hstart, hentries, entry_idx);

	myhe->next = hstart[hindex];
	myhe->hindex = (uint16) hindex;
	myhe->pos = s;
	hstart[hindex] = entry_idx;

	if (++(*hist_next) >= PGLZ_HISTORY_SIZE + 1)
	{
		*hist_next = 0;
		*recycle = true;
	}
}


/* ----------
 * pglz_out_ctrl / pglz_out_literal / pglz_out_tag — identical to step5.
 * ----------
 */
static inline void
pglz_out_ctrl(unsigned char **ctrlp, unsigned char *ctrlb,
			  unsigned char *ctrl, unsigned char **buf)
{
	if ((*ctrl & 0xff) == 0)
	{
		**ctrlp = *ctrlb;
		*ctrlp = (*buf)++;
		*ctrlb = 0;
		*ctrl = 1;
	}
}

static inline void
pglz_out_literal(unsigned char **ctrlp, unsigned char *ctrlb,
				 unsigned char *ctrl, unsigned char **buf, unsigned char byte)
{
	pglz_out_ctrl(ctrlp, ctrlb, ctrl, buf);
	*(*buf)++ = byte;
	*ctrl <<= 1;
}

static inline void
pglz_out_tag(unsigned char **ctrlp, unsigned char *ctrlb,
			 unsigned char *ctrl, unsigned char **buf, int len, int off)
{
	pglz_out_ctrl(ctrlp, ctrlb, ctrl, buf);
	*ctrlb |= *ctrl;
	*ctrl <<= 1;
	if (len > 17)
	{
		(*buf)[0] = (unsigned char)(((off & 0xf00) >> 4) | 0x0f);
		(*buf)[1] = (unsigned char)(off & 0xff);
		(*buf)[2] = (unsigned char)(len - 18);
		(*buf) += 3;
	}
	else
	{
		(*buf)[0] = (unsigned char)(((off & 0xf00) >> 4) | (len - 3));
		(*buf)[1] = (unsigned char)(off & 0xff);
		(*buf) += 2;
	}
}


/* ----------
 * pglz_find_match — identical to step5.
 * ----------
 */
static inline int
pglz_find_match(int16 *hstart, const char *input, const char *end,
				int *lenp, int *offp, int good_match, int good_drop,
				int mask, const char *source)
{
	int16		hentno;
	int32		len = 0;
	int32		off = 0;
	int			chain_len = 0;

	hentno = hstart[pglz_hist_idx(input, end, mask)];
	while (hentno != PGLZ_INVALID_ENTRY)
	{
		PGLZ_HistEntry *hent = &hist_entries[hentno];
		const char *ip = input;
		const char *hp = hent->pos;
		int32		thisoff;
		int32		thislen;

		thisoff = ip - hp;
		if (thisoff >= 0x0fff)
			break;

#ifdef USE_ASSERT_CHECKING
		Assert(hp >= source && hp < ip);
		Assert(hp + 4 <= end);
#endif

		if (memcmp(ip, hp, 4) == 0)
		{
			thislen = 4;
			ip += 4;
			hp += 4;

			if (len >= 16 && thislen < len)
			{
				if (memcmp(ip, hp, len - 4) == 0)
				{
					thislen = len;
					ip = input + len;
					hp = hent->pos + len;
				}
				else
				{
					goto next_entry;
				}
			}

			while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH)
			{
				thislen++;
				ip++;
				hp++;
			}
		}
		else
		{
			goto next_entry;
		}

		if (thislen > len)
		{
			len = thislen;
			off = thisoff;
		}

next_entry:
		hentno = hent->next;

		if (++chain_len >= PGLZ_MAX_CHAIN)
			break;

		if (hentno != PGLZ_INVALID_ENTRY)
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


/* ----------
 * pglz_compress -
 *
 *		Identical to step5, except that when strategy->skip_after_match
 *		is true, after emitting a match of length L we advance dp by L
 *		(calling hist_add only for the first byte) rather than advancing
 *		one byte at a time.
 *
 *		Behaviour:
 *		  skip_after_match=false  →  output identical to step5.
 *		  skip_after_match=true   →  output identical to step6_skipafter.
 * ----------
 */
int32
pglz_compress(const char *source, int32 slen, char *dest,
			  const PGLZ_Strategy *strategy)
{
	unsigned char *bp = (unsigned char *) dest;
	unsigned char *bstart = bp;
	int			hist_next = 0;
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
	bool		skip_after_match;

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

	/* Cache the flag once to help the branch predictor */
	skip_after_match = strategy->skip_after_match;

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

	{
		int		i;

		for (i = 0; i < hashsz; i++)
			hist_start[i] = PGLZ_INVALID_ENTRY;
	}

	while (dp < dend - 3)
	{
		if (bp - bstart >= result_max)
			return -1;

		if (!found_match && bp - bstart >= strategy->first_success_by)
			return -1;

		if (pglz_find_match(hist_start, dp, dend, &match_len,
							&match_off, good_match, good_drop, mask,
							source))
		{
			pglz_out_tag(&ctrlp, &ctrlb, &ctrl, &bp, match_len, match_off);

			if (skip_after_match)
			{
				/*
				 * Skip-after-match: add only the first byte of the match
				 * to the history, then jump dp forward by match_len.
				 * This is the same as step6_skipafter.
				 */
				pglz_hist_add(hist_start, hist_entries,
							  &hist_next, &hist_recycle,
							  dp, dend, mask);
				dp += match_len;
				if (dp > dend)
					dp = dend;
			}
			else
			{
				/*
				 * Standard path: advance dp one byte at a time, calling
				 * hist_add for every matched byte.  Identical to step5.
				 */
				while (match_len--)
				{
					pglz_hist_add(hist_start, hist_entries,
								  &hist_next, &hist_recycle,
								  dp, dend, mask);
					dp++;
				}
			}

			found_match = true;
		}
		else
		{
			pglz_out_literal(&ctrlp, &ctrlb, &ctrl, &bp, *dp);
			pglz_hist_add(hist_start, hist_entries,
						  &hist_next, &hist_recycle,
						  dp, dend, mask);
			dp++;
		}
	}

	while (dp < dend)
	{
		if (bp - bstart >= result_max)
			return -1;

		pglz_out_literal(&ctrlp, &ctrlb, &ctrl, &bp, *dp);
		pglz_hist_add(hist_start, hist_entries,
					  &hist_next, &hist_recycle,
					  dp, dend, mask);
		dp++;
	}

	*ctrlp = ctrlb;
	result_size = bp - bstart;
	if (result_size >= result_max)
		return -1;

	return result_size;
}


/* ----------
 * pglz_decompress — identical to step5.
 * ----------
 */
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
		int			ctrlc;

		for (ctrlc = 0; ctrlc < 8 && sp < srcend && dp < destend; ctrlc++)
		{
			if (ctrl & 1)
			{
				int32		len;
				int32		off;

				len = (sp[0] & 0x0f) + 3;
				off = ((sp[0] & 0xf0) << 4) | sp[1];
				sp += 2;
				if (len == 18)
					len += *sp++;

				if (unlikely(sp > srcend || off == 0 ||
							 off > (dp - (unsigned char *) dest)))
					return -1;

				len = Min(len, destend - dp);

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


/* ----------
 * pglz_maximum_compressed_size — unchanged.
 * ----------
 */
int32
pglz_maximum_compressed_size(int32 rawsize, int32 total_compressed_size)
{
	int64		compressed_size;

	compressed_size = ((int64) rawsize * 9 + 7) / 8;
	compressed_size += 2;
	compressed_size = Min(compressed_size, total_compressed_size);

	return (int32) compressed_size;
}
