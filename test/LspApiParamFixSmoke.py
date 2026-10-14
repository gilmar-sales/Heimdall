import json
import subprocess
import sys
import tempfile
from pathlib import Path

server = sys.argv[1]


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


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_param_fix_") as temporary:
    root = Path(temporary)
    include = root / "inc"
    include.mkdir()
    header_text = ("#pragma once\n"
                   "struct Snapshot { int value; };\n"
                   "int Read(Snapshot snapshot);\n")
    (include / "snapshot.hpp").write_text(header_text, encoding="utf-8")
    source = root / "main.cpp"
    text = ("#include <snapshot.hpp>\n"
            "int Read(Snapshot snapshot) {\n"
            "    int y = snapshot.value;\n"
            "    return y;\n"
            "}\n")
    source.write_text(text, encoding="utf-8")
    database = root / "compile_commands.json"
    database.write_text(json.dumps([{
        "directory": str(root),
        "command": f"c++ -I{include} -c main.cpp",
        "file": str(source),
    }]), encoding="utf-8")

    uri = source.as_uri()
    header_uri = (include / "snapshot.hpp").as_uri()
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "initializationOptions": {"enableSemantic": True, "compileCommands": str(database)}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": header_uri, "languageId": "cpp", "version": 1, "text": header_text}}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": uri, "languageId": "cpp", "version": 1, "text": text}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/codeAction", "params": {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 5, "character": 0}},
            "context": {"diagnostics": []}}},
        {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ]
    process = subprocess.run([server], input=b"".join(frame(m) for m in messages),
                             capture_output=True, cwd=root, timeout=60, check=False)
    assert process.returncode == 0, process.stderr.decode(errors="replace")
    responses = parse(process.stdout)

    publications = [r for r in responses if r.get("method") == "textDocument/publishDiagnostics"]
    diagnostics = [d for p in publications if p["params"]["uri"] == uri for d in p["params"]["diagnostics"]]

    iwyu = [d for d in diagnostics if d["code"] == "api/pass-by-const-reference"]
    assert len(iwyu) == 1, diagnostics
    assert "snapshot" in iwyu[0]["message"], iwyu

    actions = next(r for r in responses if r.get("id") == 2)["result"]
    take = [a for a in actions if "by const reference" in a["title"]]
    assert len(take) == 1, actions
    assert take[0]["kind"] == "quickfix"
    changes = take[0]["edit"]["changes"]
    assert set(changes) == {uri, header_uri}, changes

    impl = changes[uri]
    assert len(impl) == 1, impl
    assert impl[0]["newText"] == "const Snapshot& snapshot", impl

    decl = changes[header_uri]
    assert len(decl) == 1, decl
    assert decl[0]["newText"] == "const Snapshot& snapshot", decl
    assert decl[0]["range"]["start"]["line"] == 2, decl
