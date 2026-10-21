"""Measure real LSP indexing in fresh processes; OS disk caches are not flushed."""

import argparse
import hashlib
import json
import math
import os
import platform
import queue
import statistics
import subprocess
import threading
import time
from pathlib import Path


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def run(server, workspace, timeout, metrics, workers):
    environment = dict(os.environ)
    if metrics:
        environment["HEIMDALL_INDEX_METRICS"] = "1"
    if workers:
        environment["HEIMDALL_INDEX_THREADS"] = str(workers)
    process = subprocess.Popen([str(server)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               cwd=workspace, env=environment)
    inbox = queue.Queue()
    records = []
    errors = []
    peak_rss = [0]
    sampling_stop = threading.Event()

    def sample_memory():
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes

            class Counters(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
                    (name, ctypes.c_size_t) for name in ("peak", "working", "pagedPeak", "paged",
                        "nonPagedPeak", "nonPaged", "pagefile", "pagefilePeak")]

            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.restype = wintypes.HANDLE
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            query = ctypes.WinDLL("psapi").GetProcessMemoryInfo
            query.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
            handle = kernel.OpenProcess(0x410, False, process.pid)
            if not handle:
                return
            try:
                while not sampling_stop.wait(0.1):
                    counters = Counters()
                    counters.cb = ctypes.sizeof(counters)
                    if query(handle, ctypes.byref(counters), counters.cb):
                        peak_rss[0] = max(peak_rss[0], counters.peak)
            finally:
                kernel.CloseHandle(handle)
        else:
            while not sampling_stop.wait(0.1):
                try:
                    for line in Path(f"/proc/{process.pid}/status").read_text().splitlines():
                        if line.startswith("VmHWM:"):
                            peak_rss[0] = max(peak_rss[0], int(line.split()[1]) * 1024)
                except OSError:
                    return

    def read_messages():
        try:
            while True:
                header = b""
                while not header.endswith(b"\r\n\r\n"):
                    chunk = process.stdout.read(1)
                    if not chunk:
                        return
                    header += chunk
                length = int(header.decode("ascii").split(":", 1)[1].strip())
                inbox.put(json.loads(process.stdout.read(length)))
        except Exception as error:
            inbox.put({"readerError": str(error)})

    def read_metrics():
        for line in process.stderr:
            text = line.decode("utf-8", errors="replace").strip()
            if text.startswith("heimdall-index-metrics "):
                records.append(json.loads(text.split(" ", 1)[1]))
            else:
                errors.append(text)

    def send(message):
        body = json.dumps(message).encode("utf-8")
        process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        process.stdin.flush()

    readers = [threading.Thread(target=read_messages, daemon=True),
               threading.Thread(target=read_metrics, daemon=True)]
    sampler = threading.Thread(target=sample_memory, daemon=True)
    sampler.start()
    for reader in readers:
        reader.start()
    started = time.monotonic()
    statuses = []
    project_ms = None
    last_print = 0
    try:
        send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "workspaceFolders": [{"uri": workspace.as_uri(), "name": workspace.name}],
            "capabilities": {"window": {"workDoneProgress": True}},
            "initializationOptions": {"indexStatus": True}}})
        while time.monotonic() - started < timeout:
            try:
                message = inbox.get(timeout=0.5)
            except queue.Empty:
                if process.poll() is not None:
                    raise RuntimeError(f"Server exited: {process.returncode}; {errors[-10:]}")
                continue
            if "readerError" in message:
                raise RuntimeError(message["readerError"])
            if message.get("id") == 1:
                if "error" in message:
                    raise RuntimeError(str(message["error"]))
                send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            if message.get("method") == "window/workDoneProgress/create":
                send({"jsonrpc": "2.0", "id": message["id"], "result": None})
            if message.get("method") == "heimdall/indexStatus":
                status = message["params"]
                statuses.append(status)
                if time.monotonic() - last_print >= 10:
                    print(f"{server.name}: {status['done']}/{status['total']} "
                          f"{status.get('activity', '')}", flush=True)
                    last_print = time.monotonic()
            if message.get("method") == "window/logMessage" and \
                    "project files processed:" in message["params"]["message"]:
                project_ms = (time.monotonic() - started) * 1000
            if message.get("method") == "window/logMessage" and \
                    "symbol index ready:" in message["params"]["message"]:
                elapsed = (time.monotonic() - started) * 1000
                send({"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None})
                break
        else:
            raise TimeoutError(f"Indexing exceeded {timeout}s; last status: {statuses[-1:]}")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            message = inbox.get(timeout=1)
            if message.get("id") == 2:
                send({"jsonrpc": "2.0", "method": "exit", "params": None})
                process.wait(timeout=10)
                break
        for reader in readers:
            reader.join(timeout=1)
        numeric = [key for key in records[0] if key != "file"] if records else []
        return {"elapsedMs": elapsed, "projectMs": project_ms, "files": statuses[-1]["total"],
                "peakRssBytes": peak_rss[0] or None,
                "statuses": statuses, "macroMetrics": records,
                "totals": {key: sum(record[key] for record in records) for key in numeric},
                "stderr": errors}
    finally:
        sampling_stop.set()
        sampler.join(timeout=1)
        if process.poll() is None:
            process.kill()
            process.wait()
        for stream in (process.stdin, process.stdout, process.stderr):
            stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--heimdall", required=True, type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--workspace", default=Path.cwd(), type=Path)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--metrics", action="store_true")
    parser.add_argument("--workers", type=int, choices=(1, 2, 4))
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    servers = {"candidate": args.heimdall.resolve()}
    if args.baseline:
        servers["baseline"] = args.baseline.resolve()
    report = {"scenario": "fresh-process-os-cache-uncontrolled",
              "platform": platform.platform(), "cpuCount": os.cpu_count(),
              "workspace": str(args.workspace.resolve()), "runs": {key: [] for key in servers},
              "workers": args.workers,
              "executables": {key: {"path": str(path),
                  "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                  for key, path in servers.items()}}
    database = args.workspace.resolve() / "build/compile_commands.json"
    if database.exists():
        report["compileCommandsSha256"] = hashlib.sha256(database.read_bytes()).hexdigest()
    for iteration in range(args.runs):
        order = list(servers)
        if iteration % 2:
            order.reverse()
        for key in order:
            report["runs"][key].append(run(servers[key], args.workspace.resolve(),
                                             args.timeout, args.metrics, args.workers))
            args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    report["summary"] = {}
    for key, runs in report["runs"].items():
        values = [item["elapsedMs"] for item in runs]
        summary = {"samples": len(values), "medianMs": statistics.median(values),
                   "minMs": min(values), "maxMs": max(values)}
        if len(values) >= 30:
            summary["p95Ms"] = percentile(values, 0.95)
        if len(values) >= 100:
            summary["p99Ms"] = percentile(values, 0.99)
        report["summary"][key] = summary
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report["summary"], indent=2))


if __name__ == "__main__":
    main()
