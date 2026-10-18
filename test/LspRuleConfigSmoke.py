import json
import subprocess
import sys
import tempfile
from pathlib import Path

server = sys.argv[1]


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def run(cwd, initialize_params, source="auto p = NULL;\n"):
    source_uri = (cwd / "input.cpp").as_uri()
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": initialize_params},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": source_uri, "languageId": "cpp", "version": 1, "text": source}}},
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

    diagnostics = run(cwd, {"initializationOptions": {"enableSemantic": True}},
                      "void f(int); void g() { f(); }\n")
    assert not any(item["code"] in {"cpp/call-argument-count", "cpp/call-argument-type"}
                   for item in diagnostics), diagnostics

    (cwd / ".heimdall.json").write_text(json.dumps({"root": True, "rules": {
        "cpp/no-null": "off",
        "cpp/call-argument-count": "error",
        "cpp/call-argument-type": "error",
    }}), encoding="utf-8")
    diagnostics = run(cwd, {"initializationOptions": {"enableSemantic": True}},
                      "void test_method(int a, int b, std::string desc) {}\n"
                      "void g() { test_method(1, 2); }\n")
    calls = [item for item in diagnostics if item["code"] == "cpp/call-argument-count"]
    assert len(calls) == 1, diagnostics
    assert calls[0]["severity"] == 1, calls
    assert calls[0]["range"] == {"start": {"line": 1, "character": 11},
                                 "end": {"line": 1, "character": 22}}, calls

    diagnostics = run(cwd, {"initializationOptions": {"enableSemantic": True}},
                      "void test_method(int a, int b, std::string desc) {}\n"
                      "void g() { test_method(1, 2, 1); }\n")
    calls = [item for item in diagnostics if item["code"] == "cpp/call-argument-type"]
    assert len(calls) == 1 and calls[0]["severity"] == 1, diagnostics
    assert calls[0]["range"] == {"start": {"line": 1, "character": 29},
                                 "end": {"line": 1, "character": 30}}, calls

    (cwd / ".heimdall.json").write_text(
        '{"root":true,"rules":{"cpp/call-argument-count":"warning"}}', encoding="utf-8")
    diagnostics = run(cwd, {"initializationOptions": {"enableSemantic": True}},
                      "void f(int); void g() { f(); }\n")
    calls = [item for item in diagnostics if item["code"] == "cpp/call-argument-count"]
    assert len(calls) == 1 and calls[0]["severity"] == 2, diagnostics

    (cwd / ".heimdall.json").write_text(
        '{"root":true,"rules":{"cpp/call-argument-count":"off"}}', encoding="utf-8")
    diagnostics = run(cwd, {"initializationOptions": {"enableSemantic": True}},
                      "void f(int); void g() { f(); }\n")
    assert not any(item["code"] == "cpp/call-argument-count" for item in diagnostics), diagnostics
