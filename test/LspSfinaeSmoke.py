import json
import subprocess
import sys
import tempfile
from pathlib import Path


RULE = "cpp/template-substitution"
NO_MATCH = "template<class T> typename T::missing Probe(T);\nauto result = Probe(1);\n"
FALLBACK = "template<class T> typename T::missing Probe(T);\nlong Probe(...);\nauto result = Probe(1);\n"


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def diagnostics(server, directory, source, replacement=None, rule="error"):
    uri = (directory / "input.cpp").as_uri()
    (directory / ".heimdall.json").write_text(
        json.dumps({"root": True, "rules": {RULE: rule}}), encoding="utf-8"
    )
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "rootUri": directory.as_uri(),
            "initializationOptions": {"enableSemantic": True},
        }},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "cpp", "version": 1, "text": source}
        }},
    ]
    version = 1
    if replacement is not None:
        version = 2
        messages.append({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": version},
            "contentChanges": [{"text": replacement}],
        }})
    messages.extend([
        {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ])
    result = subprocess.run(
        [server], input=b"".join(frame(message) for message in messages),
        capture_output=True, cwd=directory, timeout=15, check=False
    )
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    publications = []
    wire = result.stdout
    while wire:
        header, wire = wire.split(b"\r\n\r\n", 1)
        headers = dict(line.split(":", 1) for line in header.decode("ascii").split("\r\n"))
        length = int(headers["Content-Length"])
        body, wire = wire[:length], wire[length:]
        message = json.loads(body)
        if message.get("method") == "textDocument/publishDiagnostics":
            if message["params"]["uri"] == uri:
                publications.append(message["params"])
    assert publications, result.stdout
    assert publications[-1].get("version") == version, publications
    return [item for item in publications[-1]["diagnostics"] if item["code"] == RULE]


temporary_root = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
with tempfile.TemporaryDirectory(prefix="heimdall-sfinae-lsp-", dir=temporary_root) as temporary:
    directory = Path(temporary)
    server = str(Path(sys.argv[1]).resolve())
    errors = diagnostics(server, directory, NO_MATCH)
    assert len(errors) == 1 and errors[0]["severity"] == 1, errors
    assert errors[0]["range"]["start"] == {"line": 1, "character": 14}, errors
    assert diagnostics(server, directory, FALLBACK) == []
    assert diagnostics(server, directory, NO_MATCH, replacement=FALLBACK) == []
    assert diagnostics(server, directory, NO_MATCH, rule="off") == []
