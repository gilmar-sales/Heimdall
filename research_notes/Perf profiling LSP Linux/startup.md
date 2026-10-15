# Diagnosing startup stalls in a C++ LSP over stdio

## How to separate the startup phases

### Takeaway
Measure startup as externally observable milestones rather than one “server startup” duration: process exec, initialize response, initialized/background work, then a first real document request. This separation distinguishes launch/loader cost, protocol initialization, post-initialize indexing, and request-path latency.

### Cited Findings
- LSP requires `initialize` as the first request; the client must wait for its `InitializeResult` before sending further requests or notifications, then sends `initialized` after initialization. [LSP 3.17 specification](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/)
- Heimdall's `lsp/Server.cpp` responds to `initialize` after loading initialization options and advertising capabilities; on `initialized`, it starts the optional workspace-scan worker and symbol-index worker. Its own workspace scan smoke test waits for “workspace scan finished.” [Heimdall Server.cpp](https://github.com/gilmar/Heimdall/blob/main/lsp/Server.cpp); [LspWorkspaceScanSmoke.py](https://github.com/gilmar/Heimdall/blob/main/test/LspWorkspaceScanSmoke.py)
- Heimdall has an end-to-end latency benchmark that starts the actual LSP binary and measures `didChange` followed by completion response, illustrating that a feature-path metric differs from initialize latency. [LspLatencyBench.cpp](https://github.com/gilmar/Heimdall/blob/main/bench/src/LspLatencyBench.cpp)

### Inferences
- Record timestamps on a monotonic clock at harness-side process spawn, complete initialize response, initialized notification write, first scan/index completion notification (if any), and first request response. Report each interval, not only total elapsed.
- Use a protocol-aware harness with framed JSON-RPC over stdin/stdout. Send initialize, await its response, send initialized, then issue an explicit real request such as didOpen followed by completion/hover. Read and preserve asynchronous notifications while matching responses by JSON-RPC id; notifications may interleave with responses.
- To measure the request before/after workspace scan, create separate runs: one issuing a request immediately after initialized and one waiting for scan completion. Do not assume that scan completion is a prerequisite for every request.
- Distinguish process-launch wall time (parent spawn to child ready / first protocol response) from server CPU time. `exec` itself may be fast while dynamic loader work, configuration reads, or waiting on filesystem dominate.

### Gaps
- The exact default and CLI controls for Heimdall workspace scan and symbol indexing should be confirmed against the tested binary/version and its configuration; this note does not assume scan is always enabled.

## Practical staged profiling protocol

### Takeaway
Start with repeatable wall-clock milestones and syscall tracing; only then capture CPU stacks and scheduler events around the phase that is actually slow. Use the same real workspace and binary throughout, and keep tracing overhead separate from baseline latency runs.

### Cited Findings
- `strace -f` follows child processes/threads, `-ttt` provides epoch timestamps, and `-T` reports syscall duration; syscall traces can expose time in filesystem calls, reads, writes, polling, and process creation. [strace(1)](https://man7.org/linux/man-pages/man1/strace.1.html)
- `perf record` supports sampled profiles and call graphs. Its manual documents `fp`, `dwarf`, and `lbr` unwind modes; frame-pointer unwinding can produce bogus call graphs when binaries omit frame pointers, while DWARF unwinding needs suitable perf support. [perf-record(1)](https://man7.org/linux/man-pages/man1/perf-record.1.html)
- `perf sched record` captures scheduler events; `perf sched timehist` reports wait time, scheduling delay (runnable-to-running), and runtime. [perf-sched(1)](https://www.man7.org/linux/man-pages/man1/perf-sched.1.html)
- Brendan Gregg's FlameGraph project documents converting `perf script` samples using `stackcollapse-perf.pl` and `flamegraph.pl`; the graph visualizes sampled stack aggregation, not chronological phase duration. [FlameGraph README](https://github.com/brendangregg/FlameGraph); [CPU Flame Graphs](https://www.brendangregg.com/FlameGraphs/cpuflamegraphs.html)

### Inferences
1. **Establish a baseline:** Build/use the intended optimized binary; capture build revision, compiler/build mode, command line, workspace path, kernel, CPU, available memory, and whether workspace scan/index options are on. Run the protocol harness repeatedly without profilers; report median and tail percentiles with sample count.
2. **Process and syscall phase:** Run the exact executable under `strace -ff -ttt -T -yy -o /tmp/lsp.trace -- <server>` (or attach to the child if the harness launches it). Correlate timestamps with protocol milestone logs. Inspect long `openat/statx/getdents64/read/mmap/futex/poll` calls and syscall counts. `strace` perturbs timing, so use it to explain behavior, not as the headline latency number.
3. **CPU-hot phase:** Capture only the relevant interval if possible, with `perf record -F 99 -g --call-graph dwarf -p PID` (or launch the controlled harness under perf); inspect `perf report`, then optionally generate a flame graph. Ensure symbols/debug info and usable unwind support. CPU sampling answers where on-CPU time goes; it will not by itself explain time blocked on I/O or runnable-but-not-scheduled.
4. **Scheduling contention:** If wall time is high but CPU samples are sparse, capture `perf sched record -- <controlled command>` and inspect `perf sched timehist`. High wait-for-scheduling delay suggests contention/oversubscription; sleeping/wait time should be investigated against syscall and application phase logs.
5. **Add low-overhead application instrumentation:** Emit monotonic duration events to stderr or a dedicated trace file around initialization-option/config reads, compile database load, scan directory enumeration, per-file work, indexing, and request handlers. Never write diagnostic text to stdout, which is the LSP transport. Keep event names and identifiers stable enough for comparison.
6. **Validate a hypothesis:** Change one factor at a time (for example scan enabled/disabled, compile DB present/absent, workspace size), repeat baseline and targeted profile, then confirm improvement in unprofiled end-to-end milestone timings and relevant functional smoke tests.

### Gaps
- No single command is guaranteed to work across all kernel/perf versions, event permissions, and hardware; check local `perf record --help`, `perf list`, `perf_event_paranoid`, and unwind support before interpreting missing data.

## Cache conditions, workspace representativeness, and pitfalls

### Takeaway
Report warm-cache and cold-cache results as different experiments. Use a representative checkout with its real directory structure and compile database, and document cache manipulation and external system load rather than presenting a single “startup” number.

### Cited Findings
- Linux documents `/proc/sys/vm/drop_caches` as dropping clean page cache and reclaimable dentries/inodes; it can be useful for reproducible filesystem benchmarks but discards caching benefits and may degrade system performance. Dirty objects are not freed; `sync` is recommended first. [proc_sys_vm(5)](https://man7.org/linux/man-pages/man5/proc_sys_vm.5.html)
- Heimdall's workspace scan smoke test deliberately includes source, build, dependency, and third-party directories to validate exclusions; this demonstrates that directory composition affects actual scan behavior and should be represented in performance fixtures. [LspWorkspaceScanSmoke.py](https://github.com/gilmar/Heimdall/blob/main/test/LspWorkspaceScanSmoke.py)

### Inferences
- **Warm runs:** Repeat identical startup on the same checkout without clearing caches. Name this “warm filesystem cache”; process-local caches still reset if every trial spawns a fresh process.
- **Cold runs:** Prefer an isolated test machine/VM. If privileged cache dropping is available, run `sync` then `echo 3 > /proc/sys/vm/drop_caches` immediately before each cold trial and document it. This is global-impacting and should not be done casually on shared/production systems. “Cold” here means kernel clean page/dentry/inode caches were requested to be dropped, not guaranteed cold hardware/remote filesystem/controller caches.
- Use the same immutable workspace snapshot for all trials. Include a realistic `compile_commands.json`, headers/dependencies, generated/build trees, and ignored/vendor directories as present in the target project; record file count, total bytes, and directory count. Also retain a small controlled fixture for diagnosis, but do not substitute it for a real-workspace result.
- Keep the workspace on the same filesystem/storage class as actual use (local SSD vs network mount can dominate). Record CPU frequency/power policy and avoid concurrent builds or indexing jobs. Run enough repetitions to show variance; randomize or alternate configurations to reduce drift.
- The LSP process should be freshly spawned for process-start measurements. Clearly state whether each run includes editor-side spawn, protocol exchange, scan completion, and first request. Use identical request payloads and await the correlated response.
- Common pitfalls: confusing stdout logs with valid Content-Length-framed JSON-RPC; blocking the harness by not draining stderr/stdout; timing only the initialize response while the complaint concerns scan or first completion; treating flamegraph width as elapsed timeline; profiling with heavy tracing and reporting that latency as baseline; using unsymbolized/stripped builds; neglecting perf permissions or missing unwind support; claiming cold cache from one read without controlling cache state.

### Gaps
- The repository smoke test is intentionally tiny, not a representative performance corpus. Select a real user workspace (or a documented public large C++ project plus compile database) and avoid inventing a universal “representative” file count.
