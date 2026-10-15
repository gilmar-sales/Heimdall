# Linux Memory Profiling for a C++ LSP

## Which process-level measurements best describe memory, and how should peaks be recorded?

### Takeaway
Use `/proc/<pid>/smaps_rollup` for periodic RSS/PSS/private-memory snapshots, and distinguish those snapshots from allocation totals and high-water marks. RSS from `/proc/<pid>/status` is convenient but documented as inaccurate; capture an external high-frequency series or cgroup peak when a transient peak matters.

### Cited Findings
- `smaps_rollup` provides summed mapping fields, including RSS, PSS, private clean/dirty, and `Pss_Anon`, `Pss_File`, `Pss_Shmem`; kernel docs describe it as faster and more convenient than scanning all `smaps`, while `smaps` is higher cost. [Linux kernel procfs documentation](https://docs.kernel.org/filesystems/proc.html)
- PSS divides each resident shared page by the number of processes sharing it. RSS counts full mapped resident pages, so summed RSS across processes can double-count shared libraries; PSS is generally the more useful process share measure. [Linux kernel procfs documentation](https://docs.kernel.org/filesystems/proc.html)
- USS is not a named rollup metric; approximate it as `Private_Clean + Private_Dirty` from smaps rollup. Private pages are not necessarily identical to “memory the process could immediately return,” so treat USS as a practical private-resident measure, not allocator-live bytes. [Linux kernel procfs documentation](https://docs.kernel.org/filesystems/proc.html)
- `VmHWM` is peak RSS and `VmRSS` is current RSS, but both are explicitly documented as inaccurate. `VmRSS` is the sum of `RssAnon`, `RssFile`, and `RssShmem`. [proc_pid_status(5)](https://man7.org/linux/man-pages/man5/proc_pid_status.5.html)
- RSS accounting is asynchronous and can be imprecise; kernel docs say scanning `smaps` can provide a precise snapshot but is slow. `/proc/<pid>/smaps` and `maps` reads can race with changing mappings. [Linux kernel procfs documentation](https://docs.kernel.org/filesystems/proc.html)
- The kernel permits writing `5` to `/proc/<pid>/clear_refs` to reset the peak RSS high-water mark to the current value. [Linux kernel procfs documentation](https://docs.kernel.org/filesystems/proc.html)

### Inferences
- For repeatable LSP runs, record timestamped `smaps_rollup` at a modest cadence (e.g. 100–500 ms) and retain `VmHWM` as a cross-check; polling more frequently costs more and still can miss a short transient.
- Report both PSS and RSS: PSS helps compare actual share when libraries are shared, while RSS remains a common operational metric. Report private clean+dirty as USS-like, and include units and kernel version.
- `getrusage(RUSAGE_SELF).ru_maxrss` or `/usr/bin/time -v` can provide a convenient run-level maximum RSS, but neither supplies PSS/USS or allocation-site attribution. Compare their semantics/platform units before combining datasets.

### Gaps
- No single cited upstream source establishes one universally best sampling interval for short-lived LSP spikes; choose interval based on expected spike duration and observer cost.
- Cgroup v2 `memory.peak` is useful when the LSP is isolated in its own cgroup, but peak semantics/reset behavior and availability should be checked against the host kernel documentation and deployment setup.

## Which heap profilers attribute allocations to C++ code, and what are their tradeoffs?

### Takeaway
Start with Heaptrack for practical whole-process malloc/new allocation attribution and Massif when page-level or stack accounting is required. For repeatable allocator-specific snapshots, jemalloc profiling or gperftools/tcmalloc can work, but both replace/change the allocator and therefore alter the measurement subject.

### Cited Findings
- Heaptrack records allocation/deallocation events with stack traces; its reports expose peak memory consumers, allocation call hotspots, and temporary allocations. It can start a command or attach to a running PID, and profiles can be analyzed later with CLI or GUI. [KDE Heaptrack documentation](https://github.com/KDE/heaptrack)
- Heaptrack says it imposes substantially lower time and memory overhead than Massif in its comparison, and principally charges overhead on allocation activity rather than CPU-only work. This is the project’s qualitative comparison, not a universal overhead guarantee. [KDE Heaptrack documentation](https://github.com/KDE/heaptrack)
- Heaptrack instruments normal allocation functions; Massif’s advantage is that `--pages-as-heap=yes` can attribute low-level `mmap`/`brk` page allocations and `--stacks=yes` can include stacks. Heaptrack cannot efficiently profile stack allocations; custom pools bypassing malloc/new may need explicit annotations. [KDE Heaptrack documentation](https://github.com/KDE/heaptrack); [Valgrind Massif manual](https://valgrind.org/docs/manual/ms-manual.html)
- Massif provides allocation trees at snapshots, including the heap peak. It runs the program slowly under Valgrind; stack profiling slows it greatly. By default it excludes direct low-level mmap/brk usage, and peak detection is normally within configured 1% tolerance and is taken after deallocation events. [Valgrind Massif manual](https://valgrind.org/docs/manual/ms-manual.html)
- jemalloc exposes allocator stats and sampling-based profiling through `mallctl`; profiling requires a build configured with `--enable-prof`. Default sample interval is 512 KiB of allocation activity. Profiles are consumable by `jeprof`/pprof; thread caches and stats refresh can make live statistics not fully current. [jemalloc manual](https://jemalloc.net/jemalloc.3.html)
- jemalloc’s arenas and thread-specific caches trade synchronization for fragmentation and memory use; its manual notes caches can retain objects and increase memory consumption. Its allocator-level resident metric includes metadata and unused dirty pages, and is not the same as process RSS/PSS. [jemalloc manual](https://jemalloc.net/jemalloc.3.html)
- gperftools heap profiling is coupled to tcmalloc: using it requires `-ltcmalloc` or preload and changes the allocator. It records allocation-site stacks and supports periodic dumps by allocation volume, in-use high-water increments, time, or signal. [gperftools Heap Profiler manual](https://gperftools.github.io/gperftools/heapprofile.html)
- Enabling gperftools `HEAP_PROFILE_MMAP` includes mmap/mremap/sbrk but also captures tcmalloc’s own internal mappings, which may need filtering. Profiles can be compared with a baseline and pprof can show live bytes or cumulative allocated bytes. [gperftools Heap Profiler manual](https://gperftools.github.io/gperftools/heapprofile.html)
- Memray is primarily a Python profiler; it tracks native C/C++ calls in Python processes, but it is not a general standalone C++ executable profiler. [Memray overview](https://bloomberg.github.io/memray/overview.html)
- glibc `malloc_info(0, stream)` exports XML state for all malloc arenas; it is useful for allocator-state diagnostics but has no allocation-site stack attribution. `malloc_stats()` prints a smaller summary to stderr. [malloc_info(3)](https://man7.org/linux/man-pages/man3/malloc_info.3.html); [malloc_stats(3)](https://man7.org/linux/man-pages/man3/malloc_stats.3.html)

### Inferences
- Use Heaptrack first for source attribution without changing the allocator; preserve debug symbols (and usable unwind information) in the exact binary. Its own recorder and log add overhead/memory, so never treat profiled RSS as the baseline.
- Use Massif on a reduced reproducible workspace if suspecting custom mmap-backed arenas, stack use, or allocation-accounting gaps. `--pages-as-heap=yes` answers a different, lower-level question and can make stacks less intuitive.
- Use jemalloc sampling or tcmalloc snapshots when repeated in-process dumps (e.g. immediately before/after indexing) are needed. Results describe that allocator’s behavior, not glibc’s; validate important conclusions against an unprofiled process-level run.
- `malloc_info` or allocator stats can help explain a gap between live heap bytes and resident pages (arenas, cached/free chunks, fragmentation), but cannot by themselves identify which project call sites own those bytes.

### Gaps
- Upstream tool docs do not provide a portable numeric overhead percentage for Heaptrack, Massif, jemalloc profiling, or gperftools across project workloads. Measure overhead on the target binary/workspace.
- Direct `mmap` use, custom allocators, GPU/shared memory, stacks, and kernel-accounted memory can make allocator heap totals differ substantially from RSS/PSS; no single heap profiler covers every category equally.

## What repeatable workflow isolates LSP startup from workspace indexing and attributes growth to code?

### Takeaway
Make the language-server workload deterministic and phase-labelled, collect an unprofiled process-memory timeline first, then run an attribution profiler against the same exact scenario. Compare startup baseline, post-initialization, post-index, and steady-state snapshots rather than one end-of-run number.

### Cited Findings
- Heaptrack supports command launch and attach; Massif has allocation snapshots and stack traces; tcmalloc and jemalloc support configurable/explicit interval or high-water profile dumps. These enable collecting distinct phase snapshots when the process can be held at a phase boundary. [KDE Heaptrack documentation](https://github.com/KDE/heaptrack); [Valgrind Massif manual](https://valgrind.org/docs/manual/ms-manual.html); [gperftools Heap Profiler manual](https://gperftools.github.io/gperftools/heapprofile.html); [jemalloc manual](https://jemalloc.net/jemalloc.3.html)
- gperftools documents comparing two heap profiles by subtracting a baseline profile, including an example that excludes program initialization to examine later growth. [gperftools Heap Profiler manual](https://gperftools.github.io/gperftools/heapprofile.html)
- Linux exposes per-mapping RSS/PSS/private and anonymous/file/shmem splits in smaps rollup, supporting process-level correlation with allocator profiles. [Linux kernel procfs documentation](https://docs.kernel.org/filesystems/proc.html)

### Inferences
- **Scenario control:** Pin binary/build, compiler, flags, environment, kernel/tool versions, and workspace fixture. Disable unrelated background indexing/network access; use a clean cache for cold-start and a separately documented warm-cache scenario. Feed identical LSP JSON-RPC messages and wait for a deterministic readiness/idle condition.
- **Phases:** Start server and record `t=0`; sample during executable/library startup and LSP initialize; capture “initialized but before workspace load”; send `workspace/didChangeWorkspaceFolders` or open a fixed workspace; wait for indexing completion/idle; capture post-index; then hold idle and capture later steady state. Use server logs/test harness markers to correlate timestamps.
- **Baseline run:** Sample `/proc/$pid/smaps_rollup` for `Rss`, `Pss`, `Private_Clean`, `Private_Dirty`, `Anonymous`, `Pss_Anon`, `Pss_File`; sample `status` for VmHWM and thread count. Record peak and endpoint deltas. Keep this uninstrumented run as the trustworthy footprint reference.
- **Attribution run:** launch Heaptrack from process start to retain constructor/startup allocations, and use the same input protocol. If attaching after startup, note that prior allocations are absent from history though still appear as live memory if the profiler supports it. Label capture start/stop or use allocator profile dumps at the same phase boundaries.
- **Interpretation:** Compare live-at-phase allocations (retained ownership) separately from cumulative allocations (churn). In heaptrack inspect peak consumers and temporary allocations; for tcmalloc/pprof compare `--inuse_space` and `--alloc_space`; use Massif’s peak tree for its measured heap/page category. Symbolize to source using matching binary/debug files.
- **Causal triangulation:** If PSS/USS grows while heap-live bytes do not, inspect mmap pools, stacks, file mappings, allocator retention/fragmentation, and shared memory rather than assuming a leak. If heap-live rises across post-index/idle snapshots, compare stack-attributed live allocations and repeat with a second fresh process.
- Repeat each scenario several times and report median plus range; keep profiler overhead separate, and avoid claiming absolute RSS from an instrumented heap run. Compare like-for-like workspace/cache state.

### Gaps
- No cited upstream LSP-specific protocol defines a universal “index complete” signal; each server needs a deterministic readiness/idle marker, test harness, or protocol-specific progress notification.
- Heap profiles do not necessarily explain RSS/PSS: allocator-retained free pages and non-heap mappings create legitimate divergence. Correlating per-phase allocator snapshots with kernel snapshots is necessary, but not an exact byte-for-byte reconciliation.
