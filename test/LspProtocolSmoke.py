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
        "uri": uri, "languageId": "cpp", "version": 1, "text": "void f()\n{\nint value=NULL;\n    int unused = NULL;\n}\n"}}},
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

# Request quick fixes with the cursor away from the diagnostic's column,
# on a clean line, and across a selection ending at the next line's start.
for request_id, start, end in [(8, (2, 0), (2, 0)), (9, (3, 0), (3, 0)),
                               (10, (0, 0), (0, 0)), (11, (2, 0), (3, 0)),
                               (12, (2, 0), (3, 15))]:
    messages.insert(-2, {"jsonrpc": "2.0", "id": request_id,
                        "method": "textDocument/codeAction", "params": {
                            "textDocument": {"uri": uri},
                            "range": {"start": {"line": start[0], "character": start[1]},
                                      "end": {"line": end[0], "character": end[1]}},
                            "context": {"diagnostics": []}}})

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
diagnostics = next(item for item in responses
                   if item.get("method") == "textDocument/publishDiagnostics" and item["params"]["uri"] == uri)
codes = {item["code"] for item in diagnostics["params"]["diagnostics"]}
assert {"cpp/modernize-const", "cpp/no-null", "semantic/no-unused-local"} <= codes, diagnostics
formatted = next(item for item in responses if item.get("id") == 2)["result"]
assert formatted[0]["newText"] == "void f()\n{\n    int value = NULL;\n    int unused = NULL;\n}\n"
broken = next(item for item in responses
              if item.get("method") == "textDocument/publishDiagnostics"
              and item["params"]["uri"] == broken_uri)
assert any(item["code"] == "syntax/parse-error" and item["severity"] == 1
           for item in broken["params"]["diagnostics"]), broken
actions = next(item for item in responses if item.get("id") == 3)["result"]
# "Fix all" is the default pick: it comes first and batches the safe fixes.
assert actions[0]["kind"] == "source.fixAll", actions
assert actions[0]["title"].startswith("Fix all Heimdall issues"), actions
fix_all_texts = [e["newText"] for e in actions[0]["edit"]["changes"][uri]]
assert "nullptr" in fix_all_texts, actions
null_actions = [a for a in actions if a["edit"]["changes"][uri][0]["newText"] == "nullptr"]
assert null_actions, actions

def quick_fixes(request_id):
    return [a for a in next(r for r in responses if r.get("id") == request_id)["result"]
            if a["kind"] == "quickfix"]


line_two = quick_fixes(8)
line_three = quick_fixes(9)
assert line_two and line_three, (line_two, line_three)
assert any(a["edit"]["changes"][uri][0]["newText"] == "nullptr" for a in line_two), line_two
for expected_line, fixes in [(2, line_two), (3, line_three)]:
    assert all(e["range"]["start"]["line"] == expected_line
               for a in fixes for e in a["edit"]["changes"][uri]), fixes
assert quick_fixes(10) == [], quick_fixes(10)
assert quick_fixes(11) == line_two, quick_fixes(11)
assert quick_fixes(3) == line_two, quick_fixes(3)
assert {a["title"] for a in quick_fixes(12)} == {a["title"] for a in line_two + line_three}, quick_fixes(12)
# Line 3 is "    int unused = NULL;": (3,11) sits after "unu", so "unused" must be
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
