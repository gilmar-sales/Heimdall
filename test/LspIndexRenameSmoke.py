"""Rename of a class declared in one file and used in two others, then refusals."""
import json
import pathlib
import subprocess
import sys
import tempfile
import threading


root = pathlib.Path(tempfile.mkdtemp(prefix="heimdall-index-rename-"))
files = {
    "test/fixtures/parse_syntax_error.cpp": "int broken(\n",
    "Alone.hpp": "struct Alone {};\n",
    "Widget.hpp": "struct Widget\n{\n    Widget();\n    int Size() const;\n};\n",
    "Widget.cpp": '#include "Widget.hpp"\nWidget::Widget() {}\nint Widget::Size() const { return 1; }\n',
    "User.cpp": '#include "Widget.hpp"\nint Use(const Widget& widget) { return widget.Size(); }\n',
}
for name, text in files.items():
    (root / name).parent.mkdir(parents=True, exist_ok=True)
    (root / name).write_bytes(text.encode("utf-8"))
    assert (root / name).read_bytes() == text.encode("utf-8")

process = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
condition = threading.Condition()
inbox = []


def reader():
    while True:
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            char = process.stdout.read(1)
            if not char:
                return
            header += char
        size = int(header.split(b":", 1)[1].strip())
        message = json.loads(process.stdout.read(size))
        with condition:
            inbox.append(message)
            condition.notify_all()


threading.Thread(target=reader, daemon=True).start()
next_id = 0


def send(method, params, request=False):
    global next_id
    message = {"jsonrpc": "2.0", "method": method, "params": params}
    if request:
        next_id += 1
        message["id"] = next_id
    data = json.dumps(message).encode()
    process.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)
    process.stdin.flush()
    if not request:
        return None
    with condition:
        assert condition.wait_for(lambda: any(m.get("id") == next_id for m in inbox), timeout=20), method
        return next(m for m in inbox if m.get("id") == next_id)


def wait_for(predicate, what):
    with condition:
        assert condition.wait_for(lambda: any(predicate(m) for m in inbox), timeout=20), what


try:
    send("initialize", {"rootUri": root.as_uri(), "capabilities": {
        "workspace": {"workspaceEdit": {"documentChanges": True}}}}, True)
    send("initialized", {})
    wait_for(lambda m: m.get("method") == "window/logMessage" and "symbol index ready" in m["params"]["message"],
             "index ready")
    uri = (root / "Widget.hpp").as_uri()
    send("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "cpp", "version": 1,
                                                   "text": files["Widget.hpp"]}})
    wait_for(lambda m: m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == uri,
             "diagnostics")
    prepared = send("textDocument/prepareRename", {"textDocument": {"uri": uri},
                                                    "position": {"line": 0, "character": 8}}, True)
    assert "error" not in prepared, prepared
    assert prepared["result"]["placeholder"] == "Widget", prepared
    constructor = send("textDocument/prepareRename", {"textDocument": {"uri": uri},
                                                       "position": {"line": 2, "character": 6}}, True)
    assert "error" not in constructor, constructor
    assert constructor["result"]["placeholder"] == "Widget", constructor
    response = send("textDocument/rename", {"textDocument": {"uri": uri},
                                            "position": {"line": 0, "character": 8},
                                            "newName": "Gadget"}, True)
    assert "error" not in response, response
    changes = {pathlib.Path(change["textDocument"]["uri"][7:]).name: change["edits"]
               for change in response["result"]["documentChanges"]}
    assert sorted(changes) == ["User.cpp", "Widget.cpp", "Widget.hpp"], sorted(changes)
    assert len(changes["Widget.hpp"]) == 2, changes["Widget.hpp"]
    assert len(changes["Widget.cpp"]) == 3, changes["Widget.cpp"]
    assert len(changes["User.cpp"]) == 1, changes["User.cpp"]
    assert all(edit["newText"] == "Gadget" for edits in changes.values() for edit in edits)

    alone_uri = (root / "Alone.hpp").as_uri()
    send("textDocument/didOpen", {"textDocument": {"uri": alone_uri, "languageId": "cpp", "version": 1,
                                                    "text": files["Alone.hpp"]}})
    wait_for(lambda m: m.get("method") == "textDocument/publishDiagnostics" and m["params"]["uri"] == alone_uri,
             "single-file diagnostics")
    alone = send("textDocument/rename", {"textDocument": {"uri": alone_uri},
                                          "position": {"line": 0, "character": 8}, "newName": "Independent"}, True)
    assert "error" not in alone, alone
    assert len(alone["result"]["documentChanges"]) == 1, alone

    refused = send("textDocument/rename", {"textDocument": {"uri": uri},
                                           "position": {"line": 0, "character": 8},
                                           "newName": "class"}, True)
    assert "error" in refused, refused
    send("shutdown", {}, True)
    send("exit", {})
finally:
    process.kill()
