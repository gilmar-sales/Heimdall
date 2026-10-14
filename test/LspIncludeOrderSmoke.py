import json
import subprocess
import sys
import tempfile
from pathlib import Path


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def parse(stream):
    responses = []
    while stream:
        header, stream = stream.split(b"\r\n\r\n", 1)
        length = int(header.decode("ascii").split(":", 1)[1].strip())
        body, stream = stream[:length], stream[length:]
        responses.append(json.loads(body))
    return responses


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_include_order_") as temporary:
    root = Path(temporary)
    (root / ".heimdall.json").write_text(json.dumps({
        "root": True,
        "rules": {"cpp/sort-includes": "warning"},
        "format": {"sort-includes": True},
        "include-order": {"groups": ["angle", "quote"], "case-insensitive": True},
    }), encoding="utf-8")
    source = root / "main.cpp"
    report = root / "Reporting.cpp"
    report_text = '#include <iostream>\n#include <fstream>\n\n#include "Reporting.hpp"\n'
    report_expected = '#include "Reporting.hpp"\n\n#include <fstream>\n#include <iostream>\n'
    report.write_text(report_text, encoding="utf-8")
    report_uri = report.as_uri()
    text = '#include "z.hpp"\n#include <apple>\n#include <Zebra>\n#include "App.hpp"\n'
    expected = '#include <apple>\n#include <Zebra>\n\n#include "App.hpp"\n#include "z.hpp"\n'
    source.write_text(text, encoding="utf-8")
    uri = source.as_uri()
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "rootUri": root.as_uri(), "initializationOptions": {"enableSemantic": False}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": uri, "languageId": "cpp", "version": 1, "text": text}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/codeAction", "params": {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 4, "character": 0}},
            "context": {"diagnostics": [], "only": ["quickfix"]}}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": report_uri, "languageId": "cpp", "version": 1, "text": report_text}}},
        {"jsonrpc": "2.0", "id": 3, "method": "textDocument/formatting", "params": {
            "textDocument": {"uri": uri}, "options": {"tabSize": 4, "insertSpaces": True}}},
        {"jsonrpc": "2.0", "id": 5, "method": "textDocument/formatting", "params": {
            "textDocument": {"uri": report_uri}, "options": {"tabSize": 4, "insertSpaces": True}}},
        {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ]
    process = subprocess.run([sys.argv[1]], input=b"".join(frame(m) for m in messages),
                             capture_output=True, cwd=root, timeout=30, check=False)
    assert process.returncode == 0, process.stderr.decode(errors="replace")
    responses = parse(process.stdout)
    diagnostics = [d for r in responses if r.get("method") == "textDocument/publishDiagnostics"
                   and r["params"]["uri"] == uri for d in r["params"]["diagnostics"]]
    unsorted = [d for d in diagnostics if d["code"] == "cpp/sort-includes"]
    assert len(unsorted) == 1, diagnostics
    assert unsorted[0]["severity"] == 2, unsorted
    assert "configured order" in unsorted[0]["message"], unsorted
    actions = next(r for r in responses if r.get("id") == 2)["result"]
    fixes = [a for a in actions if a["kind"] == "quickfix"
             and "cpp/sort-includes" in a["title"]]
    assert len(fixes) == 1, actions
    edit = fixes[0]["edit"]["changes"][uri][0]
    assert edit["newText"] == expected, edit
    assert edit["range"]["start"] == {"line": 0, "character": 0}, edit
    assert edit["range"]["end"] == {"line": 4, "character": 0}, edit
    formatted = next(r for r in responses if r.get("id") == 3)["result"]
    assert len(formatted) == 1 and formatted[0]["newText"] == expected, formatted
    rep_formatted = next(r for r in responses if r.get("id") == 5)["result"]
    assert len(rep_formatted) == 1 and rep_formatted[0]["newText"] == report_expected, rep_formatted
