import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# End-to-end: member completion (`obj.`) through a header type, and go-to-definition
# from a *header* that is absent from compile_commands.json (it borrows the flags
# of the nearest entry, otherwise `#include <lib.h>` resolves nothing).
server = sys.argv[1]
include_dir = Path(sys.argv[2]).resolve()

workspace = Path(tempfile.mkdtemp(prefix="heimdall-member-"))
main_cpp = workspace / "main.cpp"
header_user = workspace / "header_user.hpp"
main_cpp.write_text("")
flags = (f'-I"{include_dir.as_posix()}" -DMYLIB_API= -DMYLIB_NOEXCEPT=noexcept '
         '"-DMYLIB_NODISCARD=[[nodiscard]]"')
compile_commands = workspace / "compile_commands.json"
compile_commands.write_text(json.dumps([{
    "directory": workspace.as_posix(),
    "command": f"c++ -std=c++20 {flags} -c {main_cpp.as_posix()}",
    "file": main_cpp.as_posix(),
}]))

process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
pending = []


def send(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    process.stdin.flush()


def read_message():
    length = None
    while True:
        line = process.stdout.readline()
        if not line:
            raise SystemExit(f"server closed stdout: {process.stderr.read().decode(errors='replace')}")
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1])
    return json.loads(process.stdout.read(length))


def request(identifier, method, params):
    send({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params})
    while True:
        message = read_message()
        if message.get("id") == identifier:
            return message
        pending.append(message)


def open_document(path, text):
    send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
        "uri": path.as_uri(), "languageId": "cpp", "version": 1, "text": text}}})


request(1, "initialize", {"initializationOptions": {"compileCommands": compile_commands.as_posix()}})
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

# 1. `s.` on a header alias of a header record that inherits from another record.
text = '#include "mylib/shapes.hpp"\nvoid f()\n{\n    mylib::ShapeAlias s;\n    s.\n}\n'
open_document(main_cpp, text)
labels = []
for attempt in range(60):
    reply = request(100 + attempt, "textDocument/completion", {
        "textDocument": {"uri": main_cpp.as_uri()}, "position": {"line": 4, "character": 6}})
    result = reply["result"]
    labels = [item["label"] for item in result["items"]]
    if not result["isIncomplete"]:
        break
    time.sleep(0.25)

assert not result["isIncomplete"], "header index never completed"
for expected in ("area", "cached", "base_value", "base_run"):
    assert expected in labels, (expected, labels)

# 2. Go-to-definition from a header that is not in compile_commands.json.
header_text = '#include "mylib/shapes.hpp"\nmylib::Shape value;\n'
open_document(header_user, header_text)
reply = request(500, "textDocument/definition", {
    "textDocument": {"uri": header_user.as_uri()}, "position": {"line": 1, "character": 9}})
locations = reply["result"]
assert locations, reply
assert any(location["uri"].endswith("mylib/shapes.hpp") for location in locations), locations

# 3. Hover renders alias origin and layout, and accepts builtin type tokens.
hover_user = workspace / "hover_user.cpp"
hover_text = "struct Point { double x; double y; };\nusing Coord = Point;\nCoord position;\nbool json = false;\n"
open_document(hover_user, hover_text)
reply = request(600, "textDocument/hover", {
    "textDocument": {"uri": hover_user.as_uri()}, "position": {"line": 2, "character": 2}})
value = reply["result"]["contents"]["value"]
assert "`<Point>`" in value, value
assert "**Size:** 16 bytes" in value and "**Align:** 8 bytes" in value, value
reply = request(601, "textDocument/hover", {
    "textDocument": {"uri": hover_user.as_uri()}, "position": {"line": 3, "character": 1}})
value = reply["result"]["contents"]["value"]
assert "**Size:** 1 bytes" in value and "**Align:** 1 bytes" in value, value

request(900, "shutdown", {})
send({"jsonrpc": "2.0", "method": "exit"})
process.wait(timeout=20)
assert process.returncode == 0, process.returncode
