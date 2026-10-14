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


def code_actions(root, sources, main):
    database = root / "compile_commands.json"
    database.write_text(json.dumps([{
        "directory": str(root),
        "command": f"c++ -I{root / 'inc'} -c {main}",
        "file": str(root / main),
    }]), encoding="utf-8")
    uri = (root / main).as_uri()
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "rootUri": root.as_uri(),
            "initializationOptions": {"enableSemantic": True, "compileCommands": str(database)}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
    ]
    for name, text in sources.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        messages.append({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": path.as_uri(), "languageId": "cpp", "version": 1,
                             "text": text}}})
    messages += [
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/codeAction", "params": {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0},
                      "end": {"line": 100, "character": 0}},
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
    actions = next(r for r in responses if r.get("id") == 2)["result"]
    return diagnostics, actions


def apply_all(files, changes):
    for target, edits in changes.items():
        text = files[target]
        for edit in sorted(edits, key=lambda e: (e["range"]["start"]["line"],
                                                 e["range"]["start"]["character"]), reverse=True):
            lines = text.split("\n")
            a, b = edit["range"]["start"], edit["range"]["end"]
            assert a["line"] == b["line"], edit
            line = lines[a["line"]]
            lines[a["line"]] = line[:a["character"]] + edit["newText"] + line[b["character"]:]
            text = "\n".join(lines)
        files[target] = text


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_param_fix_") as temporary:
    # A by-value parameter that is only read: the fix must rewrite the
    # definition and the header declaration with the same spelling.
    root = Path(temporary) / "const-ref"
    root.mkdir()
    (root / "inc").mkdir()
    header_text = ("#pragma once\n"
                   "struct Snapshot;\n"
                   "int Read(Snapshot snapshot);\n")
    text = ("#include <snapshot.hpp>\n"
            "struct Snapshot { int value; };\n"
            "int Read(Snapshot snapshot) {\n"
            "    int y = snapshot.value;\n"
            "    return y;\n"
            "}\n")
    diagnostics, actions = code_actions(root, {"inc/snapshot.hpp": header_text,
                                               "main.cpp": text}, "main.cpp")
    uri = (root / "main.cpp").as_uri()
    header_uri = (root / "inc" / "snapshot.hpp").as_uri()
    found = [d for d in diagnostics if d["code"] == "api/pass-by-const-reference"]
    assert len(found) == 1, diagnostics
    take = [a for a in actions if "by const reference" in a["title"]]
    assert len(take) == 1, actions
    assert take[0]["kind"] == "quickfix"
    assert set(take[0]["edit"]["changes"]) == {uri, header_uri}, take[0]
    assert take[0]["edit"]["changes"][uri][0]["newText"] == "const Snapshot& snapshot"
    assert take[0]["edit"]["changes"][header_uri][0]["newText"] == "const Snapshot& snapshot"
    files = {uri: text, header_uri: header_text}
    apply_all(files, take[0]["edit"]["changes"])
    assert files[uri] == ("#include <snapshot.hpp>\n"
                          "struct Snapshot { int value; };\n"
                          "int Read(const Snapshot& snapshot) {\n"
                          "    int y = snapshot.value;\n"
                          "    return y;\n"
                          "}\n"), files[uri]
    assert files[header_uri] == ("#pragma once\n"
                                 "struct Snapshot;\n"
                                 "int Read(const Snapshot& snapshot);\n"), files[header_uri]

with tempfile.TemporaryDirectory(prefix="heimdall_lsp_param_fix_value_") as temporary:
    # A sink constructor taking `const T&` but copying: same requirement in
    # the other direction, reaching the in-class declaration through the
    # qualified-name fallback (its hash differs from the definition's).
    root = Path(temporary)
    (root / "inc").mkdir()
    header_text = ("#pragma once\n"
                   "#include <string>\n"
                   "struct Holder {\n"
                   "    std::string name_;\n"
                   "    Holder(const std::string& name);\n"
                   "};\n")
    text = ("#include <holder.hpp>\n"
            "Holder::Holder(const std::string& name) : name_(name) {}\n")
    diagnostics, actions = code_actions(root, {"inc/holder.hpp": header_text,
                                               "main.cpp": text}, "main.cpp")
    uri = (root / "main.cpp").as_uri()
    header_uri = (root / "inc" / "holder.hpp").as_uri()
    found = [d for d in diagnostics if d["code"] == "api/pass-by-value"]
    assert len(found) == 1, diagnostics
    take = [a for a in actions if "by value" in a["title"]]
    assert len(take) == 1, actions
    assert set(take[0]["edit"]["changes"]) == {uri, header_uri}, take[0]
    assert take[0]["edit"]["changes"][uri][0]["newText"] == "std::string name"
    assert take[0]["edit"]["changes"][header_uri][0]["newText"] == "std::string name"
