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
    const char *pos;    /* position in input buffer — 8 bytes */
    int16   next;       /* index into hist_entries[], -1 = end */
    uint16  hindex;     /* hash bucket this entry belongs to */
    /* 4 bytes tail padding to align struct to 8 */
} PGLZ_HistEntry;   /* 16 bytes on 64-bit */
```

**Why:**
- Shrinks each entry from 32 bytes to 16 bytes (2×). The pointer must be 8-byte aligned, so with natural alignment the struct is 16 bytes, not 12 (an earlier version of this spec had 12 — that requires `__attribute__((packed))` which Postgres never uses and would hurt performance on some architectures). Still a major win.
- The history table drops from 4097 × 32 = 131 KiB to 4097 × 16 = 64 KiB.
- The `hist_start` array shrinks from `PGLZ_HistEntry *` (8 bytes × 8192 = 64 KiB) to `int16` (2 bytes × 8192 = 16 KiB).
- Total working set drops from ~195 KiB to ~80 KiB — fits in L2 (256 KiB–1 MiB typical) with room to spare, and significantly more L1-friendly.
- Use `-1` as the sentinel for "no entry" in `hist_start` and `next`. Index 0..4095 maps directly to `hist_entries[0..4095]`, wasting no slot. `-1` is the conventional null index in systems code.
- Add compile-time assertions: `StaticAssertExpr(PGLZ_MAX_HISTORY_LISTS <= UINT16_MAX)` and `StaticAssertExpr(PGLZ_HISTORY_SIZE <= INT16_MAX)`.

**Note on further shrinking:** After step 3 removes `prev` and potentially `hindex`, we could represent `pos` as a `uint32` offset from the source start (pglz inputs are bounded by ~1 GiB). This would give us a true 8-byte struct and ~32 KiB table — genuinely fitting in L1. But it adds an addition per dereference and is a bigger semantic change. Worth benchmarking as a follow-on.

**Risk:** Low. The linked list traversal logic changes from pointer chasing to index arithmetic, but the algorithm is identical. The `prev` pointer is also eliminated (see #3).

### 3. Remove the prev pointer — singly-linked unlink via predecessor scan

**What:** Convert the doubly-linked hash chain to a singly-linked list. When recycling an old entry, unlink it from its current bucket chain by scanning forward from `hist_start[entry->hindex]` to find the predecessor, then splice the entry out. This preserves the invariant that **every bucket chain is valid at all times**.

**Why:** The `prev` pointer exists solely to enable O(1) removal when recycling old entries. Removing it:
- Saves 2 bytes per entry (from `int16 prev`), though with alignment this may not change the struct size.
- More importantly, simplifies the data structure and reduces the number of pointer updates on every insert.
- The unlink cost becomes O(chain length) instead of O(1), but chain lengths average 0.5 entries (4096 entries / 8192 buckets) and are bounded by PGLZ_HISTORY_SIZE in the worst case.

**Important correction:** An earlier version of this spec proposed "lazy invalidation" — skipping stale entries during traversal without unlinking. This is **incorrect**: when an entry is recycled, its `next` pointer is overwritten to point into the new bucket's chain. Any old bucket chain that still references this entry would follow `next` into a completely different chain, corrupting traversal. You must unlink before reuse.

**Recycling frequency:** After the first 4096 bytes of input, *every* `pglz_hist_add` call recycles an entry (the ring buffer is full). For a typical 8 KiB TOAST datum, half the calls involve recycling. The argument for removing `prev` is not that recycling is rare — it's that the amortized cost of O(chain_length) predecessor scans is lower than the per-insert cost of maintaining `prev` pointers, because chain lengths are short (average <1) and the cache savings from the smaller struct benefit every traversal.

**Chain length defense:** Add `#define PGLZ_MAX_CHAIN 256` as a traversal limit in `pglz_find_match`. If a chain exceeds this length (possible only with pathological hash collisions), break and move on. LZ4 uses a similar technique (exponential backoff). This is defense-in-depth against worst-case O(n) per match attempt.

**`hindex` field status:** After removing `prev`, `hindex` is still needed — it tells the recycler which bucket chain to scan for the predecessor. If we decided not to unlink at all (lazy invalidation), `hindex` could be used for a stolen-node check, but since we're unlinking properly, `hindex` serves its original purpose.

**Debug assertions:** In Assert-enabled builds, add `Assert(entry->hindex == search_hindex)` during chain traversal to catch any chain corruption immediately.

**Risk:** Moderate. The predecessor scan is a new code path that must be tested thoroughly. Must verify no regression on pathological hash collision inputs.

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

1. Change `PGLZ_HistEntry` to use `int16 next` (index, -1 = end), `uint16 hindex`, `const char *pos`. Put `pos` first for natural alignment.
2. Change `hist_start` from `int16 *` (pointer to entry) to `int16` (index, -1 = empty).
3. Use `-1` as the sentinel (no entry). Indexes 0..4095 map directly to `hist_entries[0..4095]`. Initialize `hist_start` with `memset(hist_start, 0xFF, ...)` for -1, or loop-assign.
4. Add compile-time assertions: `StaticAssertExpr(PGLZ_MAX_HISTORY_LISTS <= UINT16_MAX)`, `StaticAssertExpr(PGLZ_HISTORY_SIZE <= INT16_MAX)`.
5. Keep `prev` as `int16` for now — removal is step 3. The intermediate state has a doubly-linked list with indexes instead of pointers.
6. Update `pglz_hist_add` and `pglz_find_match` to use index arithmetic.
7. In the commit message, state the invariant: "bucket chains remain valid; each entry belongs to exactly one bucket; -1 terminates every chain."

**Commit message:** `Use uint16 indexes instead of pointers in pglz history table`

**Tests:** Full regression suite. Also run Tomas's compression benchmark (random and compressible data at 1K, 4K, 1M sizes) to verify no performance regression and measure the speedup from better cache behavior.

### Step 3: Remove prev pointer — singly-linked with predecessor-scan unlink

**Files:** `src/common/pg_lzcompress.c`

1. Remove `prev` field from `PGLZ_HistEntry`.
2. In `pglz_hist_add`, when recycling an entry, unlink it from its old bucket chain by scanning from `hist_start[entry->hindex]`:

```c
/* Unlink entry from its old bucket chain (predecessor scan) */
static inline void
pglz_hist_unlink(int16 *hist_start, PGLZ_HistEntry *hist_entries,
                 int entry_idx)
{
    PGLZ_HistEntry *entry = &hist_entries[entry_idx];
    int16 *pp = &hist_start[entry->hindex];

    while (*pp != -1) {
        if (*pp == entry_idx) {
            *pp = entry->next;  /* splice out */
            return;
        }
        pp = &hist_entries[*pp].next;
    }
    /* Entry not found in chain — already unlinked or chain was truncated.
     * This can happen if a chain-length limit caused early break during
     * a previous traversal that would have found this entry. Safe to ignore. */
}
```

3. Add `#define PGLZ_MAX_CHAIN 256` and break out of `pglz_find_match` traversal after that many hops. This bounds worst-case chain scan cost.
4. In Assert-enabled builds, add `Assert(entry->hindex == search_hindex)` during chain traversal to catch corruption early.
5. **Invariant preserved:** every bucket chain is valid at all times. No stolen nodes, no broken chains.

**Commit message:** `Remove prev pointer from pglz history entries`

**Tests:** Full regression suite. Benchmark on both short (512 bytes) and long (1 MiB) inputs. Test with pathological hash collision inputs (all bytes identical → all hash to same bucket) to verify chain-length limit works and performance doesn't degrade catastrophically.

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

3. **Boundary proof for `hp`:** Since `hp` always points earlier in the input buffer than `ip` (we're matching backward), and `ip <= end - 4` is guaranteed by the caller, `hp + 4 <= ip + 4 <= end`. For overlapping matches (offset < 4), `hp + 4` may exceed `ip`, but the memory is valid (it's within the input buffer). Add `Assert(hp + 4 <= end)` in debug builds and a block comment proving this invariant.

4. **Critically:** verify that `pglz_hist_add` does not read past the buffer end. The hash function (`pglz_hist_idx`) reads 4 bytes starting from the current position. The existing code has a `(end - s) < 4` fallback to `(int)s[0]`, but this boundary must be verified under ASan with inputs of every length mod 4.

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

## Cross-version compatibility

The compressed output format is unchanged — same tags, same control bytes. But the 4-byte comparison optimization may produce slightly different (equally valid) compressed output for some inputs.

**Required test:** Take a corpus of data (random, compressible, real-world). Compress with stock PG18dev, save the compressed bytes. Compress the same corpus with each patched commit. Verify:
1. Both compressed outputs decompress correctly with the stock decompressor.
2. Both compressed outputs decompress correctly with the patched decompressor.
3. Both decompress to the identical original data.

The compressed bytes may differ between stock and patched — this is expected and acceptable. What matters is that decompression is always correct in both directions. This is critical for rolling upgrades and logical replication.

## Parallel worker safety

The static arrays `hist_start` and `hist_entries` are process-local. Parallel workers are separate processes with independent address spaces, so each worker has its own copy of these arrays. There is no shared-memory contention.

The TOAST call path (`pglz_compress` ← `pglz_compress_datum` ← `toast_compress_datum` ← `toast_tuple_try_compression`) runs in the worker's own process context. WAL compression also runs in the inserting backend. No cross-process coordination is needed.

## Future work

Items explicitly out of scope for this patch set, but worth pursuing:

1. **Decompressor optimization.** `pglz_decompress` has a byte-by-byte copy loop for match copying. For long matches, `memcpy`-based bulk copies would be faster. Separate patch.
2. **`pos` as uint32 offset.** Replace `const char *pos` (8 bytes) with a uint32 offset from source start, shrinking `PGLZ_HistEntry` to 8 bytes (32 KiB table, fits in L1). Adds one addition per dereference. Worth benchmarking.
3. **`test_pglz` contrib module.** A standalone `src/test/modules/test_pglz/` module that exercises compression with various input sizes and patterns, measures throughput, and verifies round-trip correctness. Doubles as the fuzz/benchmark harness. Similar to `pg_test_fsync` and `pg_test_timing`.
4. **Better hash function.** Multiply-shift hash for fewer collisions. See "Alternative approaches" section.
5. **History-skip on long matches.** See "Alternative approaches" section.

## Success criteria

1. All four commits apply cleanly to current Postgres master.
2. `make check-world` passes on all supported platforms.
3. ASan and Valgrind builds show zero errors.
4. Fuzz testing: 10M+ iterations with ASan+UBSan, zero findings.
5. Cross-version round-trip test: patched ↔ stock compress/decompress matrix passes.
6. Benchmark shows ≥15% speedup on compressible data (TOAST-heavy workload).
7. Benchmark shows ≥10% speedup on WAL compression workload.
8. No measurable regression on random (incompressible) data.
9. Compression ratio change is ≤1 byte on standard test corpus.
10. Each commit is independently reviewable, committable, and fuzz-clean.

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
