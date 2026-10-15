# Profile Heimdall’s Linux LSP effectively

The most reliable approach is to **measure the server from launch, not attach late**, and to separate protocol startup from the asynchronous workspace/indexing workload. The existing observations are encouraging but answer different questions: the client received `initialize` in **27.64 ms**, while a 25-second workload reached roughly **432 MiB sampled RSS** and **2.44 GiB VmSize**. The former is a fast protocol milestone, not proof that startup/indexing is complete; VmSize is virtual address space, not resident memory. A working `perf stat` on a simple launched command shows perf can work in at least that context, but does not establish that attach to this LSP PID is permitted or correctly targeted.

## Launch-under-perf avoids the likely attach trap

Use `perf stat -- command` or `perf record -- command` around the client/harness that launches the actual `build/lsp/heimdall-lsp` as a child. This captures process startup and avoids PID races and the attach path; it does **not** bypass perf permissions, since the caller still opens the events. A simple-command `perf stat` success alongside blank attach output points first to attach-specific scope/permissions, a wrong or short-lived PID, or output being awaited before the attached process exits—not to a general failure of the perf subsystem. `perf stat -p PID` commonly reports its aggregate when measurement ends, so blank output while the server is still alive is not by itself proof that no counters were collected. Check exit status and stderr, target PID/lifetime, and run a short controlled attach to a process that remains alive before concluding. Linux documents process-scoped permission checks and the role of `CAP_PERFMON`; `perf_event_paranoid`, kernel version, seccomp/container policy, and LSM policy can all matter. ([perf-stat(1)](https://man7.org/linux/man-pages/man1/perf-stat.1.html); [perf_event_open(2)](https://man7.org/linux/man-pages/man2/perf_event_open.2.html); [kernel perf security guide](https://docs.kernel.org/admin-guide/perf-security.html))

Start with read-only checks and a low-risk software event:

```sh
uname -a
perf --version
cat /proc/sys/kernel/perf_event_paranoid
grep -E '^(Cap(Inh|Prm|Eff|Bnd|Amb)|NoNewPrivs|Seccomp):' /proc/self/status
getcap "$(command -v perf)" 2>/dev/null || true
perf stat -v -e task-clock -- sleep 1
perf stat -v -e cycles:u -- sleep 1
```

Then run the protocol harness under perf, substituting its real invocation:

```sh
perf stat -r 10 -e task-clock,cycles,instructions,branches,branch-misses \
  -- ./lsp_test_client --server ./build/lsp/heimdall-lsp --scenario initialize
```

Use `perf stat --null -r 10 -- <harness>` as a no-counter wrapper baseline. Keep the harness's own monotonic timestamps: `perf stat` elapsed time includes harness setup and shutdown and is not initialize latency. For sampled call stacks, preserve the exact Release executable/build IDs and usable debug/unwind information, then try:

```sh
perf record -F 99 -g --call-graph dwarf -o /tmp/heimdall-lsp.perf.data \
  -- ./lsp_test_client --server ./build/lsp/heimdall-lsp --scenario initialize
perf report -i /tmp/heimdall-lsp.perf.data
```

If stacks are absent, verify local perf's unwind support and binary unwind metadata; frame-pointer mode is only trustworthy when frame pointers are present. `perf record` supports `fp`, `dwarf`, and hardware-dependent `lbr` modes. ([perf-record(1)](https://man7.org/linux/man-pages/man1/perf-record.1.html))

Do not make `sudo sysctl -w kernel.perf_event_paranoid=-1` the first fix. Relaxing this system-wide setting broadens observability access and can expose execution context/address information; it is a security-policy change, not a harmless debugging toggle. If diagnostics prove a permission barrier and profiling is approved, ask the host administrator for the narrowest temporary access (prefer `CAP_PERFMON` where supported), record the original policy, and restore it. Avoid adding broad capabilities or changing shared-host policy casually. ([kernel perf security guide](https://docs.kernel.org/admin-guide/perf-security.html))

## Treat initialize, indexing, and first request as different timings

The LSP protocol makes `initialize` the first request; the client waits for its result before sending `initialized`. Heimdall responds to initialize after loading options and advertising capabilities, then its `initialized` handling can start workspace-scan and symbol-index workers. Thus the observed **27.64 ms initialize response** does not measure the background work or the first useful completion. ([LSP 3.17 specification](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/); [Heimdall `Server.cpp`](https://github.com/gilmar/Heimdall/blob/main/lsp/Server.cpp); [workspace scan smoke test](https://github.com/gilmar/Heimdall/blob/main/test/LspWorkspaceScanSmoke.py))

Instrument a deterministic client with monotonic timestamps for process spawn, complete initialize response, `initialized` write, scan/index completion or a documented idle marker, and first real request/response (for example fixed `didOpen` then completion). Drain and parse framed JSON-RPC correctly because notifications can interleave with responses; keep diagnostics off stdout. Run separate immediate-request and post-index-request cases. Repeat on the same workspace and report median and range, noting whether filesystem cache is warm. For phase-specific perf data, launch the harness under `perf record -D -1` and use perf's FIFO control to enable immediately before initialize and disable after its response; capture indexing as a separate window. Do not use a guessed delay when the harness can mark protocol boundaries. ([perf-record(1)](https://man7.org/linux/man-pages/man1/perf-record.1.html); [Heimdall LSP latency benchmark](https://github.com/gilmar/Heimdall/blob/main/bench/src/LspLatencyBench.cpp))

If wall time is long but on-CPU samples are sparse, investigate blocking and scheduling rather than treating a CPU flame graph as a timeline. Run syscall tracing for a diagnostic pass and correlate timestamps with the protocol milestones:

```sh
strace -ff -ttt -T -yy -o /tmp/heimdall-lsp.trace \
  ./lsp_test_client --server ./build/lsp/heimdall-lsp --scenario initialize-and-index
```

Look for slow filesystem enumeration/open/read, mappings, futex waits, and polling; tracing perturbs timing, so use it to explain behavior, not as the latency baseline. If contention is suspected, use `perf sched record` on the controlled workload and inspect `perf sched timehist` for runnable-to-running delay versus blocked time. ([strace(1)](https://man7.org/linux/man-pages/man1/strace.1.html); [perf sched(1)](https://www.man7.org/linux/man-pages/man1/perf-sched.1.html))

## Separate resident footprint from allocation hotspots

The reported **~432 MiB sampled RSS** is a useful process-footprint observation, but its peak precision depends on polling cadence; **~2.44 GiB VmSize is not 2.44 GiB of RAM use**. Track timestamped `/proc/$pid/smaps_rollup` snapshots through startup, post-initialize, post-index, and idle. Record `Rss`, `Pss`, `Private_Clean`, `Private_Dirty`, `Anonymous`, `Pss_Anon`, and `Pss_File`; private clean plus dirty is a useful USS-like approximation, not allocator-live bytes. PSS apportions shared resident pages, while RSS counts each mapped shared page in full. `/proc/$pid/status` `VmHWM` is a convenient peak cross-check but Linux documents RSS fields there as inaccurate. Use a modest sampling cadence such as 100–500 ms, retain timestamps, and note that very brief peaks can still be missed. ([Linux procfs documentation](https://docs.kernel.org/filesystems/proc.html); [proc_pid_status(5)](https://man7.org/linux/man-pages/man5/proc_pid_status.5.html))

For source-level allocation attribution, run Heaptrack from process launch on the same protocol scenario and workspace; it records allocation/deallocation stacks and can identify retained and temporary allocation hotspots. Keep its profile separate from baseline RSS because instrumentation adds overhead. If Heaptrack's malloc/new view does not explain resident growth, investigate allocator retention, custom pools, direct `mmap`, stacks, and file mappings; Massif with `--pages-as-heap=yes` can answer a lower-level mapping question but is much slower. Avoid switching to jemalloc or tcmalloc as the first experiment: both change allocator behavior and therefore the object being measured. ([Heaptrack](https://github.com/KDE/heaptrack); [Valgrind Massif manual](https://valgrind.org/docs/manual/ms-manual.html))

## Recommended investigation sequence

First, preserve a trustworthy unprofiled baseline for the exact Release Ninja binary and representative checkout: record revision, compiler/options, kernel/tool versions, workspace and compile database, scan/index configuration, and warm-cache status. Capture protocol milestones and a smaps-rollup timeline over repeated runs. Second, run `perf stat` by launching the protocol client under perf; use task-clock and available hardware events to compare CPU cost across phases or one-factor changes. Third, record a narrow phase-specific stack profile for the expensive interval, then use Heaptrack if the RSS/PSS growth needs allocation-site attribution. Use strace or scheduler profiling only when timings indicate blocking/contention. Validate each proposed optimization in unprofiled repetitions and functional tests; keep profiling overhead, workload, and cache conditions explicit.

The notes do not establish what caused the prior blank attach output, nor whether the 25-second RSS sample included indexing, exact sampling cadence, or a representative workspace. Do not interpret the measurements as a leak or as evidence that initialize is slow. The next decisive result is a protocol-phased memory timeline paired with launch-under-perf task-clock/profile data; that will show whether the opportunity is CPU work, I/O/wait time, or retained resident memory before code changes are attempted.
