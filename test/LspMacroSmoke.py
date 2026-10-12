import json
import subprocess
import sys
import tempfile
from pathlib import Path

# End-to-end: a macro defined in a header (not in the open file) is highlighted as `macro`,
# hovered with its expansion, and navigable to its #define; inactive code is flagged.
server = sys.argv[1]

workspace = Path(tempfile.mkdtemp(prefix="heimdall-macro-"))
header = workspace / "config.h"
header.write_text("#pragma once\n#define LIMIT 8\n#define SQUARE(x) ((x) * (x))\n#define FEATURE 1\n"
                  "#define TEST(a, b) void Test_##a##_##b()\n"
                  "#define LOCK_EXCLUDED(x)\n"
                  "#define DECLARE_FLAG(name) extern bool name;\n"
                  "#define FREYR_NAMESPACE fr\n")
main_cpp = workspace / "main.cpp"
text = ('#include "config.h"\n'
        "int a = LIMIT;\n"
        "int b = SQUARE(LIMIT);\n"
        "#if !FEATURE\n"
        "int dead;\n"
        "#endif\n")
main_cpp.write_text(text)
compile_commands = workspace / "compile_commands.json"
compile_commands.write_text(json.dumps([{
    "directory": workspace.as_posix(),
    "command": f"c++ -std=c++20 -c {main_cpp.as_posix()}",
    "file": main_cpp.as_posix(),
}]))

process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


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


def check(condition, description):
    if not condition:
        process.kill()
        raise SystemExit(f"FAILED: {description}")


initialize = request(1, "initialize", {"capabilities": {
    "workspace": {"workspaceEdit": {"documentChanges": True}}}, "initializationOptions": {
    "enableSemantic": True, "compileCommands": compile_commands.as_posix()}})
provider = initialize["result"]["capabilities"].get("semanticTokensProvider")
check(provider is not None, "semanticTokensProvider is advertised")
types = provider["legend"]["tokenTypes"]
modifiers = provider["legend"]["tokenModifiers"]
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": main_cpp.as_uri(), "languageId": "cpp", "version": 1, "text": text}}})

reply = request(2, "textDocument/semanticTokens/full", {"textDocument": {"uri": main_cpp.as_uri()}})
data = reply["result"]["data"]
tokens, line, char = [], 0, 0
for i in range(0, len(data), 5):
    delta_line, delta_char, length, kind, mods = data[i:i + 5]
    line += delta_line
    char = char + delta_char if delta_line == 0 else delta_char
    tokens.append((line, char, length, types[kind], mods))

macro_uses = [t for t in tokens if t[3] == "macro" and t[0] == 1]
check(any(t[1] == 8 and t[2] == 5 for t in macro_uses), f"LIMIT on line 1 is a macro: {tokens}")
check(any(t[0] == 2 and t[3] == "macro" and t[2] == 6 for t in tokens), "SQUARE on line 2 is a macro")
inactive = 1 << modifiers.index("inactive")
check(any(t[0] == 4 and t[4] & inactive for t in tokens), "code under `#if !FEATURE` is inactive")

hover = request(3, "textDocument/hover", {
    "textDocument": {"uri": main_cpp.as_uri()}, "position": {"line": 2, "character": 10}})
value = hover["result"]["contents"]["value"]
check("#define SQUARE(x) ((x) * (x))" in value, f"hover shows the definition: {value}")
check("((8) * (8))" in value, f"hover shows the expansion: {value}")

definition = request(4, "textDocument/definition", {
    "textDocument": {"uri": main_cpp.as_uri()}, "position": {"line": 1, "character": 9}})
target = definition["result"][0]
check(target["uri"] == header.as_uri(), f"definition points at the header: {target}")
check(target["range"]["start"] == {"line": 1, "character": 8}, f"definition range: {target}")

symbols = request(5, "workspace/symbol", {"query": "SQUARE"})["result"]
check(any(item["name"] == "SQUARE" and item["kind"] == 14 and item["location"]["uri"] == header.as_uri()
          for item in symbols), f"workspace/symbol lists the header macro: {symbols}")

outline = request(6, "textDocument/documentSymbol", {"textDocument": {"uri": main_cpp.as_uri()}})["result"]
check(isinstance(outline, list), "documentSymbol answers")

completion = request(8, "textDocument/completion", {
    "textDocument": {"uri": main_cpp.as_uri()}, "position": {"line": 1, "character": 11}})["result"]
labels = [item["label"] for item in completion["items"]]
check("LIMIT" in labels, f"completion offers header macros: {labels[:10]}")
check("SQUARE" not in labels, "completion filters by prefix")

# Macro-heavy code parses as it compiles: a TEST()-style block, a decoration macro with
# arguments and a declaration macro need no syntax errors.
tests_cpp = workspace / "tests.cpp"
tests_text = ('#include "config.h"\n'
              "DECLARE_FLAG(verbose);\n"
              "struct Widget\n{\n    void Run() LOCK_EXCLUDED(mutex);\n};\n"
              "TEST(Suite, Case)\n{\n    int value = ::std::max(1, 2);\n    (void) value;\n}\n")
tests_cpp.write_text(tests_text)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": tests_cpp.as_uri(), "languageId": "cpp", "version": 1, "text": tests_text}}})
diagnostics = None
while diagnostics is None:
    message = read_message()
    if message.get("method") == "textDocument/publishDiagnostics" and \
            message["params"]["uri"] == tests_cpp.as_uri():
        diagnostics = message["params"]["diagnostics"]
syntax = [d for d in diagnostics if d.get("code") == "syntax/parse-error"]
check(not syntax, f"macro-heavy file has no syntax errors: {syntax}")

# References of a header macro span the header and every user. The header is opened so it is
# indexed as a buffer; the index fills in the background, so ask until it has caught up.
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": header.as_uri(), "languageId": "cpp", "version": 1, "text": header.read_text()}}})
import time
uris = set()
for attempt in range(60):
    references = request(100 + attempt, "textDocument/references", {
        "textDocument": {"uri": main_cpp.as_uri()}, "position": {"line": 1, "character": 10},
        "context": {"includeDeclaration": True}})["result"]
    uris = {item["uri"] for item in references or []}
    if header.as_uri() in uris:
        break
    time.sleep(0.25)
check(uris == {header.as_uri(), main_cpp.as_uri()}, f"references of LIMIT: {uris}")
lines = sorted((item["uri"].rsplit("/", 1)[1], item["range"]["start"]["line"]) for item in references)
check(("main.cpp", 1) in lines and ("main.cpp", 2) in lines and ("config.h", 1) in lines,
      f"every use and the definition: {lines}")

outline = request(9, "textDocument/documentSymbol", {"textDocument": {"uri": tests_cpp.as_uri()}})["result"]
names = [item["name"] for item in outline]
check("TEST(Suite, Case)" in names, f"the TEST() macro is listed in the outline: {names}")

namespace_cpp = workspace / "namespace.cpp"
namespace_text = '#include "config.h"\nnamespace FREYR_NAMESPACE\n{\n} // namespace FREYR_NAMESPACE'
namespace_cpp.write_text(namespace_text)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": namespace_cpp.as_uri(), "languageId": "cpp", "version": 1, "text": namespace_text}}})
eof = {"line": 3, "character": len(namespace_text.splitlines()[-1])}
actions = request(200, "textDocument/codeAction", {
    "textDocument": {"uri": namespace_cpp.as_uri()},
    "range": {"start": eof, "end": eof}, "context": {"diagnostics": []}})["result"]
kinds = [action["kind"] for action in actions]
check("refactor.inline.macro" in kinds, f"macro expansion is offered at EOF: {kinds}")
check("source.fixAll" in kinds, f"final newline fix is offered alongside expansion: {kinds}")
check("quickfix" in kinds, f"individual newline fix is offered: {kinds}")

request(7, "shutdown", None)
send({"jsonrpc": "2.0", "method": "exit"})
process.wait(timeout=10)
print("ok")
