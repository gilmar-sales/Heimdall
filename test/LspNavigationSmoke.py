"""End-to-end check of go-to-definition / go-to-implementation and the
using-directive ambiguity diagnostic over real files on disk."""
import json
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

server = sys.argv[1]

workdir = Path(tempfile.mkdtemp(prefix="heimdall-nav-")).resolve()
header = workdir / "lib.hpp"
source = workdir / "lib.cpp"
main = workdir / "main.cpp"
header.write_text(
    "namespace lib {\nint answer;\nvoid greet(int times);\n"
    "enum class Mode { Fast, Slow };\nstruct Widget { void run(); };\n}\n", encoding="utf-8")
source_text = '#include "lib.hpp"\nnamespace lib {\nvoid greet(int times) {}\nvoid Widget::run() {}\n}\n'
source.write_text(source_text, encoding="utf-8")
main_text = (
    '#include "lib.hpp"\n'
    "namespace a { int x; }\n"
    "namespace b { int x; }\n"
    "using namespace lib;\n"
    "using namespace a;\n"
    "using namespace b;\n"
    "int f() {\n"
    "    greet(1);\n"
    "    return answer + x;\n"
    "    lib::Mode mode = lib::Mode::Fast;\n"
    "    lib::Widget widget;\n"
    "    widget.run();\n"
    "}\n"
)
main.write_text(main_text, encoding="utf-8")
main_uri = main.as_uri()

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


next_id = [10]


def request(method, params, timeout=8.0, until=None):
    """Send a request; with `until`, re-send until the predicate accepts the result
    (the header index is built on a background worker)."""
    deadline = time.time() + timeout
    while True:
        next_id[0] += 1
        current = next_id[0]
        send({"jsonrpc": "2.0", "id": current, "method": method, "params": params})
        while time.time() < deadline:
            with lock:
                found = next((m for m in inbox if m.get("id") == current), None)
            if found is not None:
                break
            time.sleep(0.01)
        else:
            raise SystemExit(f"timeout waiting for {method}")
        result = found.get("result")
        if until is None or until(result) or time.time() >= deadline:
            return result
        time.sleep(0.1)


def wait_diagnostics(predicate, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with lock:
            for message in inbox:
                if message.get("method") == "textDocument/publishDiagnostics" and \
                        message["params"]["uri"] == main_uri and predicate(message["params"]["diagnostics"]):
                    return message["params"]["diagnostics"]
        time.sleep(0.05)
    return None


def position(line, text, needle, nth=0):
    line_text = text.split("\n")[line]
    index = -1
    for _ in range(nth + 1):
        index = line_text.index(needle, index + 1)
    return {"line": line, "character": index}


send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": main_uri, "languageId": "cpp", "version": 1, "text": main_text}}})

caps = None
for _ in range(200):
    with lock:
        caps = next((m for m in inbox if m.get("id") == 1), None)
    if caps:
        break
    time.sleep(0.02)
assert caps is not None, "no initialize response"
capabilities = caps["result"]["capabilities"]
assert capabilities.get("definitionProvider") is True, capabilities
assert capabilities.get("implementationProvider") is True, capabilities

doc = {"uri": main_uri}

# `answer` is declared in lib.hpp and visible through `using namespace lib`.
answer = request("textDocument/definition",
                 {"textDocument": doc, "position": position(8, main_text, "answer")},
                 until=lambda result: bool(result))
assert answer and answer[0]["uri"] == header.as_uri(), answer
assert answer[0]["range"]["start"] == {"line": 1, "character": 4}, answer

# `greet` is declared in lib.hpp; its body lives in lib.cpp (same stem).
greet = request("textDocument/definition",
                {"textDocument": doc, "position": position(7, main_text, "greet")},
                until=lambda result: bool(result) and result[0]["uri"] == source.as_uri())
assert greet and greet[0]["uri"] == source.as_uri(), greet
assert greet[0]["range"]["start"] == {"line": 2, "character": 5}, greet

# Go to implementation resolves to the same body.
implementation = request("textDocument/implementation",
                         {"textDocument": doc, "position": position(7, main_text, "greet")},
                         until=lambda result: bool(result))
assert any(item["uri"] == source.as_uri() for item in implementation), implementation

# `x` is brought in by both `using namespace a;` and `using namespace b;`.
diagnostics = wait_diagnostics(lambda items: any(item.get("code") == "semantic/ambiguous-reference" for item in items))
assert diagnostics is not None, "ambiguity diagnostic missing"
ambiguity = next(item for item in diagnostics if item.get("code") == "semantic/ambiguous-reference")
assert "'x' is ambiguous" in ambiguity["message"], ambiguity
assert "a::x" in ambiguity["message"] and "b::x" in ambiguity["message"], ambiguity
assert ambiguity["severity"] == 1, ambiguity
assert ambiguity["range"]["start"]["line"] == 8, ambiguity

# Both candidates are offered by go-to-definition.
both = request("textDocument/definition",
               {"textDocument": doc, "position": position(8, main_text, "x")})
assert len(both) == 2, both

# An enum used with a qualifier jumps to its name in the header (not the keyword).
mode = request("textDocument/definition",
               {"textDocument": doc, "position": position(9, main_text, "Mode")},
               until=lambda result: bool(result))
assert len(mode) == 1 and mode[0]["uri"] == header.as_uri(), mode
assert mode[0]["range"]["start"] == {"line": 3, "character": 11}, mode
assert mode[0]["range"]["end"] == {"line": 3, "character": 15}, mode
enumerator = request("textDocument/definition",
                     {"textDocument": doc, "position": position(9, main_text, "Fast")})
assert len(enumerator) == 1 and enumerator[0]["range"]["start"] == {"line": 3, "character": 18}, enumerator

# Method: use -> body in the .cpp; implementation of the header declaration is
# only the body (no declaration mixed in); the body jumps back to the declaration.
run_def = request("textDocument/definition",
                  {"textDocument": doc, "position": position(11, main_text, "run")},
                  until=lambda result: bool(result) and result[0]["uri"] == source.as_uri())
assert run_def == [{"uri": source.as_uri(), "range": {"start": {"line": 3, "character": 13},
                                                       "end": {"line": 3, "character": 16}}}], run_def
header_doc = {"uri": header.as_uri()}
header_text = header.read_text(encoding="utf-8")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": header.as_uri(), "languageId": "cpp", "version": 1, "text": header_text}}})
run_impl = request("textDocument/implementation",
                   {"textDocument": header_doc, "position": position(4, header_text, "run")},
                   until=lambda result: bool(result))
assert run_impl == run_def, run_impl
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": source.as_uri(), "languageId": "cpp", "version": 1, "text": source_text}}})
back = request("textDocument/definition",
               {"textDocument": {"uri": source.as_uri()}, "position": position(3, source_text, "run")},
               until=lambda result: bool(result) and result[0]["uri"] == header.as_uri())
assert back == [{"uri": header.as_uri(), "range": {"start": {"line": 4, "character": 21},
                                                    "end": {"line": 4, "character": 24}}}], back

send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}})
send({"jsonrpc": "2.0", "method": "exit"})
try:
    process.wait(timeout=5)
except subprocess.TimeoutExpired:
    process.kill()
