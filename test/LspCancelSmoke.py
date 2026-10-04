import json
import subprocess
import sys
import time

# A request is cancelled while it runs: $/cancelRequest must be read by the I/O
# thread immediately (requests run on the worker pool) and the request must
# answer RequestCancelled (-32800). Also checks a later request is still served.
server = sys.argv[1]
uri = "file:///cancel_smoke.cpp"
big = "".join(f"int f{i}(int a) {{ return a + {i}; }}\n" for i in range(150000))
small_uri = "file:///small.cpp"


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def send(message):
    process.stdin.write(frame(message))
    process.stdin.flush()


send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": uri, "languageId": "cpp", "version": 1, "text": big}}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": small_uri, "languageId": "cpp", "version": 1, "text": "int g() { return 1; }\n"}}})
send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": {"line": 10, "character": 3}}})
send({"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": 2}})
# Sent while the big request is still in flight: must not queue behind it.
send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": small_uri}, "position": {"line": 0, "character": 5}}})
send({"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": {}})
send({"jsonrpc": "2.0", "method": "exit"})

out, err = process.communicate(timeout=60)
if process.returncode != 0:
    raise SystemExit(f"LSP server exited with {process.returncode}: {err.decode(errors='replace')}")

responses = []
stream = out
while stream:
    header, stream = stream.split(b"\r\n\r\n", 1)
    length = int(header.decode("ascii").split(":", 1)[1].strip())
    body, stream = stream[:length], stream[length:]
    responses.append(json.loads(body))

cancelled = next(item for item in responses if item.get("id") == 2)
assert cancelled.get("error", {}).get("code") == -32800, cancelled
assert any(item.get("id") == 3 for item in responses), responses
assert any(item.get("id") == 4 for item in responses), responses
