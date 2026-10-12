import json
import pathlib
import subprocess
import sys
import tempfile


server = sys.argv[1]
root = pathlib.Path(tempfile.mkdtemp(prefix="heimdall-index-def-"))
header_uri = (root / "Twice.hpp").as_uri()
source_uri = (root / "Twice.cpp").as_uri()
user_uri = (root / "User.cpp").as_uri()
header_text = "int Twice(int value);\n"
source_text = '#include "Twice.hpp"\nint Twice(int value) { return value * 2; }\n'
user_text = '#include "Twice.hpp"\nint main() { return Twice(4); }\n'


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

    (root / "Twice.hpp").write_text(header_text)
    send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
    send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    fillers = tuple(((root / f"Filler{n}.cpp").as_uri(), "int Other() { return 0; }\n")
                    for n in range(6))
    documents = ((header_uri, header_text), (source_uri, source_text), (user_uri, user_text),
                 *fillers)
    for doc_uri, text in documents:
        send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": doc_uri, "languageId": "cpp", "version": 1, "text": text}}})
    pending = {doc_uri for doc_uri, _ in documents}
    while pending:
        message = read_message(process.stdout)
        if message.get("method") == "textDocument/publishDiagnostics":
            pending.discard(message["params"]["uri"])
    send({"jsonrpc": "2.0", "id": 10, "method": "textDocument/definition", "params": {
        "textDocument": {"uri": user_uri}, "position": {"line": 1, "character": 22}}})
    while True:
        message = read_message(process.stdout)
        if message.get("id") == 10:
            result = message["result"]
            break
    send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}})
    send({"jsonrpc": "2.0", "method": "exit"})
finally:
    process.kill()

assert [item["uri"] for item in result] == [source_uri], result
assert result[0]["range"]["start"]["line"] == 1, result
