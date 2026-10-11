import json
import subprocess
import sys


server = sys.argv[1]
uri = "file:///tmp/heimdall-inlay-hints.cpp"
source = (
    "struct Box { int value; };\n"
    "Box makeBox();\n"
    "void f() {\n"
    "    auto scalar = 42;\n"
    "    auto object = makeBox();\n"
    "    struct Local { int member; };\n"
    "    auto local = Local{};\n"
    "}\n"
)


def run(disabled=False):
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "initializationOptions": {"inlayHints": not disabled}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "cpp", "version": 1,
                             "text": source}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/inlayHint", "params": {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0},
                      "end": {"line": 7, "character": 0}}}},
        {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ]
    wire = bytearray()
    for message in messages:
        body = json.dumps(message, separators=(",", ":")).encode()
        wire.extend(f"Content-Length: {len(body)}\r\n\r\n".encode())
        wire.extend(body)
    process = subprocess.run([server], input=wire, capture_output=True,
                             timeout=10, check=False)
    if process.returncode:
        raise AssertionError(process.stderr.decode(errors="replace"))
    stream = process.stdout
    responses = []
    while stream:
        header, stream = stream.split(b"\r\n\r\n", 1)
        length = int(header.decode().split(":", 1)[1].strip())
        body, stream = stream[:length], stream[length:]
        responses.append(json.loads(body))
    return next(item["result"] for item in responses if item.get("id") == 2)


hints = run()
assert [hint["label"] for hint in hints] == [
    ": int", ": Box", ": Local"
], hints
assert [hint["position"]["line"] for hint in hints] == [3, 4, 6], hints
assert all(hint["kind"] == 1 and hint["paddingLeft"] for hint in hints), hints
assert run(disabled=True) == []
