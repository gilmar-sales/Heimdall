"""Compare Heimdall and clangd over identical LSP stdio workloads."""

import argparse
import hashlib
import json
import math
import os
import platform
import queue
import shutil
import statistics
import subprocess
import tempfile
import threading
import time
from pathlib import Path


def percentile(values, fraction):
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)]


def response_count(method, result):
    if method == "textDocument/completion":
        return len(result if isinstance(result, list) else (result or {}).get("items", []))
    if method == "textDocument/semanticTokens/full":
        return len((result or {}).get("data", [])) // 5
    if isinstance(result, list):
        return len(result)
    return int(result is not None)


def locations(result):
    entries = result if isinstance(result, list) else [result] if result else []
    return [{"uri": entry.get("uri", entry.get("targetUri")),
             "range": entry.get("range", entry.get("targetSelectionRange"))}
            for entry in entries]


def normalize_result(value, workspace):
    if isinstance(value, str):
        return value.replace(workspace.as_uri(), "workspace://").replace(str(workspace), "<workspace>")
    if isinstance(value, list):
        return [normalize_result(item, workspace) for item in value]
    if isinstance(value, dict):
        return {key: normalize_result(item, workspace) for key, item in value.items()}
    return value


def reflection_inventory(text):
    return {"reflection_operators": text.count("^^"),
            "splice_openers": text.count("[:"), "meta_mentions": text.count("std::meta::"),
            "limitation": "lexical inventory, not semantic reflection evaluation"}


def document_diagnostics(notifications, uri):
    publications = [message["params"] for message in notifications
                    if message.get("method") == "textDocument/publishDiagnostics"
                    and message.get("params", {}).get("uri") == uri]
    if not publications:
        return {"status": "not_observed", "diagnostics": None}
    latest = publications[-1]
    return {"status": "observed", "version": latest.get("version"),
            "diagnostics": latest["diagnostics"]}


def quality_workspace(root, compiler):
    source, _, _ = create_workspace(root, 1, compiler)
    text = '''#include "Widget.hpp"
int Select(int value) { return value; }
double Select(double value) { return value; }
template<class T> struct Box { T payload; };
int main() {
    Widget object;
    int value = 1;
    { int value = 2; value += 1; }
    Box<Widget> box;
    auto selected = Select(1);
    return object.Value() + value + box.payload.Value();
}
'''
    source.write_text(text)
    rows = text.splitlines()

    def position(line, token, offset=1):
        return {"line": line, "character": rows[line].index(token) + offset}

    document = {"textDocument": {"uri": source.as_uri()}}
    cases = []

    def add(label, capability, method, line, token, expectation, **extra):
        cases.append({"name": label, "capability": capability, "method": method,
                      "params": dict(document, position=position(line, token), **extra),
                      "expectation": expectation})

    add("dependency_definition", "definitionProvider", "textDocument/definition", 10,
        "Value", {"kind": "definition", "file": "Widget.hpp", "line": 1})
    add("overload_definition", "definitionProvider", "textDocument/definition", 9,
        "Select", {"kind": "definition", "file": "Main.cpp", "line": 1})
    add("shadowed_references", "referencesProvider", "textDocument/references", 6,
        "value", {"kind": "references", "lines": [6, 10]}, context={"includeDeclaration": True})
    add("shadowed_rename", "renameProvider", "textDocument/rename", 6,
        "value", {"kind": "rename", "lines": [6, 10]}, newName="renamedValue")
    add("template_member_hover", "hoverProvider", "textDocument/hover", 10,
        "payload", {"kind": "hover", "contains": "Widget"})
    cases.append({"name": "member_completion", "capability": "completionProvider",
                  "method": "textDocument/completion",
                  "params": dict(document, position=position(10, "Value", 0)),
                  "expectation": {"kind": "completion", "contains": "Value"}})
    cases.append({"name": "overload_signature", "capability": "signatureHelpProvider",
                  "method": "textDocument/signatureHelp",
                  "params": dict(document, position=position(9, "Select(", 7)),
                  "expectation": {"kind": "signature", "minimum": 2}})
    return source, text, cases


def assess_result(result, expectation):
    kind = expectation["kind"]
    if kind == "definition":
        found = locations(result)
        return bool(found) and all(item["uri"].endswith("/" + expectation["file"])
                                  and item["range"]["start"]["line"] == expectation["line"]
                                  for item in found)
    if kind in ("references", "rename"):
        if kind == "references":
            edits = locations(result)
        else:
            edits = [{"uri": uri, "range": edit["range"]}
                     for uri, changes in (result or {}).get("changes", {}).items() for edit in changes]
            edits += [{"uri": change["textDocument"]["uri"], "range": edit["range"]}
                      for change in (result or {}).get("documentChanges", [])
                      if "textDocument" in change for edit in change.get("edits", [])]
        return (sorted(item["range"]["start"]["line"] for item in edits) == expectation["lines"]
                and all(item["uri"].endswith("/Main.cpp") for item in edits))
    if kind == "completion":
        items = result if isinstance(result, list) else (result or {}).get("items", [])
        return any(expectation["contains"] in item.get("label", "") for item in items)
    if kind == "signature":
        return len((result or {}).get("signatures", [])) >= expectation["minimum"]
    return bool(result) and expectation["contains"] in json.dumps(result.get("contents"))


def compare_quality(servers, args, compiler):
    results = {}
    for name, executable in servers.items():
        with tempfile.TemporaryDirectory(prefix="lsp-quality-", dir="/tmp/opencode"
                                         if Path("/tmp/opencode").is_dir() else None) as directory:
            root = Path(directory).resolve()
            source, text, cases = quality_workspace(root, compiler)
            command = [executable]
            if name == "clangd":
                command += ["--log=error", "--background-index", "--clang-tidy=false",
                            "--compile-commands-dir=" + str(root)]
            client = Client(command, root, args.timeout, args.memory_limit_mib)
            results[name] = {"cases": [], "command": command}
            try:
                _, initialization, _ = client.request("initialize", {
                    "processId": os.getpid(), "rootUri": root.as_uri(),
                    "capabilities": {"workspace": {"workspaceEdit": {"documentChanges": True}}},
                    "initializationOptions": {"enableSemantic": True,
                        "compileCommands": str(root / "compile_commands.json"),
                        "workspaceSymbols": True, "workspaceDiagnostics": True}})
                capabilities = initialization["capabilities"]
                results[name]["capabilities"] = capabilities
                results[name]["server_info"] = initialization.get("serverInfo")
                client.notify("initialized", {})
                client.notify("textDocument/didOpen", {"textDocument": {
                    "uri": source.as_uri(), "languageId": "cpp", "version": 1, "text": text}})
                for case in cases:
                    record = {"name": case["name"], "method": case["method"],
                              "expectation": case["expectation"]}
                    if not capabilities.get(case["capability"]):
                        record["status"] = "unsupported"
                    else:
                        try:
                            elapsed, response, _ = client.request(case["method"], case["params"])
                            record.update(status="pass" if assess_result(response, case["expectation"])
                                          else "mismatch", elapsed_ms=elapsed,
                                          response=normalize_result(response, root))
                        except (RuntimeError, TimeoutError) as error:
                            record.update(status="error", error=str(error))
                    results[name]["cases"].append(record)
            finally:
                client.close()
    return results


class Client:
    def __init__(self, command, workspace, timeout, memory_limit):
        self.timeout = timeout
        self.memory_limit = memory_limit
        self.responses = queue.Queue()
        self.notifications = []
        self.write_lock = threading.Lock()
        self.identifier = 0
        self.peak_rss_mib = None
        self.current_rss_mib = None
        self.memory_exceeded = False
        self.stop = threading.Event()
        self.stderr = tempfile.TemporaryFile()
        self.process = subprocess.Popen(command, cwd=workspace, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.stderr)
        self.reader = threading.Thread(target=self.read_messages, daemon=True)
        self.monitor = threading.Thread(target=self.sample_memory, daemon=True)
        self.reader.start()
        self.monitor.start()

    def read_messages(self):
        try:
            while True:
                length = None
                while True:
                    line = self.process.stdout.readline()
                    if not line:
                        raise RuntimeError("server closed stdout")
                    if line == b"\r\n":
                        break
                    if line.lower().startswith(b"content-length:"):
                        length = int(line.split(b":", 1)[1])
                if length is None or length > 64 * 1024 * 1024:
                    raise RuntimeError("missing or oversized Content-Length")
                body = self.process.stdout.read(length)
                if len(body) != length:
                    raise RuntimeError("truncated LSP response")
                message = json.loads(body)
                if "id" not in message:
                    self.notifications.append(message)
                if "id" in message:
                    if "method" in message:
                        self.send({"id": message["id"], "result": None})
                    else:
                        self.responses.put((time.perf_counter(), message, length))
        except Exception as error:
            self.responses.put(error)

    def sample_memory(self):
        status = Path(f"/proc/{self.process.pid}/status")
        if not status.exists():
            return
        while not self.stop.is_set():
            try:
                fields = dict(line.split(":", 1) for line in status.read_text().splitlines()
                              if ":" in line)
                rss = int(fields["VmRSS"].split()[0]) / 1024
                high_water = int(fields["VmHWM"].split()[0]) / 1024
                self.current_rss_mib = rss
                self.peak_rss_mib = max(self.peak_rss_mib or 0, high_water)
                if rss > self.memory_limit:
                    self.memory_exceeded = True
                    self.process.kill()
                    return
            except (OSError, KeyError, ValueError):
                return
            self.stop.wait(0.01)

    def send(self, message):
        message["jsonrpc"] = "2.0"
        body = json.dumps(message, separators=(",", ":")).encode("utf-8")
        with self.write_lock:
            self.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
            self.process.stdin.flush()

    def notify(self, method, params):
        self.send({"method": method, "params": params})

    def request(self, method, params, started=None):
        self.identifier += 1
        identifier = self.identifier
        started = time.perf_counter() if started is None else started
        self.send({"id": identifier, "method": method, "params": params})
        deadline = time.monotonic() + self.timeout
        while True:
            try:
                reply = self.responses.get(timeout=max(0, deadline - time.monotonic()))
            except queue.Empty as error:
                raise TimeoutError(f"{method} exceeded {self.timeout}s") from error
            if isinstance(reply, Exception):
                raise RuntimeError(f"{method}: {reply}; memory limit exceeded={self.memory_exceeded}")
            received, message, size = reply
            if message.get("id") != identifier:
                continue
            if "error" in message:
                raise RuntimeError(f"{method}: {message['error']}")
            return (received - started) * 1000, message.get("result"), size

    def close(self):
        try:
            if self.process.poll() is None:
                self.request("shutdown", None)
                self.notify("exit", None)
                try:
                    self.process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.stop.set()
            self.reader.join(timeout=1)
            self.monitor.join(timeout=1)
            self.process.stdin.close()
            self.process.stdout.close()
            self.stderr.close()


def create_workspace(root, lines, compiler):
    dependency = root / "build/_deps/widget-src/include"
    dependency.mkdir(parents=True)
    (dependency / "Widget.hpp").write_text(
        "#pragma once\nstruct Widget { int Value() const { return 42; } };\n")
    body = ['#include "Widget.hpp"']
    body.extend(f"int Function{i}() {{ return {i}; }}" for i in range(lines))
    body.append("int main() { Widget object; return object.Value(); }")
    text = "\n".join(body) + "\n"
    source = root / "Main.cpp"
    source.write_text(text)
    commands = [{"directory": str(root), "file": str(source), "arguments": [
        compiler, "-std=c++20", "-I" + str(dependency), "-c", str(source)]}]
    (root / "compile_commands.json").write_text(json.dumps(commands))
    (root / ".heimdall.json").write_text(json.dumps({"root": True}))
    position = {"line": len(body) - 1, "character": body[-1].index("Value") + 2}
    return source, text, position


def benchmark(name, executable, workspace, source, text, position, args):
    compile_commands = args.compile_commands if args.project else workspace / "compile_commands.json"
    command = [executable]
    if name == "clangd":
        command += ["--log=error", "--background-index", "--clang-tidy=false",
                    "--compile-commands-dir=" + str(compile_commands.parent)]
    started = time.perf_counter()
    client = Client(command, workspace, args.timeout, args.memory_limit_mib)
    result = {"command": command, "samples": {}, "response_counts": {}, "response_bytes": {}}
    try:
        _, initialization, _ = client.request("initialize", {
            "processId": os.getpid(), "rootUri": workspace.as_uri(),
            "capabilities": {"workspace": {"workspaceEdit": {"documentChanges": True}},
                "textDocument": {"completion": {"completionItem": {"snippetSupport": True}},
                    "semanticTokens": {"requests": {"full": True},
                        "tokenTypes": ["namespace", "type", "class", "enum", "interface", "struct",
                            "typeParameter", "parameter", "variable", "property", "enumMember",
                            "event", "function", "method", "macro", "keyword", "modifier", "comment",
                            "string", "number", "regexp", "operator", "decorator"],
                        "tokenModifiers": ["declaration", "definition", "readonly", "static",
                            "deprecated", "abstract", "async", "modification", "documentation",
                            "defaultLibrary"], "formats": ["relative"]}}},
            "initializationOptions": {"enableSemantic": True,
                                      "compileCommands": str(compile_commands),
                                      "workspaceDiagnostics": True, "workspaceSymbols": True}})
        result["initialize_ms"] = (time.perf_counter() - started) * 1000
        result["server_info"] = initialization.get("serverInfo")
        capabilities = initialization["capabilities"]
        client.notify("initialized", {})
        uri = source.as_uri()
        document = {"textDocument": {"uri": uri}}
        cursor = dict(document, position=position)
        started = time.perf_counter()
        client.notify("textDocument/didOpen", {"textDocument": {
            "uri": uri, "languageId": "cpp", "version": 1, "text": text}})
        cold, completion, _ = client.request("textDocument/completion", cursor, started)
        result["open_to_completion_ms"] = cold
        result["cold_completion_items"] = response_count("textDocument/completion", completion)
        workloads = [("completion", "completionProvider", "textDocument/completion", cursor),
                     ("hover", "hoverProvider", "textDocument/hover", cursor),
                     ("symbols", "documentSymbolProvider", "textDocument/documentSymbol", document),
                     ("tokens", "semanticTokensProvider", "textDocument/semanticTokens/full", document)]
        for label, capability, method, params in workloads:
            advertised = capabilities.get(capability)
            if not advertised or (label == "tokens" and not advertised.get("full")):
                result["samples"][label] = None
                continue
            samples, counts, sizes = [], [], []
            for iteration in range(args.warmup + args.iterations):
                elapsed, response, size = client.request(method, params)
                if iteration >= args.warmup:
                    samples.append(elapsed)
                    counts.append(response_count(method, response))
                    sizes.append(size)
            result["samples"][label] = samples
            result["response_counts"][label] = counts
            result["response_bytes"][label] = sizes
        edits, counts = [], []
        number_column = 0 if args.project else text.splitlines()[1].index("return ") + len("return ")
        for iteration in range(args.warmup + args.iterations):
            started = time.perf_counter()
            client.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": iteration + 2},
                "contentChanges": [{"range": {
                    "start": {"line": 1, "character": number_column},
                    "end": {"line": 1, "character": number_column + 1}},
                    "text": str((iteration + 1) % 10)}] if not args.project else
                    [{"text": text + f"\n// LSP comparison revision {iteration}\n"}]})
            elapsed, response, _ = client.request("textDocument/completion", cursor, started)
            if iteration >= args.warmup:
                edits.append(elapsed)
                counts.append(response_count("textDocument/completion", response))
        result["samples"]["edit_completion"] = edits
        result["response_counts"]["edit_completion"] = counts
        result["rss_mib"] = client.current_rss_mib
        result["peak_rss_mib"] = client.peak_rss_mib
        result["capabilities"] = capabilities
        if args.project:
            result["probes"] = {}
            for label, capability, method, extra in [
                    ("definition", "definitionProvider", "textDocument/definition", {}),
                    ("references", "referencesProvider", "textDocument/references",
                     {"context": {"includeDeclaration": True}}),
                    ("hover", "hoverProvider", "textDocument/hover", {})]:
                if not capabilities.get(capability):
                    result["probes"][label] = {"status": "unsupported"}
                    continue
                try:
                    elapsed, response, _ = client.request(method, dict(cursor, **extra))
                    result["probes"][label] = {"status": "nonempty" if response_count(method, response)
                                              else "empty", "elapsed_ms": elapsed,
                                              "response": normalize_result(response, workspace)}
                except (RuntimeError, TimeoutError) as error:
                    result["probes"][label] = {"status": "error", "error": str(error)}
    finally:
        try:
            client.stderr.seek(0)
            result["stderr"] = client.stderr.read().decode("utf-8", errors="replace")
            client.close()
        finally:
            result["notifications"] = normalize_result(client.notifications, workspace)
            result["document_diagnostics"] = normalize_result(
                document_diagnostics(client.notifications, source.as_uri()), workspace)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--heimdall", default="build/lsp/heimdall-lsp")
    parser.add_argument("--clangd", default="clangd")
    parser.add_argument("--compiler", default="c++")
    parser.add_argument("--lines", type=int, nargs="+", default=[1000, 5000])
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--memory-limit-mib", type=float, default=1024)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--quality-only", action="store_true",
                        help="run functional probes without the latency benchmark")
    parser.add_argument("--project", type=Path, help="benchmark an existing project without disk edits")
    parser.add_argument("--compile-commands", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--line", type=int, default=1, help="one-based cursor line for --project")
    parser.add_argument("--character", type=int, default=0, help="zero-based UTF-16 cursor column")
    args = parser.parse_args()
    args.project = args.project.resolve() if args.project else None
    if args.project:
        if not args.source or args.quality_only:
            parser.error("--project requires --source and cannot be combined with --quality-only")
        args.source = (args.project / args.source).resolve()
        args.compile_commands = (args.compile_commands or args.project / "build/compile_commands.json").resolve()
        if not args.source.is_file() or not args.compile_commands.is_file():
            parser.error("source and compile_commands.json must exist")
        text = args.source.read_text()
        if not 1 <= args.line <= len(text.splitlines()) or not 0 <= args.character <= len(
                text.splitlines()[args.line - 1].encode("utf-16-le")) // 2:
            parser.error("cursor is outside the source")
    if min(args.lines) < 1 or min(args.iterations, args.repeats) < 1 or args.warmup < 0:
        parser.error("lines, iterations and repeats must be positive; warmup must be nonnegative")
    if args.timeout <= 0 or args.memory_limit_mib <= 0:
        parser.error("timeout and memory limit must be positive")
    servers = {}
    for name, executable in [("heimdall", args.heimdall), ("clangd", args.clangd)]:
        found = shutil.which(executable)
        if found is None:
            parser.error(f"server not found: {executable}")
        servers[name] = str(Path(found).resolve())
    compiler = shutil.which(args.compiler)
    if compiler is None:
        parser.error(f"compiler not found: {args.compiler}")
    report = {"platform": platform.platform(), "compiler": compiler,
              "settings": {key: str(value) if isinstance(value, Path) else value
                           for key, value in vars(args).items()},
              "server_sha256": {name: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                                for name, path in servers.items()}, "runs": []}
    temporary_root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
    report["quality"] = compare_quality(servers, args, compiler) if not args.project else {}
    print("\nFunctional probes (pass means only the explicit expectation was met):")
    for index, case in enumerate(report["quality"].get("heimdall", {}).get("cases", [])):
        print(f"{case['name']:26} Heimdall: {case['status']:11} "
              f"clangd: {report['quality']['clangd']['cases'][index]['status']}")
    sizes = [len(text.splitlines())] if args.project else args.lines
    if args.project:
        report["project"] = {"root": str(args.project), "source": str(args.source),
                             "compile_commands_sha256": hashlib.sha256(args.compile_commands.read_bytes()).hexdigest(),
                             "source_sha256": hashlib.sha256(text.encode()).hexdigest(),
                             "reflection": reflection_inventory(text),
                             "position": {"line": args.line - 1, "character": args.character}}
    for lines in ([] if args.quality_only else sizes):
        for repeat in range(args.repeats):
            order = list(servers) if repeat % 2 == 0 else list(reversed(servers))
            for name in order:
                if args.project:
                    try:
                        run = benchmark(name, servers[name], args.project, args.source, text,
                                        report["project"]["position"], args)
                    except (RuntimeError, TimeoutError, OSError) as error:
                        report["runs"].append({"server": name, "lines": lines, "repeat": repeat,
                                               "error": str(error), "samples": {}})
                        if args.output:
                            args.output.write_text(json.dumps(report, indent=2) + "\n")
                        print(f"{name}: error: {error}", flush=True)
                        continue
                    report["runs"].append(dict(run, server=name, lines=lines, repeat=repeat))
                    print(f"{name}: {args.source.name}; probes "
                          f"{ {key: value['status'] for key, value in run['probes'].items()} }", flush=True)
                    continue
                with tempfile.TemporaryDirectory(prefix="lsp-comparison-", dir=temporary_root) as directory:
                    root = Path(directory).resolve()
                    source, text, position = create_workspace(root, lines, compiler)
                    run = benchmark(name, servers[name], root, source, text, position, args)
                    report["runs"].append(dict(run, server=name, lines=lines, repeat=repeat))
                print(f"{name:8} {lines:5} functions run {repeat + 1}: "
                      f"open {run['open_to_completion_ms']:.1f} ms; "
                      f"peak RSS {run['peak_rss_mib']} MiB", flush=True)
    unit = "lines" if args.project else "functions"
    print(f"\nserver    {unit:9} operation           median ms    p95 ms   responses")
    for lines in ([] if args.quality_only else sizes):
        for name in servers:
            runs = [run for run in report["runs"] if run["server"] == name and run["lines"] == lines]
            runs = [run for run in runs if "error" not in run]
            if not runs:
                continue
            for label in runs[0]["samples"]:
                samples = [value for run in runs for value in (run["samples"][label] or [])]
                if not samples:
                    print(f"{name:8} {lines:9} {label:19} unsupported")
                    continue
                counts = [value for run in runs for value in run["response_counts"][label]]
                print(f"{name:8} {lines:9} {label:19} {statistics.median(samples):9.2f} "
                      f"{percentile(samples, 0.95):9.2f} {min(counts)}..{max(counts)}")
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
