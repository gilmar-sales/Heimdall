import json
import subprocess
import sys
from pathlib import Path

server = sys.argv[1]
compile_commands = sys.argv[2]
fixture = Path(sys.argv[3]).resolve()
uri = fixture.as_uri()
broken_uri = uri + ".broken.cpp"
messages = [
    {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "initializationOptions": {"enableSemantic": True, "compileCommands": compile_commands}}},
    {"jsonrpc": "2.0", "method": "initialized", "params": {}},
    {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
        "uri": uri, "languageId": "cpp", "version": 1, "text": "void f()\n{\nint value=NULL;\n    int unused;\n}\n"}}},
    {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
        "uri": broken_uri, "languageId": "cpp", "version": 1,
        "text": "int broken()\n{\n    int value = 1\n    return value;\n}\n"}}},
    {"jsonrpc": "2.0", "id": 2, "method": "textDocument/formatting", "params": {
        "textDocument": {"uri": uri}, "options": {"tabSize": 4, "insertSpaces": True}}},
    {"jsonrpc": "2.0", "id": 3, "method": "textDocument/codeAction", "params": {
        "textDocument": {"uri": uri},
        "range": {"start": {"line": 0, "character": 0}, "end": {"line": 3, "character": 0}},
        "context": {"diagnostics": []}}},
    {"jsonrpc": "2.0", "id": 4, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": uri}, "position": {"line": 3, "character": 11}}},
    {"jsonrpc": "2.0", "id": 5, "method": "textDocument/hover", "params": {
        "textDocument": {"uri": uri}, "position": {"line": 3, "character": 10}}},
    {"jsonrpc": "2.0", "id": 7, "method": "textDocument/rangeFormatting", "params": {
        "textDocument": {"uri": uri},
        "range": {"start": {"line": 2, "character": 0}, "end": {"line": 3, "character": 0}},
        "options": {"tabSize": 4, "insertSpaces": True}}},
    {"jsonrpc": "2.0", "id": 6, "method": "shutdown", "params": {}},
    {"jsonrpc": "2.0", "method": "exit"},
]

wire = bytearray()
for message in messages:
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    wire.extend(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
    wire.extend(body)

process = subprocess.run([server], input=wire, capture_output=True, timeout=10, check=False)
if process.returncode != 0:
    raise SystemExit(f"LSP server exited with {process.returncode}: {process.stderr.decode(errors='replace')}")

responses = []
stream = process.stdout
while stream:
    header, stream = stream.split(b"\r\n\r\n", 1)
    length = int(header.decode("ascii").split(":", 1)[1].strip())
    body, stream = stream[:length], stream[length:]
    responses.append(json.loads(body))

initialize = next(item for item in responses if item.get("id") == 1)
assert initialize["result"]["capabilities"]["documentFormattingProvider"] is True
assert initialize["result"]["capabilities"].get("documentRangeFormattingProvider") is True
assert "completionProvider" in initialize["result"]["capabilities"]
assert initialize["result"]["capabilities"].get("hoverProvider") is True
diagnostics = next(item for item in responses if item.get("method") == "textDocument/publishDiagnostics")
assert diagnostics["params"]["diagnostics"][0]["code"] == "cpp/modernize-const"
assert diagnostics["params"]["diagnostics"][1]["code"] == "cpp/no-null"
formatted = next(item for item in responses if item.get("id") == 2)["result"]
assert formatted[0]["newText"] == "void f()\n{\n    int value = NULL;\n    int unused;\n}\n"
codes = {item["code"] for item in diagnostics["params"]["diagnostics"]}
assert "semantic/no-unused-local" in codes
broken = next(item for item in responses
              if item.get("method") == "textDocument/publishDiagnostics"
              and item["params"]["uri"] == broken_uri)
assert any(item["code"] == "syntax/parse-error" and item["severity"] == 1
           for item in broken["params"]["diagnostics"]), broken
actions = next(item for item in responses if item.get("id") == 3)["result"]
null_actions = [a for a in actions if a["edit"]["changes"][uri][0]["newText"] == "nullptr"]
assert null_actions, actions
# Line 2 is "    int unused;": (2,11) sits after "unu", so "unused" must be
# offered with a textEdit replacing just the typed prefix.
completion = next(item for item in responses if item.get("id") == 4)["result"]
assert completion["isIncomplete"] is False
labels = [item["label"] for item in completion["items"]]
assert "unused" in labels, labels
unused = next(item for item in completion["items"] if item["label"] == "unused")
assert unused["textEdit"]["range"]["start"] == {"line": 3, "character": 8}
assert unused["textEdit"]["range"]["end"] == {"line": 3, "character": 11}
assert unused["textEdit"]["newText"] == "unused"
# Range formatting touches only the requested lines: line 1 is normalized
# while lines 0 and 2 produce no edits.
ranged = next(item for item in responses if item.get("id") == 7)["result"]
assert ranged == [{"range": {"start": {"line": 2, "character": 0},
                             "end": {"line": 3, "character": 0}},
                   "newText": "    int value = NULL;\n"}], ranged
# Hovering the middle of `unused` shows its type line as markdown.
hover = next(item for item in responses if item.get("id") == 5)["result"]
assert hover["contents"]["kind"] == "markdown", hover
assert "unused: int" in hover["contents"]["value"], hover
