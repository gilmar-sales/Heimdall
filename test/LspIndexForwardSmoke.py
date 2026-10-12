import json
import pathlib
import subprocess
import sys
import tempfile


server = sys.argv[1]
root = pathlib.Path(tempfile.mkdtemp(prefix="heimdall-index-fwd-"))
forward_uri = (root / "Forward.hpp").as_uri()
definition_uri = (root / "Widget.hpp").as_uri()
user_uri = (root / "User.cpp").as_uri()
forward_text = "struct Widget;\n"
definition_text = "struct Widget { int size; };\n"
user_text = '#include "Forward.hpp"\nWidget* Make();\n'


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode()
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body


def read_message(stream):
    length = 0
    while True:
        line = stream.readline()
        if line in (b"\r\n", b""):
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1])
    return json.loads(stream.read(length))


process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
try:
    def send(message):
        process.stdin.write(frame(message))
        process.stdin.flush()

    (root / "Forward.hpp").write_text(forward_text)
    send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
    send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    documents = ((forward_uri, forward_text), (definition_uri, definition_text),
                 (user_uri, user_text))
    for doc_uri, text in documents:
        send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": doc_uri, "languageId": "cpp", "version": 1, "text": text}}})
    pending = {doc_uri for doc_uri, _ in documents}
    while pending:
        message = read_message(process.stdout)
        if message.get("method") == "textDocument/publishDiagnostics":
            pending.discard(message["params"]["uri"])
    send({"jsonrpc": "2.0", "id": 10, "method": "textDocument/definition", "params": {
        "textDocument": {"uri": forward_uri}, "position": {"line": 0, "character": 9}}})
    while True:
        message = read_message(process.stdout)
        if message.get("id") == 10:
            result = message["result"]
            break
    send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}})
    send({"jsonrpc": "2.0", "method": "exit"})
finally:
    process.kill()

assert [item["uri"] for item in result] == [definition_uri], result
