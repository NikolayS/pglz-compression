# pglz compression optimization — specification

## Goal

Speed up the built-in `pglz` compressor in Postgres by 15–30% with zero impact on the decompression format. No changes to on-disk layout, no new dependencies, fully backward-compatible. The patch must be structured for reviewability: one logical change per commit, each independently committable.

## Why this matters

pglz is Postgres's only built-in compressor. It is used by:

- **TOAST** — compresses any varlena datum exceeding ~2 KiB. Every Postgres installation uses this, whether or not LZ4/zstd is available. Even when LZ4 is the default for new tables, existing TOAST data compressed with pglz must still be decompressed by the old code, and re-compression after UPDATE still uses pglz unless the column's compression method was explicitly changed.
- **WAL compression** (`wal_compression = pglz`) — compresses full-page images in WAL records. On write-heavy workloads, pglz can consume 18% of total CPU (measured by Andrey Borodin on a TPC-C cluster with 32 vCPU, 128 GiB RAM, sync replication, 2000 warehouses).
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

### Step 3: Remove prev pointer — singly-linked chains

**Files:** `src/common/pg_lzcompress.c`

1. Remove `prev` field from `PGLZ_HistEntry`.
2. In `pglz_hist_add`, when recycling an entry, skip the unlink step — just overwrite.
3. In `pglz_find_match`, when traversing a chain, skip entries whose position is outside the valid 4096-byte window (they were recycled but not unlinked). This check is essentially free since we already compare `thisoff >= 0x0fff`.
4. Verify the traversal loop correctly handles chains with stale entries interspersed.

**Commit message:** `Remove prev pointer from pglz history entries`

**Tests:** Full regression suite. Benchmark on both short (512 bytes) and long (1 MiB) inputs to verify no regression from longer chains.

### Step 4: Use 4-byte comparisons in match finding

**Files:** `src/common/pg_lzcompress.c`

1. In `pglz_find_match`, add a `memcmp(ip, hp, 4) == 0` check before entering the byte-by-byte extension loop.
2. Ensure the 4-byte comparison is only done when `end - ip >= 4` (i.e., at least 4 bytes remain in the input). For the tail of the input, fall back to byte-by-byte.
3. Remove the `pglz_compare32` function that was in earlier versions — just use `memcmp()` directly. Modern compilers inline it.
4. **Critically:** verify that `pglz_hist_add` does not read past the buffer end. The hash function (`pglz_hist_idx`) reads 4 bytes starting from the current position. If the input has fewer than 4 bytes remaining, it falls back to `(int)s[0]`. This boundary condition must be exactly right — it's where v9 had the ASan bug.

**Commit message:** `Use 4-byte comparisons in pglz match finding`

**Tests:** Full regression suite plus ASan and Valgrind builds. Specifically:
- `./configure --enable-debug CFLAGS="-fsanitize=address" && make check-world`
- Valgrind: `make check EXTRA_REGRESS_OPTS="--valgrind"`
- Run with inputs of length 0, 1, 2, 3, 4, 5 bytes (boundary conditions).
- Run with inputs that are exactly 2048 bytes (TOAST threshold).
- Run with inputs where the last 3 bytes form a potential match.

### Step 5: Benchmark and commit message preparation

1. Run the full benchmark suite on the final combined result:
   - Tomas's test: random/compressible data at 1K, 4K, 1M into unlogged table.
   - Andrey's test: `REINDEX table pgbench_accounts` (scale 100) with `wal_compression = on`.
   - TPC-C workload with `wal_compression = on` if feasible.
   - TOAST-heavy workload: insert 1M rows of compressible text into a text column.
2. Measure on both x86-64 and ARM64 if possible.
3. Prepare the commitfest submission with a clear summary email to pgsql-hackers.

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
4. Benchmark shows ≥15% speedup on compressible data (TOAST-heavy workload).
5. Benchmark shows ≥10% speedup on WAL compression workload.
6. No measurable regression on random (incompressible) data.
7. Compression ratio change is ≤1 byte on standard test corpus.
8. Each commit is independently reviewable and committable.

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
