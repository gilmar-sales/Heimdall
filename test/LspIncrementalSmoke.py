"""didChange keeps the cached lines, tokens and parse tree consistent: after any
sequence of incremental edits (batched, full-sync, breaking and repairing the
syntax) the server answers exactly as it does for a fresh didOpen of that text."""
import json
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

server = sys.argv[1]
workdir = Path(tempfile.mkdtemp(prefix="heimdall-incr-")).resolve()
edited_uri = (workdir / "edited.cpp").as_uri()
fresh_uri = (workdir / "fresh.cpp").as_uri()

process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
inbox = []
lock = threading.Lock()


def reader():
    stream = process.stdout
    while True:
        header_line = b""
        while not header_line.endswith(b"\r\n\r\n"):
            chunk = stream.read(1)
            if not chunk:
                return
            header_line += chunk
        length = int(header_line.decode("ascii").split(":", 1)[1].strip())
        body = stream.read(length)
        with lock:
            inbox.append(json.loads(body))


threading.Thread(target=reader, daemon=True).start()


def send(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    process.stdin.flush()


next_id = [100]


def request(method, params, timeout=8.0):
    next_id[0] += 1
    current = next_id[0]
    send({"jsonrpc": "2.0", "id": current, "method": method, "params": params})
    deadline = time.time() + timeout
    while time.time() < deadline:
        with lock:
            found = next((m for m in inbox if m.get("id") == current), None)
        if found is not None:
            return found.get("result")
        time.sleep(0.01)
    raise SystemExit(f"timeout waiting for {method}")


def last_diagnostics(uri, settle=0.6):
    time.sleep(settle)
    with lock:
        published = [m for m in inbox if m.get("method") == "textDocument/publishDiagnostics" and
                     m["params"]["uri"] == uri]
    assert published, f"no diagnostics for {uri}"
    return sorted((d["range"]["start"]["line"], d["range"]["start"]["character"], d["message"])
                  for d in published[-1]["params"]["diagnostics"])


def apply(text, change):
    if "range" not in change:
        return change["text"]
    lines = text.split("\n")

    def offset(pos):
        return sum(len(l) + 1 for l in lines[:pos["line"]]) + pos["character"]
    start, end = offset(change["range"]["start"]), offset(change["range"]["end"])
    return text[:start] + change["text"] + text[end:]


def rng(l1, c1, l2, c2):
    return {"start": {"line": l1, "character": c1}, "end": {"line": l2, "character": c2}}


text = "".join(f"int f{n}(int a) {{\n    return a + {n};\n}}\n" for n in range(40)) + \
    "int main() {\n    return f3(1) + f20(2);\n}\n"
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
version = [1]
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": edited_uri, "languageId": "cpp", "version": 1, "text": text}}})

steps = [
    [{"range": rng(61, 0, 61, 0), "text": "int added() { return 7; }\n"}],                    # insert an item
    [{"range": rng(1, 11, 1, 12), "text": "b"}],                                              # edit inside a body
    [{"range": rng(4, 4, 4, 4), "text": "x"}, {"range": rng(10, 4, 10, 10), "text": "return"},  # a batch of changes
     {"range": rng(30, 0, 31, 0), "text": ""}],
    [{"range": rng(7, 0, 7, 1), "text": "int ("}],                                            # break the syntax
    [{"range": rng(7, 0, 7, 5), "text": "int"}],                                              # and repair it
    [{"range": rng(20, 0, 22, 1), "text": "int g() {}"}, {"range": rng(0, 0, 0, 0), "text": "// é😀\n"}],
    [{"text": text}],                                                                         # full sync back
    [{"range": rng(2, 1, 2, 1), "text": "}\nint stray() {"}],                                 # unbalanced brace
    [{"range": rng(2, 1, 4, 1), "text": "}"}],
]
current = text
for change_set in steps:
    version[0] += 1
    for change in change_set:
        current = apply(current, change)
    send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
        "textDocument": {"uri": edited_uri, "version": version[0]}, "contentChanges": change_set}})
    # Queries between edits exercise the cached parse (and the reuse of it) at every step.
    request("textDocument/hover", {"textDocument": {"uri": edited_uri}, "position": {"line": 0, "character": 4}})

    send({"jsonrpc": "2.0", "method": "textDocument/didClose", "params": {"textDocument": {"uri": fresh_uri}}})
    send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
        "uri": fresh_uri, "languageId": "cpp", "version": version[0], "text": current}}})
    options = {"tabSize": 4, "insertSpaces": True}
    assert request("textDocument/formatting", {"textDocument": {"uri": edited_uri}, "options": options}) ==         request("textDocument/formatting", {"textDocument": {"uri": fresh_uri}, "options": options}),         f"formatting differs after {change_set}"
    for pos in ({"line": 40 * 3, "character": 20}, {"line": 0, "character": 5}):
        a = request("textDocument/definition", {"textDocument": {"uri": edited_uri}, "position": pos})
        b = request("textDocument/definition", {"textDocument": {"uri": fresh_uri}, "position": pos})
        norm = lambda r: [x["range"] for x in (r or [])]
        assert norm(a) == norm(b), (pos, a, b)

# The edited document's diagnostics match a fresh open of the final text.
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": edited_uri, "version": version[0] + 1},
    "contentChanges": [{"range": rng(0, 0, 0, 0), "text": " "}, {"range": rng(0, 0, 0, 1), "text": ""}]}})
after = last_diagnostics(edited_uri)
assert after == last_diagnostics(fresh_uri, 0), (after, last_diagnostics(fresh_uri, 0))

send({"jsonrpc": "2.0", "id": 9999, "method": "shutdown", "params": None})
send({"jsonrpc": "2.0", "method": "exit"})
process.wait(timeout=10)
print("ok")
