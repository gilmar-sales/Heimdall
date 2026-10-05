"""FIFO snapshot pins, suspended parse consumers, cancellation and single replies."""
import json
import os
import subprocess
import sys
import threading
import time

env = dict(os.environ, HEIMDALL_LSP_THREADS="3")
process = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, env=env)
messages = []
condition = threading.Condition()


def reader():
    while True:
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            byte = process.stdout.read(1)
            if not byte:
                return
            header += byte
        length = int(header.split(b":", 1)[1].strip())
        message = json.loads(process.stdout.read(length))
        with condition:
            messages.append(message)
            condition.notify_all()


threading.Thread(target=reader, daemon=True).start()


def send(method, params=None, request_id=None):
    message = {"jsonrpc": "2.0", "method": method, "params": params or {}}
    if request_id is not None:
        message["id"] = request_id
    body = json.dumps(message, separators=(",", ":")).encode()
    process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    process.stdin.flush()


def reply(request_id, timeout=30):
    with condition:
        assert condition.wait_for(lambda: any(m.get("id") == request_id for m in messages), timeout), request_id
        found = [m for m in messages if m.get("id") == request_id]
        assert len(found) == 1, found
        return found[0]


def hover(uri, request_id, character=5):
    send("textDocument/hover", {"textDocument": {"uri": uri},
                              "position": {"line": 0, "character": character}}, request_id)


def open_document(uri, text, version=1):
    send("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "cpp", "version": version, "text": text}})


try:
    send("initialize", {"initializationOptions": {"workspaceScan": False}}, 1)
    reply(1)
    uri = "file:///snapshot_order.cpp"
    # Absence is pinned too: this request must not observe the future didOpen.
    hover(uri, 2)
    open_document(uri, "int before;\n")
    hover(uri, 3)
    send("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2},
                                   "contentChanges": [{"text": "double after;\n"}]})
    hover(uri, 4, 8)
    send("textDocument/didClose", {"textDocument": {"uri": uri}})
    hover(uri, 5)
    open_document(uri, "float reopened;\n", 3)
    hover(uri, 6, 8)
    assert reply(2).get("result") is None
    assert "before" in json.dumps(reply(3).get("result")), reply(3)
    assert "after" in json.dumps(reply(4).get("result")), reply(4)
    assert reply(5).get("result") is None
    assert "reopened" in json.dumps(reply(6).get("result")), reply(6)

    big_uri = "file:///shared_parse.cpp"
    big = "int shared;\n" + "".join(f"int f{i}() {{ return {i}; }}\n" for i in range(25000))
    open_document(big_uri, big)
    # More consumers than interactive workers. Cancelling several must not
    # cancel the common parse needed by the remaining consumers.
    for request_id in range(10, 22):
        hover(big_uri, request_id)
    for request_id in range(10, 20):
        send("$/cancelRequest", {"id": request_id})
    hover(uri, 22, 8)
    assert "reopened" in json.dumps(reply(22).get("result"))
    for request_id in range(10, 20):
        assert reply(request_id).get("error", {}).get("code") == -32800, reply(request_id)
    for request_id in (20, 21):
        assert "shared" in json.dumps(reply(request_id).get("result")), reply(request_id)
    send("shutdown", request_id=30)
    reply(30)
    # Shutdown drains suspended continuations: no duplicate late replies.
    for request_id in [2, 3, 4, 5, 6, *range(10, 23)]:
        assert len([m for m in messages if m.get("id") == request_id]) == 1, request_id
    send("exit")
    process.wait(timeout=10)
    assert process.returncode == 0
finally:
    if process.poll() is None:
        process.kill()
        process.wait()
