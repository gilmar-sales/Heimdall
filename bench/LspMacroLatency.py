"""Latency of macro-aware highlighting through the real heimdall-lsp (stdio).

Opens a file that includes several standard headers and a project header full of macros,
then measures `textDocument/semanticTokens/full` for:
  * the first request (cold: compiler probe, every header scanned),
  * an unchanged document (answered from the cached environment),
  * the request right after each edit (new text, header effects replayed from the cache).

    python bench/LspMacroLatency.py <heimdall-lsp> [--lines 5000] [--edits 20]
                                    [--cold-budget-ms 1500] [--edit-budget-ms 30]

Exit code 1 when the median edit latency exceeds --edit-budget-ms or the cold one exceeds
--cold-budget-ms. Budgets are for the median: a shared CI machine has noisy tails.
"""
import argparse
import json
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("server")
    parser.add_argument("--lines", type=int, default=5000)
    parser.add_argument("--edits", type=int, default=20)
    parser.add_argument("--cold-budget-ms", type=float, default=1500)
    parser.add_argument("--edit-budget-ms", type=float, default=30)
    args = parser.parse_args()

    workspace = Path(tempfile.mkdtemp(prefix="heimdall-macro-latency-"))
    header = workspace / "config.h"
    header_lines = ["#pragma once"]
    for i in range(300):
        header_lines.append(f"#define CONFIG_{i}(x) ((x) + {i})")
    header_lines.append("#define LIMIT 64")
    header.write_text("\n".join(header_lines) + "\n")

    body = ['#include "config.h"', "#include <iostream>", "#include <vector>", "#include <string>",
            "#include <map>", "#include <algorithm>", "#include <memory>", "#include <functional>"]
    for i in range(args.lines):
        body.append(f"int value{i} = CONFIG_{i % 300}(LIMIT) + {i};")
    text = "\n".join(body) + "\n"
    main_cpp = workspace / "main.cpp"
    main_cpp.write_text(text)
    commands = workspace / "compile_commands.json"
    commands.write_text(json.dumps([{
        "directory": workspace.as_posix(),
        "command": f"c++ -std=c++20 -c {main_cpp.as_posix()}",
        "file": main_cpp.as_posix()}]))

    process = subprocess.Popen([args.server], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL)

    def send(message):
        data = json.dumps(message, separators=(",", ":")).encode()
        process.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)
        process.stdin.flush()

    def read():
        # The body stays bytes: parsing a 200 KB token array in Python costs more than the
        # server takes to produce it, and would drown what is being measured.
        length = None
        while True:
            line = process.stdout.readline().strip()
            if not line:
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        return process.stdout.read(length)

    def request(identifier, method, params):
        send({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params})
        marker = b'"id":%d,' % identifier
        while True:
            body = read()
            if marker in body[:64]:
                return body

    request(1, "initialize", {"initializationOptions": {
        "enableSemantic": True, "compileCommands": commands.as_posix()}})
    send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    uri = main_cpp.as_uri()
    send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
        "uri": uri, "languageId": "cpp", "version": 1, "text": text}}})

    def tokens(identifier):
        started = time.perf_counter()
        reply = request(identifier, "textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})
        return (time.perf_counter() - started) * 1000, reply.count(b",") // 5

    cold, count = tokens(10)
    warm = statistics.median(tokens(11 + i)[0] for i in range(5))
    edits = []
    current = text
    for i in range(args.edits):
        current = current + f"int extra{i} = LIMIT;\n"
        send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 2 + i},
            "contentChanges": [{"text": current}]}})
        edits.append(tokens(100 + i)[0])

    request(999, "shutdown", None)
    send({"jsonrpc": "2.0", "method": "exit"})
    process.wait(timeout=10)

    median = statistics.median(edits)
    print(f"{args.lines} lines, {count} tokens")
    print(f"cold   {cold:8.1f} ms")
    print(f"warm   {warm:8.1f} ms (median of 5, unchanged document)")
    print(f"edit   {median:8.1f} ms median, {max(edits):.1f} ms worst of {len(edits)}")
    failed = median > args.edit_budget_ms or cold > args.cold_budget_ms
    if failed:
        print(f"over budget (cold {args.cold_budget_ms} ms, edit {args.edit_budget_ms} ms)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
