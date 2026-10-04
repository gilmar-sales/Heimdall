import json
import subprocess
import sys
import tempfile
from pathlib import Path

server = sys.argv[1]


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def run(workspace, options, extra=()):
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "workspaceFolders": [{"uri": workspace.as_uri(), "name": "workspace"}],
            "initializationOptions": options}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        *extra,
        {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ]
    process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, cwd=workspace)
    # Keep stdin open until the scan reports completion, then shut down.
    process.stdin.write(frame(messages[0]) + frame(messages[1]))
    process.stdin.flush()
    responses = []
    done = False
    while not done:
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            chunk = process.stdout.read(1)
            if not chunk:
                raise AssertionError("LSP closed stdout early")
            header += chunk
        length = int(header.decode("ascii").split(":", 1)[1].strip())
        item = json.loads(process.stdout.read(length))
        responses.append(item)
        done = item.get("method") == "window/logMessage" and "workspace scan finished" in item["params"]["message"]
    process.stdin.write(b"".join(frame(m) for m in messages[2:]))
    process.stdin.close()
    process.stdout.read()
    process.wait(timeout=10)
    return responses


def published(responses):
    return {item["params"]["uri"]: item["params"]["diagnostics"]
            for item in responses if item.get("method") == "textDocument/publishDiagnostics"}


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_scan_") as temporary:
    workspace = Path(temporary)
    (workspace / "src").mkdir()
    (workspace / "build").mkdir()
    (workspace / "src" / "bad.cpp").write_text("auto p = NULL;\n", encoding="utf-8")
    (workspace / "build" / "skipped.cpp").write_text("auto p = NULL;\n", encoding="utf-8")
    (workspace / "notes.txt").write_text("auto p = NULL;\n", encoding="utf-8")

    responses = run(workspace, {})
    result = published(responses)
    bad_uri = (workspace / "src" / "bad.cpp").as_uri()
    assert any(item["code"] == "cpp/no-null" for item in result.get(bad_uri, [])), result
    assert len(result) == 1, f"build/ and non-C++ files must be skipped: {list(result)}"
