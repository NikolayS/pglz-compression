/* ----------
 * pg_lzcompress_step6_skipafter.c -
 *
 *		Step 6: Skip-after-match optimization.
 *
 *		After emitting a match tag of length L, advance dp by L without
 *		calling hist_add for intermediate positions. This eliminates the
 *		O(match_len) hist_add loop that dominated CPU time on compressible
 *		data, giving 2-10x speedup at a cost of ~1-3pp compression ratio.
 *
 *		Builds on step 5 (Fibonacci multiply-shift hash).
 *
 *		Tradeoff: NOT suitable when compression ratio is critical.
 *		Suitable for: logs, SQL dumps, JSON, time-series with repetition.
 *
 *		Original pg_lzcompress.c header:
 *		This is an implementation of LZ compression for PostgreSQL.
 *		It uses a simple history table and generates 2-3 byte tags
 *		capable of backward copy information for 3-273 bytes with
 *		a max offset of 4095.
 *
 *		Entry routines:
 *
 *			int32
 *			pglz_compress(const char *source, int32 slen, char *dest,
 *						  const PGLZ_Strategy *strategy);
 *
 *				source is the input data to be compressed.
 *
 *				slen is the length of the input data.
 *
 *				dest is the output area for the compressed result.
 *					It must be at least as big as PGLZ_MAX_OUTPUT(slen).
 *
 *				strategy is a pointer to some information controlling
 *					the compression algorithm. If NULL, the compiled
 *					in default strategy is used.
 *
 *				The return value is the number of bytes written in the
 *				buffer dest, or -1 if compression fails; in the latter
 *				case the contents of dest are undefined.
 *
 *			int32
 *			pglz_decompress(const char *source, int32 slen, char *dest,
 *							int32 rawsize, bool check_complete)
 *
 *				source is the compressed input.
 *
 *				slen is the length of the compressed input.
 *
 *				dest is the area where the uncompressed data will be
 *					written to. It is the callers responsibility to
 *					provide enough space.
 *
 *					The data is written to buff exactly as it was handed
 *					to pglz_compress(). No terminating zero byte is added.
 *
 *				rawsize is the length of the uncompressed data.
 *
 *				check_complete is a flag to let us know if -1 should be
 *					returned in cases where we don't reach the end of the
 *					source or dest buffers, or not.  This should be false
 *					if the caller is asking for only a partial result and
 *					true otherwise.
 *
 *				The return value is the number of bytes written in the
 *				buffer dest, or -1 if decompression fails.
 *
 *		The decompression algorithm and internal data format:
 *
 *			It is made with the compressed data itself.
 *
 *			The data representation is easiest explained by describing
 *			the process of decompression.
 *
 *			If compressed_size == rawsize, then the data
 *			is stored uncompressed as plain bytes. Thus, the decompressor
 *			simply copies rawsize bytes to the destination.
 *
 *			Otherwise the first byte tells what to do the next 8 times.
 *			We call this the control byte.
 *
 *			An unset bit in the control byte means, that one uncompressed
 *			byte follows, which is copied from input to output.
 *
 *			A set bit in the control byte means, that a tag of 2-3 bytes
 *			follows. A tag contains information to copy some bytes, that
 *			are already in the output buffer, to the current location in
 *			the output. Let's call the three tag bytes T1, T2 and T3. The
 *			position of the data to copy is coded as an offset from the
 *			actual output position.
 *
 *			The offset is in the upper nibble of T1 and in T2.
 *			The length is in the lower nibble of T1.
 *
 *			So the 16 bits of a 2 byte tag are coded as
 *
 *				7---T1--0  7---T2--0
 *				OOOO LLLL  OOOO OOOO
 *
 *			This limits the offset to 1-4095 (12 bits) and the length
 *			to 3-18 (4 bits) because 3 is always added to it. To emit
 *			a tag of 2 bytes with a length of 2 only saves one control
 *			bit. But we lose one byte in the possible length of a tag.
 *
 *			In the actual implementation, the 2 byte tag's length is
 *			limited to 3-17, because the value 0xF in the length nibble
 *			has special meaning. It means, that the next following
 *			byte (T3) has to be added to the length value of 18. That
 *			makes total limits of 1-4095 for offset and 3-273 for length.
 *
 *			Now that we have successfully decoded a tag. We simply copy
 *			the output that occurred <offset> bytes back to the current
 *			output location in the specified <length>. Thus, a
 *			sequence of 200 spaces (think about bpchar fields) could be
 *			coded in 4 bytes. One literal space and a three byte tag to
 *			copy 199 bytes with a -1 offset. Whow - that's a compression
 *			rate of 98%! Well, the implementation needs to save the
 *			original data size too, so we need another 4 bytes for it
 *			and end up with a total compression rate of 96%, what's still
 *			worth a Whow.
 *
 *		The compression algorithm
 *
 *			The following uses numbers used in the default strategy.
 *
 *			The compressor works best for attributes of a size between
 *			1K and 1M. For smaller items there's not that much chance of
 *			redundancy in the character sequence (except for large areas
 *			of identical bytes like trailing spaces) and for bigger ones
 *			our 4K maximum look-back distance is too small.
 *
 *			The compressor creates a table for lists of positions.
 *			For each input position (except the last 3), a hash key is
 *			built from the 4 next input bytes and the position remembered
 *			in the appropriate list. Thus, the table points to linked
 *			lists of likely to be at least in the first 4 characters
 *			matching strings. This is done on the fly while the input
 *			is compressed into the output area.  Table entries are only
 *			kept for the last 4096 input positions, since we cannot use
 *			back-pointers larger than that anyway.  The size of the hash
 *			table is chosen based on the size of the input - a larger table
 *			has a larger startup cost, as it needs to be initialized to
 *			zero, but reduces the number of hash collisions on long inputs.
 *
 *			For each byte in the input, its hash key (built from this
 *			byte and the next 3) is used to find the appropriate list
 *			in the table. The lists remember the positions of all bytes
 *			that had the same hash key in the past in increasing backward
 *			offset order. Now for all entries in the used lists, the
 *			match length is computed by comparing the characters from the
 *			entries position with the characters from the actual input
 *			position.
 *
 *			The compressor starts with a so called "good_match" of 128.
 *			It is a "prefer speed against compression ratio" optimizer.
 *			So if the first entry looked at already has 128 or more
 *			matching characters, the lookup stops and that position is
 *			used for the next tag in the output.
 *
 *			For each subsequent entry in the history list, the "good_match"
 *			is lowered by 10%. So the compressor will be more happy with
 *			short matches the further it has to go back in the history.
 *			Another "speed against ratio" preference characteristic of
 *			the algorithm.
 *
 *			Thus there are 3 stop conditions for the lookup of matches:
 *
 *				- a match >= good_match is found
 *				- there are no more history entries to look at
 *				- the next history entry is already too far back
 *				  to be coded into a tag.
 *
 *			Finally the match algorithm checks that at least a match
 *			of 3 or more bytes has been found, because that is the smallest
 *			amount of copy information to code into a tag. If so, a tag
 *			is omitted and all the input bytes covered by that are just
 *			scanned for the history add's, otherwise a literal character
 *			is omitted and only his history entry added.
 *
 *		Acknowledgments:
 *
 *			Many thanks to Adisak Pochanayon, who's article about SLZ
 *			inspired me to write the PostgreSQL compression this way.
 *
 *			Jan Wieck
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
 * Maximum chain traversal length in pglz_find_match.  Defense-in-depth
 * against pathological hash collisions — bounds worst-case match-finding
 * to O(PGLZ_MAX_CHAIN) per input byte.  LZ4 uses a similar technique.
 */
#define PGLZ_MAX_CHAIN			256

/* ----------
 * PGLZ_HistEntry -
 *
 *		Singly-linked list for the backward history lookup
 *
 * Each entry lives in exactly one hash bucket chain at a time.  When an
 * entry is recycled (ring buffer wraps), it is unlinked from its old
 * chain via predecessor scan before being inserted into the new chain.
 *
 * Using int16 indexes instead of pointers, and removing the prev pointer,
 * keeps each entry at 16 bytes on 64-bit platforms (pos 8B + next 2B +
 * hindex 2B + 4B padding).
 *
 * The sentinel value -1 means "no entry" (end of chain or empty bucket).
 * Indexes 0..PGLZ_HISTORY_SIZE map directly to hist_entries[0..N].
 * ----------
 */
typedef struct PGLZ_HistEntry
{
	const char *pos;			/* my input position — 8 bytes */
	int16		next;			/* index of next entry in chain, -1 = end */
	uint16		hindex;			/* hash bucket this entry belongs to */
	/* 4 bytes tail padding to align struct to 8 */
} PGLZ_HistEntry;

/* Compile-time size checks */
#define PGLZ_STATIC_ASSERT(cond, msg) \
	typedef char pglz_static_assert_##msg[(cond) ? 1 : -1]

PGLZ_STATIC_ASSERT(PGLZ_MAX_HISTORY_LISTS <= 65535,
					max_history_lists_fits_uint16);
PGLZ_STATIC_ASSERT(PGLZ_HISTORY_SIZE <= 32767,
					history_size_fits_int16);

/* Sentinel value for empty chain entries */
#define PGLZ_INVALID_ENTRY		(-1)


/* ----------
 * The provided standard strategies
 * ----------
 */
static const PGLZ_Strategy strategy_default_data = {
	32,							/* Data chunks less than 32 bytes are not
								 * compressed */
	INT_MAX,					/* No upper limit on what we'll try to
								 * compress */
	25,							/* Require 25% compression rate, or not worth
								 * it */
	1024,						/* Give up if no compression in the first 1KB */
	128,						/* Stop history lookup if a match of 128 bytes
								 * is found */
	10							/* Lower good match size by 10% at every loop
								 * iteration */
};
const PGLZ_Strategy *const PGLZ_strategy_default = &strategy_default_data;


static const PGLZ_Strategy strategy_always_data = {
	0,							/* Chunks of any size are compressed */
	INT_MAX,
	0,							/* It's enough to save one single byte */
	INT_MAX,					/* Never give up early */
	128,						/* Stop history lookup if a match of 128 bytes
								 * is found */
	6							/* Look harder for a good match */
};
const PGLZ_Strategy *const PGLZ_strategy_always = &strategy_always_data;


/* ----------
 * Statically allocated work arrays for history
 *
 * hist_start[]: one entry per hash bucket, holding the index of the
 * first history entry in that bucket's chain, or -1 if empty.
 *
 * hist_entries[]: ring buffer of history entries, indexed 0..PGLZ_HISTORY_SIZE.
 * Entry 0 is now a valid entry (the old code wasted it as a sentinel).
 * ----------
 */
static int16 hist_start[PGLZ_MAX_HISTORY_LISTS];
static PGLZ_HistEntry hist_entries[PGLZ_HISTORY_SIZE + 1];

/* ----------
 * pglz_hist_idx -
 *
 *		Computes the history table slot for the lookup by the next 4
 *		characters in the input.
 *
 * NB: because we use the next 4 characters, we are not guaranteed to
 * find 3-character matches; they very possibly will be in the wrong
 * hash list.  This seems an acceptable tradeoff for spreading out the
 * hash keys more.
 *
 * This uses a Fibonacci multiply-shift hash instead of the original
 * polynomial hash.  The original ((s[0]<<6)^(s[1]<<4)^(s[2]<<2)^s[3])
 * had poor avalanche properties for structured data (ASCII text, SQL,
 * JSON): on typical English text, it produced only ~260 unique hashes
 * for 8K of input across 8192 buckets (3% utilization), leading to
 * average chain lengths of ~30.  The Fibonacci hash spreads entries
 * uniformly across all buckets, reducing chain traversal time in
 * pglz_find_match and improving cache behavior.
 *
 * The constant 2654435761 is the golden ratio × 2^32, commonly used
 * in hash tables (Knuth TAOCP Vol 3).  LZ4 uses the same technique.
 *
 * The 4 bytes are read portably via byte-by-byte assembly (not a
 * pointer cast) to avoid undefined behavior and endianness dependence.
 * GCC/Clang optimize this to a single 4-byte load on x86-64.
 * ----------
 */
static inline int
pglz_hist_idx(const char *s, const char *end, int mask)
{
	uint32		h;

	if ((end - s) < 4)
		return ((int) (unsigned char) s[0]) & mask;

	/*
	 * Read 4 bytes portably and multiply by the Fibonacci constant.
	 * We use little-endian assembly (low byte first) for consistency
	 * across architectures.
	 */
	h = ((uint32) (unsigned char) s[0]) |
		((uint32) (unsigned char) s[1] << 8) |
		((uint32) (unsigned char) s[2] << 16) |
		((uint32) (unsigned char) s[3] << 24);
	h *= 2654435761u;

	/*
	 * Use the high bits (best-mixed after multiply).  Shift right by 19
	 * to get 13 bits, then mask to the table size.  For smaller tables,
	 * the mask further restricts the range.
	 */
	return (int) (h >> 19) & mask;
}


/* ----------
 * pglz_hist_unlink -
 *
 *		Unlink an entry from its current bucket chain by scanning forward
 *		from the chain head to find the predecessor, then splice out.
 *
 * CRITICAL: This function must NOT have a chain-length limit.  If we
 * abandon an unlink before finding the predecessor, the entry's next
 * field gets overwritten when recycled into a new chain — this corrupts
 * the old chain (the predecessor now follows next into a completely
 * different bucket's chain).
 *
 * The worst case (all 4096 entries in one bucket) requires the input
 * to produce 4096 consecutive identical hash values — degenerate data
 * that compresses trivially, so the amortized cost is acceptable.
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
			*pp = entry->next;	/* splice out */
			return;
		}
		pp = &hentries[*pp].next;
	}

	/*
	 * Entry not found in chain — bookkeeping is wrong.  In assert builds,
	 * treat this as a bug.  In production, return silently as defense
	 * against corruption — the entry will be overwritten anyway.
	 */
#ifdef USE_ASSERT_CHECKING
	Assert(false);				/* should not happen */
#endif
}


/* ----------
 * pglz_hist_add -
 *
 *		Adds a new entry to the history table.
 *
 * If *recycle is true, then we are recycling a previously used entry,
 * and must first unlink it from its old bucket chain via predecessor
 * scan (singly-linked unlink).
 *
 * hist_next and recycle are modified by this function.
 *
 * Invariant: every bucket chain is valid at all times. Each entry
 * belongs to exactly one bucket. -1 terminates every chain.
 * ----------
 */
static inline void
pglz_hist_add(int16 *hstart, PGLZ_HistEntry *hentries,
			  int *hist_next, bool *recycle,
			  const char *s, const char *end, int mask)
{
	int			hindex = pglz_hist_idx(s, end, mask);
	int16		entry_idx = (int16) *hist_next;
	PGLZ_HistEntry *myhe = &hentries[entry_idx];

	if (*recycle)
	{
		/* Unlink from old bucket chain (predecessor scan) */
		pglz_hist_unlink(hstart, hentries, entry_idx);
	}

	/* Insert at head of the new bucket chain */
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
 * pglz_out_ctrl -
 *
 *		Outputs the last and allocates a new control byte if needed.
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


/* ----------
 * pglz_out_literal -
 *
 *		Outputs a literal byte to the destination buffer including the
 *		appropriate control bit.
 * ----------
 */
static inline void
pglz_out_literal(unsigned char **ctrlp, unsigned char *ctrlb,
				 unsigned char *ctrl, unsigned char **buf, unsigned char byte)
{
	pglz_out_ctrl(ctrlp, ctrlb, ctrl, buf);
	*(*buf)++ = byte;
	*ctrl <<= 1;
}


/* ----------
 * pglz_out_tag -
 *
 *		Outputs a backward reference tag of 2-4 bytes (depending on
 *		offset and length) to the destination buffer including the
 *		appropriate control bit.
 * ----------
 */
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
 * pglz_find_match -
 *
 *		Lookup the history table if the actual input stream matches
 *		another sequence of characters, starting somewhere earlier
 *		in the input buffer.
 *
 * The caller must ensure (end - input) >= 4.  This allows us to use a
 * 4-byte memcmp() as a fast-reject filter: if the first 4 bytes don't
 * match, skip immediately to the next history entry.  Modern compilers
 * (GCC 7.1+, Clang) optimize memcmp(a, b, 4) == 0 into a single 4-byte
 * load and compare — no function call overhead.
 *
 * This sacrifices rare 3-byte matches that differ in the 4th byte,
 * for consistent speed improvement.  The ratio impact is negligible.
 *
 * Boundary proof for hp (the history pointer):
 *   - hp was a previous position in the input buffer, so hp >= source
 *     and hp < input.
 *   - The caller guarantees input <= end - 4, and hp < input, therefore
 *     hp <= input - 1 <= end - 5, so hp + 4 <= end - 1 < end.
 *   - The 4-byte memcmp at hp is safe.
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

	/*
	 * Traverse the linked history list until a good enough match is found.
	 */
	hentno = hstart[pglz_hist_idx(input, end, mask)];
	while (hentno != PGLZ_INVALID_ENTRY)
	{
		PGLZ_HistEntry *hent = &hist_entries[hentno];
		const char *ip = input;
		const char *hp = hent->pos;
		int32		thisoff;
		int32		thislen;

		/*
		 * Stop if the offset does not fit into our tag anymore.
		 */
		thisoff = ip - hp;
		if (thisoff >= 0x0fff)
			break;

		/*
		 * Boundary assertions (debug builds only).
		 * hp >= source: hp is a previous input position, always >= buffer start.
		 * hp < input: hp must precede current position (backward reference only).
		 * hp + 4 <= end: the caller guarantees input <= end - 4, and
		 *   hp < input, so hp + 4 <= input - 1 + 4 <= end - 1 < end + 1.
		 *   Actually hp + 4 <= end strictly since hp <= end - 5.
		 */
#ifdef USE_ASSERT_CHECKING
		Assert(hp >= source && hp < ip);
		Assert(hp + 4 <= end);
#endif

		/*
		 * Use 4-byte memcmp as a fast-reject filter.  If the first 4 bytes
		 * don't match, skip immediately.  This eliminates the first 3
		 * iterations of the inner loop for every candidate.
		 */
		if (memcmp(ip, hp, 4) == 0)
		{
			thislen = 4;
			ip += 4;
			hp += 4;

			/*
			 * If we already have a match of 16+ bytes, use memcmp to
			 * quickly check if this match is at least as long.
			 */
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

			/* Extend match byte-by-byte */
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

		/*
		 * Remember this match as the best (if it is)
		 */
		if (thislen > len)
		{
			len = thislen;
			off = thisoff;
		}

next_entry:
		/*
		 * Advance to the next history entry
		 */
		hentno = hent->next;

		/*
		 * Defense-in-depth: limit chain traversal to PGLZ_MAX_CHAIN hops.
		 * This bounds worst-case per-byte cost with pathological hash
		 * collisions.  Normal inputs have average chain length < 1
		 * (4096 entries / 8192 buckets), so this limit is never hit in
		 * practice.
		 */
		if (++chain_len >= PGLZ_MAX_CHAIN)
			break;

		/*
		 * Be happy with lesser good matches the more entries we visited. But
		 * no point in doing calculation if we're at end of list.
		 */
		if (hentno != PGLZ_INVALID_ENTRY)
		{
			if (len >= good_match)
				break;
			good_match -= (good_match * good_drop) / 100;
		}
	}

	/*
	 * Return match information only if it results at least in one byte
	 * reduction.
	 */
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
 *		Compresses source into dest using strategy. Returns the number of
 *		bytes written in buffer dest, or -1 if compression fails.
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

	/*
	 * Our fallback strategy is the default.
	 */
	if (strategy == NULL)
		strategy = PGLZ_strategy_default;

	/*
	 * If the strategy forbids compression (at all or if source chunk size out
	 * of range), fail.
	 */
	if (strategy->match_size_good <= 0 ||
		slen < strategy->min_input_size ||
		slen > strategy->max_input_size)
		return -1;

	/*
	 * Limit the match parameters to the supported range.
	 */
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

	/*
	 * Compute the maximum result size allowed by the strategy, namely the
	 * input size minus the minimum wanted compression rate.  This had better
	 * be <= slen, else we might overrun the provided output buffer.
	 */
	if (slen > (INT_MAX / 100))
	{
		/* Approximate to avoid overflow */
		result_max = (slen / 100) * (100 - need_rate);
	}
	else
		result_max = (slen * (100 - need_rate)) / 100;

	/*
	 * Experiments suggest that these hash sizes work pretty well. A large
	 * hash table minimizes collision, but has a higher startup cost. For a
	 * small input, the startup cost dominates. The table size must be a power
	 * of two.
	 */
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

	/*
	 * Initialize the history lists to empty.  We use -1 as the sentinel
	 * for "no entry".  A loop is used rather than memset for clarity —
	 * the cost is negligible (runs once per pglz_compress call).
	 *
	 * We do not need to initialize the hist_entries[] array; its entries
	 * are set up as they are used.
	 */
	{
		int		i;

		for (i = 0; i < hashsz; i++)
			hist_start[i] = PGLZ_INVALID_ENTRY;
	}

	/*
	 * Compress the source directly into the output buffer.
	 *
	 * The main loop processes bytes while at least 4 bytes remain.  This
	 * guarantees the 4-byte memcmp in pglz_find_match is safe.  The last
	 * 1-3 bytes are handled as literals in the tail loop below.
	 */
	while (dp < dend - 3)
	{
		/*
		 * If we already exceeded the maximum result size, fail.
		 *
		 * We check once per loop; since the loop body could emit as many as 4
		 * bytes (a control byte and 3-byte tag), PGLZ_MAX_OUTPUT() had better
		 * allow 4 slop bytes.
		 */
		if (bp - bstart >= result_max)
			return -1;

		/*
		 * If we've emitted more than first_success_by bytes without finding
		 * anything compressible at all, fail.  This lets us fall out
		 * reasonably quickly when looking at incompressible input (such as
		 * pre-compressed data).
		 */
		if (!found_match && bp - bstart >= strategy->first_success_by)
			return -1;

		/*
		 * Try to find a match in the history.  pglz_find_match uses a
		 * 4-byte memcmp fast-reject, so the caller guarantees at least
		 * 4 bytes remain (ensured by the loop condition above).
		 */
		if (pglz_find_match(hist_start, dp, dend, &match_len,
							&match_off, good_match, good_drop, mask,
							source))
		{
			/*
			 * Create the tag and advance dp by the full match length,
			 * skipping hist_add for the intermediate positions.
			 *
			 * Skip-after-match optimization: instead of advancing dp one byte
			 * at a time (calling hist_add for every matched byte), we jump dp
			 * forward by match_len in one step.  The intermediate positions
			 * are NOT added to the history table, which means the compressor
			 * may miss some matches that start inside the matched region.
			 *
			 * Tradeoff: throughput vs. compression ratio.
			 * On highly compressible data (logs, SQL dumps, JSON) this gives
			 * 2-10x speedup with ~1-3pp ratio cost.  On incompressible data
			 * (random bytes, pre-compressed) there is no effect since no
			 * matches are found.  Not suitable for workloads where compression
			 * ratio is critical.
			 */
			pglz_out_tag(&ctrlp, &ctrlb, &ctrl, &bp, match_len, match_off);

			/*
			 * Add only the first byte of the matched region to history
			 * (consistent with where dp currently points), then skip forward.
			 * Clamp to dend to avoid overshooting in boundary cases.
			 */
			pglz_hist_add(hist_start, hist_entries,
						  &hist_next, &hist_recycle,
						  dp, dend, mask);
			dp += match_len;
			if (dp > dend)
				dp = dend;

			found_match = true;
		}
		else
		{
			/*
			 * No match found. Copy one literal byte.
			 */
			pglz_out_literal(&ctrlp, &ctrlb, &ctrl, &bp, *dp);
			pglz_hist_add(hist_start, hist_entries,
						  &hist_next, &hist_recycle,
						  dp, dend, mask);
			dp++;
		}
	}

	/*
	 * Tail: emit the last 0-3 bytes as literals.  We can't use the 4-byte
	 * memcmp fast path here.
	 */
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

	/*
	 * Write out the last control byte and check that we haven't overrun the
	 * output size allowed by the strategy.
	 */
	*ctrlp = ctrlb;
	result_size = bp - bstart;
	if (result_size >= result_max)
		return -1;

	/* success */
	return result_size;
}


/* ----------
 * pglz_decompress -
 *
 *		Decompresses source into dest. Returns the number of bytes
 *		decompressed into the destination buffer, or -1 if the
 *		compressed data is corrupted.
 *
 *		If check_complete is true, the data is considered corrupted
 *		if we don't exactly fill the destination buffer.  Callers that
 *		are extracting a slice typically can't apply this check.
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
		/*
		 * Read one control byte and process the next 8 items (or as many as
		 * remain in the compressed input).
		 */
		unsigned char ctrl = *sp++;
		int			ctrlc;

		for (ctrlc = 0; ctrlc < 8 && sp < srcend && dp < destend; ctrlc++)
		{
			if (ctrl & 1)
			{
				/*
				 * Set control bit means we must read a match tag. The match
				 * is coded with two bytes. First byte uses lower nibble to
				 * code length - 3. Higher nibble contains upper 4 bits of the
				 * offset. The next following byte contains the lower 8 bits
				 * of the offset. If the length is coded as 18, another
				 * extension tag byte tells how much longer the match really
				 * was (0-255).
				 */
				int32		len;
				int32		off;

				len = (sp[0] & 0x0f) + 3;
				off = ((sp[0] & 0xf0) << 4) | sp[1];
				sp += 2;
				if (len == 18)
					len += *sp++;

				/*
				 * Check for corrupt data: if we fell off the end of the
				 * source, or if we obtained off = 0, or if off is more than
				 * the distance back to the buffer start, we have problems.
				 * (We must check for off = 0, else we risk an infinite loop
				 * below in the face of corrupt data.  Likewise, the upper
				 * limit on off prevents accessing outside the buffer
				 * boundaries.)
				 */
				if (unlikely(sp > srcend || off == 0 ||
							 off > (dp - (unsigned char *) dest)))
					return -1;

				/*
				 * Don't emit more data than requested.
				 */
				len = Min(len, destend - dp);

				/*
				 * Now we copy the bytes specified by the tag from OUTPUT to
				 * OUTPUT (copy len bytes from dp - off to dp).  The copied
				 * areas could overlap, so to avoid undefined behavior in
				 * memcpy(), be careful to copy only non-overlapping regions.
				 *
				 * Note that we cannot use memmove() instead, since while its
				 * behavior is well-defined, it's also not what we want.
				 */
				while (off < len)
				{
					/*
					 * We can safely copy "off" bytes since that clearly
					 * results in non-overlapping source and destination.
					 */
					memcpy(dp, dp - off, off);
					len -= off;
					dp += off;

					/*----------
					 * This bit is less obvious: we can double "off" after
					 * each such step.  Consider this raw input:
					 *		112341234123412341234
					 * This will be encoded as 5 literal bytes "11234" and
					 * then a match tag with length 16 and offset 4.  After
					 * memcpy'ing the first 4 bytes, we will have emitted
					 *		112341234
					 * so we can double "off" to 8, then after the next step
					 * we have emitted
					 *		11234123412341234
					 * Then we can double "off" again, after which it is more
					 * than the remaining "len" so we fall out of this loop
					 * and finish with a non-overlapping copy of the
					 * remainder.  In general, a match tag with off < len
					 * implies that the decoded data has a repeat length of
					 * "off".  We can handle 1, 2, 4, etc repetitions of the
					 * repeated string per memcpy until we get to a situation
					 * where the final copy step is non-overlapping.
					 *
					 * (Another way to understand this is that we are keeping
					 * the copy source point dp - off the same throughout.)
					 *----------
					 */
					off += off;
				}
				memcpy(dp, dp - off, len);
				dp += len;
			}
			else
			{
				/*
				 * An unset control bit means LITERAL BYTE. So we just copy
				 * one from INPUT to OUTPUT.
				 */
				*dp++ = *sp++;
			}

			/*
			 * Advance the control bit
			 */
			ctrl >>= 1;
		}
	}

	/*
	 * If requested, check we decompressed the right amount.
	 */
	if (check_complete && (dp != destend || sp != srcend))
		return -1;

	/*
	 * That's it.
	 */
	return (char *) dp - dest;
}


/* ----------
 * pglz_maximum_compressed_size -
 *
 *		Calculate the maximum compressed size for a given amount of raw data.
 *		Return the maximum size, or total compressed size if maximum size is
 *		larger than total compressed size.
 *
 * We can't use PGLZ_MAX_OUTPUT for this purpose, because that's used to size
 * the compression buffer (and abort the compression). It does not really say
 * what's the maximum compressed size for an input of a given length, and it
 * may happen that while the whole value is compressible (and thus fits into
 * PGLZ_MAX_OUTPUT nicely), the prefix is not compressible at all.
 * ----------
 */
int32
pglz_maximum_compressed_size(int32 rawsize, int32 total_compressed_size)
{
	int64		compressed_size;

	/*
	 * pglz uses one control bit per byte, so if the entire desired prefix is
	 * represented as literal bytes, we'll need (rawsize * 9) bits.  We care
	 * about bytes though, so be sure to round up not down.
	 *
	 * Use int64 here to prevent overflow during calculation.
	 */
	compressed_size = ((int64) rawsize * 9 + 7) / 8;

	/*
	 * The above fails to account for a corner case: we could have compressed
	 * data that starts with N-1 or N-2 literal bytes and then has a match tag
	 * of 2 or 3 bytes.  It's therefore possible that we need to fetch 1 or 2
	 * more bytes in order to have the whole match tag.  (Match tags earlier
	 * in the compressed data don't cause a problem, since they should
	 * represent more decompressed bytes than they occupy themselves.)
	 */
	compressed_size += 2;

	/*
	 * Maximum compressed size can't be larger than total compressed size.
	 * (This also ensures that our result fits in int32.)
	 */
	compressed_size = Min(compressed_size, total_compressed_size);

	return (int32) compressed_size;
}
