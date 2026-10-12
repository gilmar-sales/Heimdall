import json
import pathlib
import subprocess
import sys
import tempfile


server = sys.argv[1]
root = pathlib.Path(tempfile.mkdtemp(prefix="heimdall-index-refs-"))
header_uri = (root / "Widget.hpp").as_uri()
source_uri = (root / "Widget.cpp").as_uri()
user_uri = (root / "User.cpp").as_uri()
header_text = "struct Widget {};\n"
source_text = '#include "Widget.hpp"\nint Area(const Widget& widget);\n'
user_text = '#include "Widget.hpp"\nint main() { Widget widget; return 0; }\n'


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

    (root / "Widget.hpp").write_text(header_text)
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
    send({"jsonrpc": "2.0", "id": 10, "method": "textDocument/references", "params": {
        "textDocument": {"uri": header_uri}, "position": {"line": 0, "character": header_text.index("Widget") + 1},
        "context": {"includeDeclaration": True}}})
    while True:
        message = read_message(process.stdout)
        if message.get("id") == 10:
            result = message["result"]
            break
    send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}})
    send({"jsonrpc": "2.0", "method": "exit"})
finally:
    process.kill()

uris = sorted(item["uri"] for item in result)
assert uris == sorted([header_uri, source_uri, user_uri]), result
