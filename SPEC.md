# pglz compression optimization — specification

## Goal

Speed up the built-in `pglz` compressor in Postgres by 15–30% with zero impact on the decompression format. No changes to on-disk layout, no new dependencies, fully backward-compatible. The patch must be structured for reviewability: one logical change per commit, each independently committable.

## Why this matters

pglz is Postgres's only built-in compressor. It is used by:

- **TOAST** — compresses any varlena datum exceeding ~2 KiB. Every Postgres installation uses this, whether or not LZ4/zstd is available. Even when LZ4 is the default for new tables, existing TOAST data compressed with pglz must still be decompressed by the old code, and re-compression after UPDATE still uses pglz unless the column's compression method was explicitly changed.
- **WAL compression** (`wal_compression = pglz`) — compresses full-page images in WAL records. On write-heavy workloads with WAL compression enabled, pglz routinely dominates `perf top` — 12–18% of CPU cycles in production-like TPC-C benchmarks ([pgsql-hackers thread](https://www.postgresql.org/message-id/flat/CAAhFRxj0MTsOp8f162n9YhZVwPwf0OG6z3FqU_8jd%2BULfpbpBg%40mail.gmail.com)).
- **Base backup compression** (`pg_basebackup --compress=server-pglz`).

LZ4 and zstd have been available since PG14 (TOAST) and PG15 (WAL), but:

1. Many managed providers still ship binaries without `--with-lz4` or `--with-zstd`.
2. Existing TOAST data stays pglz-compressed until rewritten — which may be never.
3. Users who don't know about (or can't use) the new options still get pglz by default.
4. Tom Lane (2020): "Even if lz4 or something else shows up, the existing code will remain important for TOAST purposes. It would be years before we lose interest in it."

A 15–30% speedup in pglz benefits every Postgres installation silently — no configuration changes, no rewrite needed.

## Prior art

This work builds on Andrey Borodin and Vladimir Leskov's patch (CF entry #2897), which went through 9 versions and reviews from Mark Dilger, Tomas Vondra, Justin Pryzby, and Andres Freund over 2020–2023 before being withdrawn. The patch was withdrawn because:

1. It bundled too many changes into a single commit, making review difficult.
2. v9 introduced a heap-buffer-overflow in `pglz_hist_add` (caught by ASan/Valgrind).
3. There was a mystery 1-byte regression in compression ratio for some inputs.
4. Both Andres Freund and Tomas Vondra requested the patch be split into independent, reviewable pieces.

The optimizations themselves were validated: Tomas Vondra independently measured 15–18% speedup on compressible data. The 14% overall speedup on `REINDEX` with `wal_compression = on` was reproduced. The problem was packaging, not substance.

## The four optimizations

The original patch's commit message identified four independent changes. This is the splitting strategy both Andres and Tomas requested:

### 1. Convert macros to inline functions

**What:** Replace `pglz_hist_add`, `pglz_hist_idx`, `pglz_out_ctrl`, `pglz_out_literal`, `pglz_out_tag` macros with `static inline` functions.

**Why:** The current macros are error-prone (multiple evaluation of arguments — the code even has "The macro would do it four times — Jan." comments), hard to debug, and hard for the compiler to optimize. Inline functions give the same performance with better type safety, debuggability, and no multiple-evaluation bugs.

**Risk:** Zero — this is pure refactoring. The generated code should be identical. Any difference is a compiler regression, not our bug.

**Reviewability:** This commit touches only `pg_lzcompress.c`. It changes syntax, not semantics. Easy to verify by comparing disassembly or running the existing test suite.

### 2. Replace pointer-based hash table with uint16 index-based hash table

**What:** Change `PGLZ_HistEntry` from:

```c
typedef struct PGLZ_HistEntry {
    struct PGLZ_HistEntry *next;  /* 8 bytes on 64-bit */
    struct PGLZ_HistEntry *prev;
    int         hindex;
    const char *pos;
} PGLZ_HistEntry;   /* 32 bytes on 64-bit */
```

to:

```c
typedef struct PGLZ_HistEntry {
    int16   next;       /* index into hist_entries[], -1 = end */
    uint16  hindex;     /* hash bucket this entry belongs to */
    const char *pos;    /* position in input buffer */
} PGLZ_HistEntry;   /* 12 bytes on 64-bit (with padding) */
```

**Why:**
- Shrinks each entry from 32 bytes to 12 bytes (2.7×), which means the history table fits better in L1/L2 cache. The history table has 4097 entries — that's 131 KiB with pointers vs 49 KiB with indexes. L1 data cache on most CPUs is 32–48 KiB, so the indexed version fits, the pointer version doesn't.
- The `hist_start` array shrinks from `PGLZ_HistEntry *` (8 bytes × 8192 = 64 KiB) to `int16` (2 bytes × 8192 = 16 KiB).
- Total working set drops from ~195 KiB to ~65 KiB — fits in L1+L2 instead of spilling to L3.

**Risk:** Low. The linked list traversal logic changes from pointer chasing to index arithmetic, but the algorithm is identical. The `prev` pointer is also eliminated (see #3).

### 3. Remove the prev pointer — singly-linked list

**What:** Convert the doubly-linked hash chain to a singly-linked list. When recycling an old entry, instead of O(1) removal via prev pointer, invalidate it by checking if `entry->pos` is still within the valid window during traversal.

**Why:** The `prev` pointer exists solely to enable O(1) removal when recycling old entries. But:
- Removal is rare relative to traversal (only when an entry is >4096 positions old).
- The cost of the `prev` pointer is paid on every entry: 2 extra bytes per entry in the indexed scheme, and worse cache utilization.
- During traversal, we already skip entries whose offset is >=0x0fff. Entries being recycled have offset >4096 by definition, so they are naturally skipped.
- The original code already handles "invalid" entries in the traversal loop — this just means more entries may be found "invalid" during traversal, which is cheap (a comparison and a continue).

**Risk:** Moderate. This changes the algorithm slightly — stale entries remain in the hash chain until naturally evicted. Must verify no performance regression on short inputs where chains might be longer. Also must verify no out-of-bounds access (the ASan bug in v9 was in `pglz_hist_add`; this change touches the same area).

### 4. Use 4-byte comparisons instead of byte-by-byte

**What:** In `pglz_find_match`, replace the inner byte-by-byte comparison loop with a 4-byte `memcmp()` for the initial match test:

```c
/* Current code: byte-by-byte from the start */
while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH) {
    thislen++; ip++; hp++;
}

/* Optimized: 4-byte check first, then extend */
if (memcmp(ip, hp, 4) == 0) {
    /* extend the match */
    thislen = 4; ip += 4; hp += 4;
    while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH) {
        thislen++; ip++; hp++;
    }
}
```

**Why:**
- Modern compilers (GCC 7.1+, Clang) optimize `memcmp(a, b, 4) == 0` into a single 4-byte load and compare instruction on x86-64 and ARM64 — no function call overhead.
- Andrey verified this on Godbolt: the generated assembly is identical to a direct `*(uint32_t*)` cast, but without the undefined behavior of unaligned access.
- This skips the first 3 iterations of the inner loop for every match candidate, which adds up fast since the inner loop is the hottest code in the compressor.

**Risk:** Moderate. Must ensure we don't read past the end of the input buffer — `memcmp(ip, hp, 4)` requires at least 4 bytes remaining. The existing code already handles the "last 3 bytes" specially (the hash function needs 4 bytes too), so the input cursor should always have enough lookahead, but this must be verified carefully. **The v9 ASan heap-buffer-overflow was likely related to this optimization reading past the buffer end in `pglz_hist_add`.**

**Compression ratio note:** The 4-byte initial comparison means we won't find 3-byte matches that happen to differ in the 4th byte. In practice this is extremely rare and the ratio impact is negligible (Andrey measured occasional 1-byte differences on specific inputs). The speed gain vastly outweighs this. Tomas Vondra: "I'm not aware of a specification of what the compression must (not) produce" — the decompressor accepts any valid tag sequence, so slightly different encoding is fine.

## Implementation plan

### Step 1: Refactor macros to inline functions

**Files:** `src/common/pg_lzcompress.c`

1. Convert `pglz_hist_idx` macro to `static inline int pglz_hist_idx(const char *s, const char *end, int mask)`.
2. Convert `pglz_hist_add` macro to `static inline void pglz_hist_add(...)`. This is the most complex one — the macro modifies `hist_next` and `hist_recycle` through pointer arguments.
3. Convert `pglz_out_ctrl`, `pglz_out_literal`, `pglz_out_tag` macros to inline functions. These modify the control byte pointer and buffer pointer, so they need pointer-to-pointer arguments.
4. Run the full regression test suite. Compare disassembly of `pglz_compress` before and after to verify no code generation regressions.

**Commit message:** `Refactor pglz compression macros to inline functions`

**Tests:** `make check-world` must pass. No functional change expected.

### Step 2: Replace pointer-based history with uint16 indexes

**Files:** `src/common/pg_lzcompress.c`, `src/include/common/pg_lzcompress.h` (if PGLZ_HistEntry is exposed — check)

1. Change `PGLZ_HistEntry` to use `int16 next` instead of `struct PGLZ_HistEntry *next` and `struct PGLZ_HistEntry *prev`.
2. Change `hist_start` from `PGLZ_HistEntry *` to `int16`.
3. Use index 0 as the sentinel (invalid), index 1..4096 as valid entries.
4. Update `pglz_hist_add` to work with indexes.
5. Update `pglz_find_match` to traverse by index.
6. Keep the `prev` pointer for now (as `int16 prev`) — removal is step 3.

**Commit message:** `Use uint16 indexes instead of pointers in pglz history table`

**Tests:** Full regression suite. Also run Tomas's compression benchmark (random and compressible data at 1K, 4K, 1M sizes) to verify no performance regression and measure the speedup from better cache behavior.

### Step 3: Remove prev pointer — singly-linked chains with lazy invalidation

**Files:** `src/common/pg_lzcompress.c`

1. Remove `prev` field from `PGLZ_HistEntry`.
2. In `pglz_hist_add`, when recycling an entry, skip the unlink step — just overwrite the entry and assign it to its new hash bucket.
3. In `pglz_find_match`, implement **lazy invalidation**: when traversing a chain, if an entry's `hindex` does not match the current search bucket, the entry was "stolen" by a different hash chain during recycling. **Break traversal immediately** — the chain is effectively severed at this point, since all subsequent nodes were linked before this stolen node and are therefore older (and likely also invalid).

```c
/* Lazy invalidation during traversal */
int16 next_idx = hist_start[hindex];
while (next_idx != 0) {
    PGLZ_HistEntry *entry = &hist_entries[next_idx];

    /* Stolen node: recycled into a different hash bucket.
     * Chain is broken here — stop. */
    if (entry->hindex != hindex)
        break;

    /* Standard distance check */
    int32 thisoff = ip - entry->pos;
    if (thisoff >= 0x0fff)
        break;

    /* ... perform match checks ... */
    next_idx = entry->next;
}
```

4. This is semantically equivalent to the current behavior (old entries are skipped), but without the O(1) unlink cost on every recycling operation. The `hindex` check is a single integer comparison — essentially free.

**Commit message:** `Remove prev pointer from pglz history entries`

**Tests:** Full regression suite. Benchmark on both short (512 bytes) and long (1 MiB) inputs to verify no regression. Specifically test with high-entropy data where chains may be longer due to hash collisions — the "stolen node" break must not cause early termination on valid chains.

### Step 4: Use 4-byte comparisons in match finding

**Files:** `src/common/pg_lzcompress.c`

1. In `pglz_find_match`, add a `memcmp(ip, hp, 4) == 0` check before entering the byte-by-byte extension loop. **The caller must guarantee `(end - input) >= 4`** — if fewer than 4 bytes remain, the main compression loop should emit literals directly without calling `pglz_find_match`. This is the clean fix for the v9 ASan bug.

```c
/* In the main compression loop: */
while (dp < dend) {
    /* If fewer than 4 bytes remain, emit literals — no match possible
     * that would save space (min match = 3, but tag = 2-3 bytes). */
    if (dend - dp < 4) {
        pglz_out_literal(ctrlp, ctrlb, ctrl, bp, *dp);
        pglz_hist_add(..., dp, dend, mask);
        dp++;
        continue;
    }

    /* Safe to call pglz_find_match — at least 4 bytes available */
    if (pglz_find_match(...)) { ... }
}
```

2. Inside `pglz_find_match`, use `memcmp(ip, hp, 4) == 0` as a fast-reject filter. If the first 4 bytes don't match, skip immediately to the next history entry (don't fall through to byte-by-byte — the cost of checking 1-3 byte matches is not worth the branch overhead):

```c
if (memcmp(ip, hp, 4) == 0) {
    thislen = 4; ip += 4; hp += 4;
    while (ip < end && *ip == *hp && thislen < PGLZ_MAX_MATCH) {
        thislen++; ip++; hp++;
    }
} else {
    /* No 4-byte match — skip to next history entry.
     * We sacrifice rare 3-byte matches for consistent speed. */
    hent_idx = entry->next;
    continue;
}
```

3. **Critically:** verify that `pglz_hist_add` does not read past the buffer end. The hash function (`pglz_hist_idx`) reads 4 bytes starting from the current position. The existing code has a `(end - s) < 4` fallback to `(int)s[0]`, but this boundary must be verified under ASan with inputs of every length mod 4.

**Commit message:** `Use 4-byte comparisons in pglz match finding`

**Tests:** Full regression suite plus ASan, Valgrind, and fuzz testing:
- `./configure --enable-debug CFLAGS="-fsanitize=address" && make check-world`
- Valgrind: `make check EXTRA_REGRESS_OPTS="--valgrind"`
- Boundary inputs: length 0, 1, 2, 3, 4, 5, 2048, 4096, 4097 bytes
- Inputs where the last 3 bytes form a potential match
- **Fuzz target** (see step 5)

### Step 5: Fuzz testing

Create a standalone fuzz target `fuzz_pglz.c` that links against the modified `pg_lzcompress.c`:

```c
/* fuzz_pglz.c — libFuzzer target for pglz */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char compressed[PGLZ_MAX_OUTPUT(size)];
    char decompressed[size + 1];

    int32 clen = pglz_compress((const char *)data, size,
                               compressed, PGLZ_strategy_always);
    if (clen >= 0) {
        int32 dlen = pglz_decompress(compressed, clen,
                                     decompressed, size, true);
        assert(dlen == size);
        assert(memcmp(data, decompressed, size) == 0);
    }
    return 0;
}
```

- Compile with ASan + UBSan: `-fsanitize=address,undefined,fuzzer`
- Run for at least 10M iterations
- Focus corpus: lengths 0–8192, especially N where N % 4 ∈ {1,2,3}, history-wrap boundaries (4096, 4097), TOAST threshold (2048)
- **Every commit must pass fuzz testing independently**, not just the final result

### Step 6: Benchmark and commit message preparation

1. Run the full benchmark suite on the final combined result:
   - Tomas's test: random/compressible data at 1K, 4K, 1M into unlogged table.
   - Andrey's test: `REINDEX table pgbench_accounts` (scale 100) with `wal_compression = on`.
   - TPC-C workload with `wal_compression = on` if feasible.
   - TOAST-heavy workload: insert 1M rows of compressible text into a text column.
   - **Worst-case test:** random (incompressible) data — must show no regression from stolen-node checks or memcmp overhead on miss.
2. Measure on both x86-64 and ARM64 if possible.
3. Benchmark each commit independently (not just the combined result) so reviewers can see the isolated contribution of each optimization.
4. Prepare the commitfest submission with a clear summary email to pgsql-hackers.

## Key design decisions

### Split into 4 independent commits

Both Andres Freund and Tomas Vondra explicitly asked for this. The original patch was on its 10th commitfest when withdrawn, largely because reviewers couldn't confidently approve a single 20 KiB patch touching safety-critical compression code. Each commit must be reviewable in isolation, with its own benchmark results.

### memcmp() over direct pointer cast

Andrey's v5 switched from `*(uint32_t*)` to `memcmp(ptr, ptr, 4)`. This is:
- Portable (no alignment issues, no undefined behavior).
- Identical assembly on GCC 7.1+/Clang for x86-64 and ARM64 (verified on Godbolt).
- Does not require architecture detection macros, configure tests, or the `PGLZ_FORCE_MEMORY_ACCESS` mechanism from earlier versions.

### No decompression changes

The decompressor (`pglz_decompress`) is untouched. The compressed output format is identical — the same tags, the same control bytes. The only possible difference is that some inputs might produce a slightly different (but equally valid) tag sequence when the 4-byte comparison skips a 3-byte match. Decompression of any previously compressed data remains correct.

### Keep history table as static arrays

The current code uses statically allocated `hist_start` and `hist_entries` arrays. This means pglz compression is not thread-safe (only one compression can happen at a time per process). This is fine because:
- Backend processes are single-threaded.
- WAL compression happens under a lock.
- There is no current need for concurrent pglz compression within a single process.

Changing this to a dynamically allocated, per-call structure would be a separate patch and is out of scope.

### PGLZ_HISTORY_SIZE stays at 4096

The history window of 4096 bytes is baked into the tag format (12-bit offset). Changing it would require a format change, which is explicitly out of scope.

## Alternative approaches considered

Andrey Borodin raised the question: are Vladimir's 2020 optimizations the best we can do, or are there fundamentally better approaches within the pglz format constraints?

### Approaches within scope (pglz-compatible output)

These produce the same compressed format and can coexist with the four core optimizations:

1. **Better hash function.** The current hash `(s[0]<<6 ^ s[1]<<4 ^ s[2]<<2 ^ s[3]) & mask` dates from the 1990s. A multiply-shift hash (e.g., `((read32(s) * 2654435761u) >> (32 - log2(hashsz)))`) would produce fewer collisions, shortening chain traversals. This could be a fifth independent commit. Worth prototyping.

2. **Skip bytes after long matches.** After emitting a match of N bytes, the current code adds all N bytes to the history table. For long matches (say, >16 bytes), most of those history entries are redundant — the next useful match is likely at or near the end of the current match. LZ4 and zstd skip intermediate bytes. This trades a marginal compression ratio loss for significant speed on compressible data. Could be significant since `pglz_hist_add` is hot in the profile.

3. **Lazy matching.** Before emitting a match at position P, check if position P+1 has a *better* match. If so, emit a literal at P and use the better match at P+1. Standard technique in zlib/zstd. Improves compression ratio, which counteracts the ratio loss from 4-byte comparisons.

### Approaches out of scope (format or API changes)

These would require format changes or new infrastructure — interesting but separate patches:

4. **SIMD-accelerated comparison.** Using SSE2/NEON for 16-byte or 32-byte match extension. The win is real but (a) requires `#ifdef` per architecture, (b) the match extension loop is not the primary bottleneck (hash table traversal is), and (c) `memcmp` with a runtime length already gets auto-vectorized by modern compilers for long matches.

5. **Parallel hash table.** The static `hist_entries` array means compression is single-threaded per process. For PG18's AIO work, thread-safe compression might matter. But this is a separate concern from performance.

6. **Larger history window.** The 4096-byte / 12-bit offset limit is baked into the tag format. A larger window would find more matches but requires format changes — out of scope.

### Decision

Start with the four proven optimizations (reviewed, benchmarked, understood). Prototype the hash function improvement (#1) and history-skip (#2) as optional fifth and sixth commits. If they show measurable improvement, include them. If not, ship without them.

The key insight: the four core optimizations attack the data structure (cache misses, memory layout) and the inner loop (branch elimination). These are the right targets — `perf` profiles show the time is split between hash table operations and match comparison, not the hash function itself. But we should measure, not assume.

## Known risks

1. **ASan/Valgrind clean build is mandatory.** The v9 buffer overflow was in `pglz_hist_add` at line 310, reading 1 byte past the input buffer. This happens when adding the last few bytes of input to the history — the hash function reads 4 bytes but there may be fewer remaining. The fix is to ensure `pglz_hist_idx` never reads past `end`, which it already handles (the `(end - s) < 4` check), but the hist_add macro in v9 was called after advancing `dp` past where it should have been.

2. **Compression ratio stability.** The 4-byte comparison means some 3-byte matches are missed. This produces valid but slightly larger output. In Andrey's testing, this affected only one known case by 1 byte. Tomas confirmed this is acceptable — there is no compression ratio guarantee in the pglz specification.

3. **Regression on short strings.** The optimizations target medium-to-large inputs (>2 KiB). For very short strings (which pglz already handles poorly), the overhead of the optimizations should be negligible, but this must be verified. The TOAST threshold is ~2 KiB, so strings shorter than that are rarely compressed in practice.

4. **Interaction with concurrent development.** The custom compression methods thread (pluggable TOAST compression) may change how `pg_lzcompress.c` is called. Our patch touches only the internal implementation, not the API, so conflicts should be minimal.

## Files changed

All changes are in a single file: `src/common/pg_lzcompress.c`.

The header `src/include/common/pg_lzcompress.h` is unchanged — `PGLZ_HistEntry` is not exposed in the public header (it's defined locally in the .c file).

## Success criteria

1. All four commits apply cleanly to current Postgres master.
2. `make check-world` passes on all supported platforms.
3. ASan and Valgrind builds show zero errors.
4. Fuzz testing: 10M+ iterations with ASan+UBSan, zero findings.
5. Benchmark shows ≥15% speedup on compressible data (TOAST-heavy workload).
6. Benchmark shows ≥10% speedup on WAL compression workload.
7. No measurable regression on random (incompressible) data.
8. Compression ratio change is ≤1 byte on standard test corpus.
9. Each commit is independently reviewable, committable, and fuzz-clean.

## References

- [pgsql-hackers thread: "pglz compression performance, take two"](https://www.postgresql.org/message-id/flat/CAAhFRxj0MTsOp8f162n9YhZVwPwf0OG6z3FqU_8jd%2BULfpbpBg%40mail.gmail.com)
- [Original Vladimir Leskov proposal (2020)](https://www.postgresql.org/message-id/169163A8-C96F-4DBE-A062-7D1CECBE9E5D@yandex-team.ru)
- [Commitfest entry #2897 (withdrawn)](https://commitfest.postgresql.org/patch/2897/)
- [Mark Drinkwater's earlier pglz optimization (2013)](https://www.postgresql.org/message-id/flat/5130C914.8080106%40vmware.com) — commit 031cc55
- [Andrey's commit 031cc55 (inspired this work)](https://github.com/x4m/postgres_g/commit/031cc55bbea6b3a6b67c700498a78fb1d4399476)
- [pglz bottleneck in insert benchmark](https://smalldatum.blogspot.com/2020/12/tuning-for-insert-benchmark-postgres_4.html)
- [Unaligned memory access article](https://fastcompression.blogspot.fr/2015/08/accessing-unaligned-memory.html)
- [LZ4 TOAST compression commit](https://github.com/postgres/postgres/commit/bbe0a81db69bd10bd166907c3701492a29aca294) (PG14)
- Current source: `src/common/pg_lzcompress.c` (876 lines as of PG18dev)
