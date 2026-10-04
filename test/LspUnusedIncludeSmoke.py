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


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_unused_include_") as temporary:
    root = Path(temporary)
    include = root / "inc"
    include.mkdir()
    (include / "used.hpp").write_text("struct Used {};\n", encoding="utf-8")
    (include / "spare.hpp").write_text("struct Spare {};\n", encoding="utf-8")
    source = root / "main.cpp"
    text = "#include <used.hpp>\n#include <spare.hpp>\nUsed value;\n"
    source.write_text(text, encoding="utf-8")
    database = root / "compile_commands.json"
    database.write_text(json.dumps([{
        "directory": str(root),
        "command": f"c++ -I{include} -c main.cpp",
        "file": str(source),
    }]), encoding="utf-8")

    uri = source.as_uri()
    # Headers are absent from compile databases; the nearest entry is borrowed.
    header = root / "widget.hpp"
    header_text = "#pragma once\n#include <spare.hpp>\nstruct Widget {};\n"
    header.write_text(header_text, encoding="utf-8")
    header_uri = header.as_uri()
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "initializationOptions": {"enableSemantic": True, "compileCommands": str(database)}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": uri, "languageId": "cpp", "version": 1, "text": text}}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": header_uri, "languageId": "cpp", "version": 1, "text": header_text}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/codeAction", "params": {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 3, "character": 0}},
            "context": {"diagnostics": []}}},
        {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ]
    process = subprocess.run([server], input=b"".join(frame(m) for m in messages),
                             capture_output=True, cwd=root, timeout=30, check=False)
    assert process.returncode == 0, process.stderr.decode(errors="replace")
    responses = parse(process.stdout)

    publications = [r for r in responses if r.get("method") == "textDocument/publishDiagnostics"]
    header_diagnostics = [d for p in publications if p["params"]["uri"] == header_uri
                          for d in p["params"]["diagnostics"]]
    assert any(d["code"] == "cpp/no-unused-include" and "<spare.hpp>" in d["message"]
               for d in header_diagnostics), header_diagnostics
    diagnostics = [d for p in publications if p["params"]["uri"] == uri for d in p["params"]["diagnostics"]]
    unused = [d for d in diagnostics if d["code"] == "cpp/no-unused-include"]
    assert len(unused) == 1, diagnostics
    assert "<spare.hpp>" in unused[0]["message"], unused
    assert unused[0]["range"]["start"]["line"] == 1, unused

    actions = next(r for r in responses if r.get("id") == 2)["result"]
    removal = [a for a in actions if "spare.hpp" in a["title"]]
    assert len(removal) == 1, actions
    assert removal[0]["kind"] == "quickfix"
    assert not removal[0].get("isPreferred"), removal
    edit = next(iter(removal[0]["edit"]["changes"].values()))[0]
    assert edit["newText"] == ""
    assert edit["range"]["start"] == {"line": 1, "character": 0}
    assert edit["range"]["end"] == {"line": 2, "character": 0}
