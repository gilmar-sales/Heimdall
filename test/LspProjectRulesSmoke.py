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


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_project_rules_") as temporary:
    root = Path(temporary)
    include = root / "inc"
    include.mkdir()
    (include / "core.hpp").write_text("#pragma once\nstruct Core {};\n", encoding="utf-8")
    (include / "shape.hpp").write_text("#pragma once\nstruct Shape { virtual void draw(); };\n",
                                       encoding="utf-8")
    (include / "wrapper.hpp").write_text("#pragma once\n#include <core.hpp>\n#include <shape.hpp>\n",
                                         encoding="utf-8")
    source = root / "main.cpp"
    text = ("#include <wrapper.hpp>\n"
            "#include <shape.hpp>\n"
            "Core value;\n"
            "struct Circle : Shape { void draw() override; };\n")
    source.write_text(text, encoding="utf-8")
    database = root / "compile_commands.json"
    database.write_text(json.dumps([{
        "directory": str(root),
        "command": f"c++ -I{include} -c main.cpp",
        "file": str(source),
    }]), encoding="utf-8")

    uri = source.as_uri()
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "initializationOptions": {"enableSemantic": True, "compileCommands": str(database)}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": uri, "languageId": "cpp", "version": 1, "text": text}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/codeAction", "params": {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 4, "character": 0}},
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

    iwyu = [d for d in diagnostics if d["code"] == "cpp/include-what-you-use"]
    assert len(iwyu) == 1, diagnostics
    assert "core.hpp" in iwyu[0]["message"], iwyu
    assert iwyu[0]["range"]["start"]["line"] == 2, iwyu

    final = [d for d in diagnostics if d["code"] == "cpp/modernize-final"]
    assert len(final) == 1, diagnostics
    assert "Circle" in final[0]["message"], final
    assert final[0]["range"]["start"]["line"] == 3, final

    actions = next(r for r in responses if r.get("id") == 2)["result"]
    add_include = [a for a in actions if "#include <core.hpp>" in a["title"]]
    assert len(add_include) == 1, actions
    assert add_include[0]["kind"] == "quickfix"
    assert not add_include[0].get("isPreferred"), add_include
    edit = next(iter(add_include[0]["edit"]["changes"].values()))[0]
    assert edit["newText"] == "#include <core.hpp>\n", edit
    assert edit["range"]["start"] == {"line": 2, "character": 0}, edit
    assert edit["range"]["end"] == {"line": 2, "character": 0}, edit

    mark_final = [a for a in actions if "final" in a["title"]]
    assert len(mark_final) == 1, actions
    assert not mark_final[0].get("isPreferred"), mark_final
    edit = next(iter(mark_final[0]["edit"]["changes"].values()))[0]
    assert edit["newText"] == " final", edit
