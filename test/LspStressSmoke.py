"""Sustained concurrent use: bursts of didChange racing completion/hover/goto/format
requests and cancellations must leave the server alive and answering, and shutdown
must still complete (regression: the server stopped responding after some uses)."""
import json
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

server = sys.argv[1]
workdir = Path(tempfile.mkdtemp(prefix="heimdall-stress-")).resolve()
uri = (workdir / "stress.cpp").as_uri()

process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
inbox = {}
lock = threading.Lock()


def reader():
    stream = process.stdout
    while True:
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            chunk = stream.read(1)
            if not chunk:
                return
            header += chunk
        length = int(header.decode("ascii").split(":", 1)[1].strip())
        message = json.loads(stream.read(length))
        if "id" in message:
            with lock:
                inbox[message["id"]] = message


threading.Thread(target=reader, daemon=True).start()


def send(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    process.stdin.flush()


def wait_for(ids, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with lock:
            if all(i in inbox for i in ids):
                return
        if process.poll() is not None:
            raise SystemExit(f"server exited early with code {process.returncode}")
        time.sleep(0.01)
    with lock:
        missing = [i for i in ids if i not in inbox]
    raise SystemExit(f"server stopped answering; missing responses {missing[:5]}")


send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
wait_for([1])
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
source = "#include <vector>\nstruct S { int a; void f(); };\nint main() { S s; s.a = 1; return s.a; }\n"
send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
      "params": {"textDocument": {"uri": uri, "languageId": "cpp", "version": 1, "text": source}}})

next_id = 100
version = 1
for round_index in range(40):
    ids = []
    version += 1
    send({"jsonrpc": "2.0", "method": "textDocument/didChange",
          "params": {"textDocument": {"uri": uri, "version": version},
                     "contentChanges": [{"range": {"start": {"line": 1, "character": 0},
                                                   "end": {"line": 1, "character": 0}},
                                         "text": f"int v{round_index};\n"}]}})
    for method, extra in (("textDocument/completion", {}), ("textDocument/hover", {}),
                          ("textDocument/definition", {}), ("textDocument/formatting", {"options": {}}),
                          ("textDocument/codeAction", {"range": {"start": {"line": 0, "character": 0},
                                                                 "end": {"line": 0, "character": 0}},
                                                       "context": {"diagnostics": []}})):
        next_id += 1
        ids.append(next_id)
        params = {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 22}}
        params.update(extra)
        send({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params})
    send({"jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": ids[0]}})
    wait_for(ids)

next_id += 1
send({"jsonrpc": "2.0", "id": next_id, "method": "shutdown"})
wait_for([next_id])
send({"jsonrpc": "2.0", "method": "exit"})
try:
    process.wait(timeout=10)
except subprocess.TimeoutExpired:
    process.kill()
    raise SystemExit("server did not exit after shutdown")
print("ok")
