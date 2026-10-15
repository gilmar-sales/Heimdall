# Linux perf profiling of an LSP subprocess

## Measuring a subprocess without attaching

### Takeaway
Prefer launching the server under `perf` rather than attaching to its PID. `perf record -- command` and `perf stat -- command` create measurement for the child as it starts, so they avoid the attach/ptrace path and capture early startup. This does not bypass kernel perf permissions: the `perf_event_open` calls still run under the caller's credentials and remain subject to `perf_event_paranoid`, capabilities, seccomp, and container policy.

### Cited Findings
- `perf record` explicitly runs a command and records its performance-counter profile; `perf stat` similarly runs a command and gathers counters and elapsed/user/system time. — [perf-record(1)](https://man7.org/linux/man-pages/man1/perf-record.1.html); [perf-stat(1)](https://man7.org/linux/man-pages/man1/perf-stat.1.html)
- For process-scoped `perf_event_open`, `pid > 0` targets a process/thread and Linux 5.9+ uses CAP_PERFMON permission checks; older kernels also used a ptrace access check. This means launch-under-perf is not a general permission workaround, but avoids trying to attach after startup and losing early events. — [perf_event_open(2)](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)
- `perf stat --null -r 10 -- <command>` measures elapsed time without enabling counters; useful to establish a repeatable baseline and approximate wrapper overhead. — [perf-stat(1)](https://man7.org/linux/man-pages/man1/perf-stat.1.html)
- Safe initial diagnostics (read-only):

  ```sh
  uname -a
  perf --version
  cat /proc/sys/kernel/perf_event_paranoid
  grep -E '^(Cap(Inh|Prm|Eff|Bnd|Amb)|NoNewPrivs|Seccomp):' /proc/self/status
  getcap "$(command -v perf)" 2>/dev/null || true
  perf list | head -40
  perf stat -v -- sleep 0.1
  ```

- For LSP initialize timing, run a deterministic harness/client that starts the server, sends `initialize`, waits for the response, then sends `shutdown`/`exit`. Wrap that whole harness invocation with `perf stat`; record the harness's own monotonic timestamps for process-spawn-to-response and request-send-to-response. The perf elapsed number includes harness startup/teardown, so do not call it pure server initialize latency. Repeat runs and report median/range; keep workspace, compile database, caches, CPU affinity, and input identical.

### Inferences
- Since attach errors are commonly confused with ptrace errors, first classify the failure: launch-under-perf with a simple command tests the perf subsystem independently of the LSP; then use the same launch strategy around the actual client/server workload.
- If ordinary hardware events fail, retry a low-risk software event (for example `task-clock`) and inspect `perf stat -v` errors. Event availability and permissions are separate questions; hardware counters may be unavailable even when software events work.

### Gaps
- Fedora-specific effective security policy in a particular host/container cannot be diagnosed from the generic documentation; inspect local sysctl, capabilities, SELinux/audit records, and container seccomp profile.

## Permissions, paranoia, and child/fork coverage

### Takeaway
`kernel.perf_event_paranoid` controls unprivileged event scope; Linux kernel documentation identifies CAP_PERFMON as the least-privilege capability for perf monitoring. Do not relax the system-wide sysctl casually. `perf record` inherits to child tasks by default (and exposes `--no-inherit`), but understand what “child” means: launched server descendants and threads are relevant, whereas a separately launched sibling/daemon is not automatically part of the measured task tree.

### Cited Findings
- Kernel docs: `perf_event_paranoid` -1 removes scope/access restrictions; >=0 permits per-process and system-wide scope but excludes raw tracepoints/ftrace; >=1 limits to per-process; >=2 limits per-process events to user-space execution. The documented thresholds are inclusive (`>=`). — [Perf events and tool security](https://docs.kernel.org/admin-guide/perf-security.html)
- Kernel docs recommend CAP_PERFMON for perf monitoring/observability; it is available since Linux 5.8 and avoids granting broad CAP_SYS_ADMIN. CAP_SYS_ADMIN remains a backward-compatible route. `perf_event_open(2)` notes CAP_PERFMON for monitored pid access since Linux 5.9. — [Kernel perf security guide](https://docs.kernel.org/admin-guide/perf-security.html); [perf_event_open(2)](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)
- Kernel docs note that before Linux 5.9, unprivileged perf monitoring also used `PTRACE_MODE_READ_REALCREDS`; on Linux 5.9+ CAP_PERFMON is sufficient for that scope check. A failed `perf record -p PID` can therefore stem from ptrace policy on older kernels, but modern attach failures may instead be perf scope, LSM, or container restrictions. — [Kernel perf security guide](https://docs.kernel.org/admin-guide/perf-security.html)
- `perf record` has `--no-inherit` to prevent child tasks inheriting counters; `perf_event_attr.inherit` is documented as applying to new children, not already-existing children or children of existing children. Inherit plus `cpu == -1` has sampling/ring-buffer constraints at the syscall API level. — [perf-record(1)](https://man7.org/linux/man-pages/man1/perf-record.1.html); [perf_event_open(2)](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)
- `perf stat` also provides `-i/--no-inherit`. Its default behavior should be checked against the installed perf version/options if exact descendant attribution matters; `perf --version` and local `perf stat --help` are authoritative for the installed Fedora build. — [perf-stat(1)](https://man7.org/linux/man-pages/man1/perf-stat.1.html)
- Safe diagnostic commands (no policy changes):

  ```sh
  cat /proc/sys/kernel/perf_event_paranoid
  id
  grep -E '^(CapEff|CapPrm|NoNewPrivs|Seccomp):' /proc/self/status
  getcap "$(command -v perf)" 2>/dev/null || true
  perf stat -v -e task-clock -- sleep 1
  perf stat -v -e cycles:u -- sleep 1
  ```

- Avoid recommending `sudo sysctl -w kernel.perf_event_paranoid=-1` as a routine fix. If policy change is approved by the host administrator, prefer scoped/temporary least-privilege access (e.g. CAP_PERFMON for the profiling tool or a controlled sysctl change), document the original value, and restore it after the test. Kernel docs explain that perf data can expose execution context, addresses, and potentially sensitive data. — [Kernel perf security guide](https://docs.kernel.org/admin-guide/perf-security.html)

### Inferences
- Launch-under-perf commonly works where `perf record -p` fails because measurement is arranged before the target starts and does not depend on racing/attaching to an already running process; it still fails if the caller is barred from opening the relevant perf events.
- If the LSP is launched by an IDE/client, launch the actual client/harness under `perf` or arrange for it to launch server as a child. Attaching to a process that has already forked may miss work that occurred before attachment; inheritance cannot retroactively cover existing children.

### Gaps
- Exact Fedora default `perf_event_paranoid` varies by release, admin hardening, and runtime/container context; check the live value rather than assuming a distro default.
- `CAP_PERFMON` availability and propagation through user namespaces/containers are environment-dependent; kernel docs do not guarantee a container runtime grants it.

## Separating startup/initialize from background indexing; profiles and call graphs

### Takeaway
Measure protocol phases with explicit harness timestamps and use `perf stat` controlled windows for count deltas. For a startup-only profile, launch under `perf record` and use `--delay=-1` plus the documented FIFO/FD control channel to enable immediately before the initialize request and disable after the response. For a separate indexing profile, enable only after initialize and disable at a defined indexing completion/steady-state boundary. Avoid relying on a fixed millisecond delay when protocol events provide a precise boundary.

### Cited Findings
- `perf record --delay=-1` starts with events disabled; its `--control=fifo:...` or `fd:...` interface accepts `enable`, `disable`, `stop` and optional acknowledgements, with a documented shell example. `perf stat` has the same disabled-start/control mechanism. — [perf-record(1)](https://man7.org/linux/man-pages/man1/perf-record.1.html); [perf-stat(1)](https://man7.org/linux/man-pages/man1/perf-stat.1.html)
- Both `perf record` and `perf stat` support `-D/--delay`; `-D <msecs>` delays measurement after program start, while `-D -1` begins disabled. The man pages explicitly identify filtering the different startup phase as a use case. — [perf-record(1)](https://man7.org/linux/man-pages/man1/perf-record.1.html); [perf-stat(1)](https://man7.org/linux/man-pages/man1/perf-stat.1.html)
- `perf record --call-graph` supports `fp`, `dwarf`, and `lbr`. The documented default is `fp`; if the executable omits frame pointers, FP stacks can be bogus and DWARF CFI should be used if perf has libunwind/libdw support. DWARF records a user stack dump (default 8192 bytes; configurable); LBR is hardware-dependent (notably newer Intel), user-only, and cannot be combined with branch-stack sampling. — [perf-record(1)](https://man7.org/linux/man-pages/man1/perf-record.1.html)
- For symbolized reports, preserve the exact executable/shared libraries and build IDs used during capture; `perf record` documents build-id recording/caching behavior and warns that skipping build IDs can misresolve symbols after binaries are rebuilt or upgraded. Build with debug symbols and, where appropriate, frame pointers for more reliable lower-overhead FP unwinding. — [perf-record(1)](https://man7.org/linux/man-pages/man1/perf-record.1.html)
- Example whole-run counts, useful for coarse comparison (replace arguments with the real harness):

  ```sh
  perf stat -r 10 -e task-clock,cycles,instructions,branches,branch-misses \
    -- ./lsp_test_client --server ./build/lsp/heimdall-lsp --scenario initialize
  ```

- Example startup and initialize CPU profile: create control FIFOs, launch the harness under `perf record -D -1 ... --control=fifo:...`, then have the harness send `enable` immediately before writing initialize and `disable` immediately after reading the initialize response. Use a separate profile/output file for indexing; do not mix these windows when interpreting `perf report`.

  ```sh
  perf record -D -1 -e cycles:u -F 99 --call-graph dwarf,8192 \
    -o startup.perf.data --control=fifo:/tmp/lsp-perf-ctl,/tmp/lsp-perf-ack \
    -- ./lsp_test_client --server ./build/lsp/heimdall-lsp --scenario initialize
  perf report -i startup.perf.data
  ```

- Example delayed indexing-focused profile if initialization duration is stable (less precise than protocol control):

  ```sh
  perf record -D 1500 -e cycles:u -F 99 --call-graph dwarf,8192 \
    -o indexing.perf.data -- ./lsp_test_client --server ./build/lsp/heimdall-lsp \
    --scenario initialize-then-wait-for-indexing
  perf report -i indexing.perf.data
  ```

- Prefer `cycles:u` to focus CPU-side user code when kernel samples are not needed; use `perf list` and test event availability. Sampling frequency (e.g. `-F 99`) trades resolution for overhead/data volume; reduce it for long indexing runs or use event period. Compare profiles with identical frequency, event, build, workload, and `--call-graph` mode.

### Inferences
- LSP protocol-aware control gives a clean boundary for initialize latency even when initialization triggers asynchronous indexing. To characterize indexing, keep the same process alive after initialize and measure a separate, explicitly bounded interval; report both wall time and task-clock/CPU counters because indexing may be I/O- or wait-bound.
- If client control cannot drive perf FIFOs, a wrapper can create/open FIFOs and coordinate with a test harness; use acknowledgements to avoid races. For startup-to-initialize, ensure the initial disabled period includes process spawn but enable before the request; separately measure spawn-to-initialize wall time with the harness.
- `perf stat` full-command wall elapsed is not the same as CPU time or pure request latency. Request-level timestamps are needed to avoid attributing client setup, shutdown, or post-initialize background work to the initialize response.

### Gaps
- The exact protocol control integration depends on the LSP test client/harness; the generic perf CLI cannot infer JSON-RPC initialize boundaries by itself.
- DWARF unwinding availability depends on how Fedora's perf was built and the target's unwind metadata. Verify with a short test and inspect `perf report`; if stacks are missing, try `--call-graph fp` only when frame pointers are present, or `lbr` only on supported hardware.
