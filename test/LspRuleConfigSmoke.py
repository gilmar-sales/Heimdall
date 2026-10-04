import json
import subprocess
import sys
import tempfile
from pathlib import Path

server = sys.argv[1]


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def run(cwd, initialize_params):
    source_uri = (cwd / "input.cpp").as_uri()
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": initialize_params},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": source_uri, "languageId": "cpp", "version": 1, "text": "auto p = NULL;\n"}}},
        {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ]
    wire = b"".join(frame(message) for message in messages)
    process = subprocess.run([server], input=wire, capture_output=True, cwd=cwd, timeout=10, check=False)
    if process.returncode != 0:
        raise AssertionError(f"LSP exited with {process.returncode}: {process.stderr.decode(errors='replace')}")
    responses = []
    stream = process.stdout
    while stream:
        header, stream = stream.split(b"\r\n\r\n", 1)
        length = int(header.decode("ascii").split(":", 1)[1].strip())
        body, stream = stream[:length], stream[length:]
        responses.append(json.loads(body))
    publication = next(item for item in responses if item.get("method") == "textDocument/publishDiagnostics")
    return publication["params"]["diagnostics"]


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_config_") as temporary:
    temp = Path(temporary)
    workspace = temp / "workspace"
    workspace.mkdir()
    (workspace / ".heimdall.json").write_text(
        '{"root":true,"rules":{"cpp/no-null":"off"}}', encoding="utf-8")
    diagnostics = run(workspace, {"workspaceFolders": [{"uri": workspace.as_uri(), "name": "workspace"}]})
    assert not any(item["code"] == "cpp/no-null" for item in diagnostics), diagnostics

    cwd = temp / "cwd"
    cwd.mkdir()
    (cwd / ".heimdall.json").write_text(
        '{"root":true,"rules":{"cpp/no-null":"off"}}', encoding="utf-8")
    diagnostics = run(cwd, {})
    assert not any(item["code"] == "cpp/no-null" for item in diagnostics), diagnostics
