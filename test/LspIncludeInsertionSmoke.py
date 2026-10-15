import json
import subprocess
import sys
import tempfile
from pathlib import Path

server = sys.argv[1]


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def read_message(stream):
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        chunk = stream.read(1)
        if not chunk:
            raise AssertionError("LSP closed stdout early")
        header += chunk
    length = int(header.decode("ascii").split(":", 1)[1].strip())
    return json.loads(stream.read(length))


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_include_insertion_") as temporary:
    root = Path(temporary)
    (root / "include" / "lib").mkdir(parents=True)
    (root / "src").mkdir()
    (root / "include" / "lib" / "widget.hpp").write_text(
        "#pragma once\n"
        "namespace lib {\n"
        "struct Widget { int size; };\n"
        "int widget_count();\n"
        "}\n", encoding="utf-8")
    (root / "src" / "util.hpp").write_text("struct UtilThing {};\n", encoding="utf-8")
    main = root / "src" / "main.cpp"
    main_text = "#include <vector>\n\n[[deprecated]] int WidgetOld; int main() {\n    Wid\n    lib::wid\n    UtilTh\n}\n"
    main.write_text(main_text, encoding="utf-8")
    other = root / "src" / "other.cpp"
    other_text = "#include \"util.hpp\"\nint f() {\n    UtilTh\n}\n"
    other.write_text(other_text, encoding="utf-8")
    database = root / "compile_commands.json"
    database.write_text(json.dumps([
        {"directory": str(root), "file": str(main),
         "command": f"c++ -I{root / 'include'} -c src/main.cpp"},
        {"directory": str(root), "file": str(other),
         "command": f"c++ -I{root / 'include'} -c src/other.cpp"},
    ]), encoding="utf-8")

    process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, cwd=root)

    def send(message):
        process.stdin.write(frame(message))
        process.stdin.flush()

    send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "workspaceFolders": [{"uri": root.as_uri(), "name": "workspace"}],
        "initializationOptions": {"compileCommands": str(database)}}})
    send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    while True:
        item = read_message(process.stdout)
        if item.get("method") == "window/logMessage" and \
                "workspace scan finished" in item["params"]["message"]:
            break

    def complete(request_id, uri, text, line, character):
        send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": uri, "languageId": "cpp", "version": 1, "text": text}}})
        send({"jsonrpc": "2.0", "id": request_id, "method": "textDocument/completion", "params": {
            "textDocument": {"uri": uri}, "position": {"line": line, "character": character}}})
        while True:
            item = read_message(process.stdout)
            if item.get("id") == request_id:
                return item["result"]["items"]

    unqualified = complete(10, main.as_uri(), main_text, 3, 7)
    widget = next((i for i in unqualified if i["label"] == "Widget"), None)
    assert widget is not None, [i["label"] for i in unqualified]
    labels = [item["label"] for item in unqualified]
    assert "WidgetOld" in labels, labels
    assert labels.index("Widget") < labels.index("WidgetOld"), labels
    assert [item["sortText"] for item in unqualified] == sorted(
        item["sortText"] for item in unqualified), unqualified
    assert widget["textEdit"]["newText"] == "lib::Widget", widget
    assert widget["textEdit"]["range"]["start"] == {"line": 3, "character": 4}, widget
    insertion = widget["additionalTextEdits"]
    assert insertion == [{"range": {"start": {"line": 1, "character": 0},
                                    "end": {"line": 1, "character": 0}},
                          "newText": "#include \"lib/widget.hpp\"\n"}], insertion
    # The header in the same directory is spelled relative to the file.
    util = next((i for i in complete(11, main.as_uri(), main_text, 5, 10)
                 if i["label"] == "UtilThing"), None)
    assert util is not None
    assert util["additionalTextEdits"][0]["newText"] == "#include \"util.hpp\"\n", util

    qualified = complete(12, main.as_uri(), main_text, 4, 12)
    count = next((i for i in qualified if i["label"] == "widget_count"), None)
    assert count is not None, [i["label"] for i in qualified]
    assert count["textEdit"]["newText"] == "widget_count", count
    assert count["additionalTextEdits"][0]["newText"] == "#include \"lib/widget.hpp\"\n", count

    # Already included: no second include is offered.
    included = complete(13, other.as_uri(), other_text, 2, 10)
    assert all("additionalTextEdits" not in i for i in included if i["label"] == "UtilThing"), \
        included

    send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}})
    send({"jsonrpc": "2.0", "method": "exit"})
    process.stdin.close()
    process.stdout.read()
    process.wait(timeout=10)

print("include insertion smoke passed")
