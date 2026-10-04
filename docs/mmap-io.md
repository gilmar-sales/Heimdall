# Memory-mapped I/O: `mio`, best practices, and what to measure

How file input should work in this project, and the honest comparison still
missing between `mmap` and plain `read()`. All `mio` behaviors below were
verified against the current `vimpunk/mio` source, including the empty-file
case.

## Current state

- `core/include/Heimdall/MappedBuffer.hpp` is a hand-rolled mapping:
  `mmap(PROT_READ, MAP_PRIVATE)` on POSIX, `MapViewOfFile(FILE_MAP_READ)` on
  Windows, with a buffered-read fallback and an explicit empty-file guard
  (`Open` returns an empty buffer for 0-byte files). The fd/mapping lifetime
  is tied to the object; `view()` must not outlive it.
- Tokens and nodes already store byte `offset`/`length`, not pointers, so trees
  do not pin mapping addresses (§7 below holds).
- `bench/src/MappedBufferBench.cpp` (`BM_MappedBufferOpen`) measures open
  throughput over `bench/corpus`, but compares against nothing.
- `src/Reporting.cpp` writes `--write`/`--fix` output with a truncating
  `ofstream` directly over the original — no atomic rename (see §8).

## Policy constraint: `mio` cannot enter `heimdall_core`

`test/src/CorePolicySpec.cpp` forbids third-party includes in `core/`; only
the standard library and the platform SDK may appear there (which is why the
hand-rolled `mmap`/`MapViewOfFile` in `MappedBuffer.cpp` is legal, but `mio`
would not be). Consequences:

- A `mio` backend behind the `MappedBuffer` interface would have to live
  **outside** `heimdall_core` (CLI/pipeline layer or a new `io/` module), or
  the policy must be amended deliberately.
- A `mio`-vs-`read()` comparison benchmark has no such problem: `bench/`
  already pulls third parties via `FetchContent` (Google Benchmark), so `mio`
  can be fetched there with zero policy impact. That is where the future
  measurement (§10) belongs.

## Verified `mio` behaviors that change the practices

- The file stays open while the mapping exists: with a path-based mapping,
  `mio` keeps the descriptor and only `close`s on `unmap()`.
- `mio` calls neither `madvise` nor uses `MAP_POPULATE`: plain `mmap` with
  `MAP_SHARED`.
- Empty files error out: `make_mmap_source` on a 0-byte file returns `EINVAL`
  ("Invalid argument").

## Best practices (status per item)

1. **Use `mio::mmap_source` with `std::error_code`.** Read-only, no
   exceptions, explicit error handling in the driver loop. Avoid
   `shared_mmap_source` unless several owners need the mapping — it pays for
   an atomic `shared_ptr` counter. *Status: n/a (no `mio` in tree).*
2. **Handle empty files before mapping.** Check size first, use an empty
   `string_view` on zero. *Status: done (`MappedBuffer::Open` returns empty
   on `st_size == 0` / `QuadPart == 0`).*
3. **Map → process → unmap, one file at a time per thread.** Never map the
   whole project at once. Each pool thread (`Pipeline`'s `std::jthread`
   workers) follows map → lex → parse → lint/format → unmap and resets its
   arena, so live mappings ≤ thread count — bounding both memory and
   descriptors, since `mio` (like our `MappedBuffer`) holds each file open.
   *Status: matches current pipeline shape; keep it if the backend changes.*
4. **To close the descriptor early, map from your own handle.** On POSIX the
   mapping survives `close(fd)`. With `mio` constructed from your handle it
   does not close for you — close it yourself right after mapping.
   *Status: our `MappedBuffer` currently holds the fd until `Release`;
   adopt this only if fd pressure shows up in profiles.*
5. **Do `madvise` yourself.** With offset 0, `data()` is page-aligned:
   `madvise(ptr, size, MADV_SEQUENTIAL)` after mapping (Windows:
   `PrefetchVirtualMemory`). `MAP_POPULATE` (fault pages in up front) is not
   exposed by `mio` — that needs raw `mmap`. *Status: todo; nobody calls
   `madvise` today.*
6. **Lexer without sentinel.** A read-only mapping has no trailing `\0`.
   Fast path while `ptr + 64 <= end` with no bounds checks (SIMD-friendly),
   slow path with checks for the tail bytes. Alternative hybrid: small files
   (a few KB, threshold to be measured) go through `read()` into a padded
   buffer, only large files use `mio`. *Status: the lexer is bounds-checked
   throughout; the hybrid threshold is undecided — feed it from §10.*
7. **View lifetimes: offsets, not pointers.** If tokens/nodes point at
   `m.data()`, the mapping must outlive the tree. Prefer `uint32_t` offsets:
   half the space, no tie to the mapping address. *Status: holds
   (`Token{kind, offset, length}`; `MappedBuffer` documents the view
   lifetime). `uint32_t` vs `size_t` is an open size optimization.*
8. **Never write to the file you have mapped.** `mio` uses `MAP_SHARED`, so
   an external write changes what you read and a truncation can `SIGBUS` you.
   (Our hand-rolled mapping uses `MAP_PRIVATE`, which does not have this
   hazard — one more behavioral difference to preserve in any migration.)
   For `--write`/`--fix`: render to a buffer, write a temp file, `rename()`
   atomically over the original *after* unmapping. Never `mmap_sink` onto the
   target. *Status: gap — `Reporting.cpp` trunc-writes in place; atomic
   rename is still todo.*
9. **Measure memory the right way.** File-backed pages are clean page cache:
   they inflate RSS but the kernel can drop them, and `heaptrack`/`massif`
   do not see them. Separate your heap from mapped files via
   `/proc/self/smaps` (`Anonymous` vs `Rss` per region) or `perf`.

## 10. Future measurement: `mmap` vs `read()` (not yet done)

Unmeasured to date is whether `mio` beats a reused-buffer `read()` on our
corpus at all — it depends on file sizes. When someone runs it:

- **Where:** `bench/`, fetching `mio` with `FetchContent`
  (`https://github.com/vimpunk/mio.git`), beside the existing
  `BM_MappedBufferOpen`. No core changes needed.
- **What:** `BM_MmapMioOpen` (make/unmap per file) vs `BM_ReadBufferedOpen`
  (`read()` into a thread-local reused buffer) vs the current
  `BM_MappedBufferOpen`, over (a) `bench/corpus` and (b) a large-file corpus
  (e.g. an LLVM checkout — cold page cache via `drop_caches`, report
  min-time as well as median, `SetBytesProcessed`, plus RSS from `smaps`).
- **Decides:** whether to adopt `mio` outside core (§policy), which files
  take the hybrid path (§6: find the size threshold), and whether `madvise`
  (§5) moves the needle. If `read()` wins everywhere, close this out and
  keep the hand-rolled mapping.
