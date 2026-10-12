import json
import subprocess
import sys
import tempfile
from pathlib import Path


server = sys.argv[1]
uri = "file:///tmp/heimdall-inlay-hints.cpp"
source = (
    "struct Box { int value; };\n"
    "Box makeBox();\n"
    "using BoxAlias = Box;\n"
    "namespace qualified { struct Item { int value; }; }\n"
    "struct Pair { int first; int second; };\n"
    "Pair makePair();\n"
    "int values[2] = {1, 2};\n"
    "struct Local { int member; };\n"
    "void f() {\n"
    "    auto scalar = 42;\n"
    "    int explicitValue = 3;\n"
    "    auto object = makeBox();\n"
    "    auto local = Local{};\n"
    "    auto alias = BoxAlias{};\n"
    "    auto qualified = qualified::Item{};\n"
    "    auto* pointer = new Box;\n"
    "    auto& reference = scalar;\n"
    "    auto [first, second] = makePair();\n"
    "    auto [arrayFirst, arraySecond] = values;\n"
    "    auto [invalidFirst, invalidSecond];\n"
    "    auto invalid = unknown_type{};\n"
    "    auto invalidPointer = (unknown_ptr*)nullptr;\n"
    "}\n"
)


def run(disabled=False, text=source, hint_range=None, initialization_options=None,
        document_uri=uri):
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "initializationOptions": {"inlayHints": not disabled,
                                      **(initialization_options or {})}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": document_uri, "languageId": "cpp", "version": 1,
                             "text": text}}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/inlayHint", "params": {
            "textDocument": {"uri": document_uri},
            "range": hint_range or {"start": {"line": 7, "character": 0},
                                    "end": {"line": 22, "character": 0}}}},
        {"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ]
    if initialization_options:
        process = subprocess.Popen([server], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        def send(message):
            body = json.dumps(message, separators=(",", ":")).encode()
            process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
            process.stdin.flush()

        def response(identifier):
            while True:
                length = None
                while True:
                    line = process.stdout.readline()
                    assert line, "server closed stdout"
                    if line == b"\r\n":
                        break
                    if line.lower().startswith(b"content-length:"):
                        length = int(line.split(b":", 1)[1])
                message = json.loads(process.stdout.read(length))
                if message.get("id") == identifier:
                    return message["result"]

        try:
            send(messages[0])
            response(1)
            send(messages[1])
            send(messages[2])
            for attempt in range(60):
                identifier = 100 + attempt
                send({"jsonrpc": "2.0", "id": identifier,
                      "method": "textDocument/completion", "params": {
                          "textDocument": {"uri": document_uri},
                          "position": {"line": 2, "character": 0}}})
                if not response(identifier)["isIncomplete"]:
                    break
            else:
                raise AssertionError("header index did not finish")
            send(messages[3])
            result = response(2)
            send(messages[4])
            response(3)
            send(messages[5])
            process.wait(timeout=10)
            assert process.returncode == 0
            return result
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    wire = bytearray()
    for message in messages:
        body = json.dumps(message, separators=(",", ":")).encode()
        wire.extend(f"Content-Length: {len(body)}\r\n\r\n".encode())
        wire.extend(body)
    process = subprocess.run([server], input=wire, capture_output=True,
                             timeout=10, check=False)
    if process.returncode:
        raise AssertionError(process.stderr.decode(errors="replace"))
    stream = process.stdout
    responses = []
    while stream:
        header, stream = stream.split(b"\r\n\r\n", 1)
        length = int(header.decode().split(":", 1)[1].strip())
        body, stream = stream[:length], stream[length:]
        responses.append(json.loads(body))
    return next(item["result"] for item in responses if item.get("id") == 2)


hints = run()
actual = [(hint["position"]["line"], hint["label"]) for hint in hints]
assert actual == [
    (9, ": int"),
    (11, ": Box"),
    (12, ": Local"),
    (13, ": Box"),
    (14, ": Item"),
    (15, ": Box*"),
    (16, ": int&"),
    (17, ": int"),
    (17, ": int"),
    (18, ": int"),
    (18, ": int"),
], hints
assert all(hint["kind"] == 1 and hint["paddingLeft"] for hint in hints), hints
assert run(disabled=True) == []

parameter_source = (
    "int MetodoTeste(unsigned int first, int second, const char* desc);\n"
    "template<class T> struct List { void Push(const T& value); };\n"
    "int Inner(int number);\n"
    "void Outer(int left, int right);\n"
    "void Unnamed(int, const char*);\n"
    "void Ambiguous(int count);\n"
    "void Ambiguous(double length);\n"
    "void Variadic(int count, ...);\n"
    "void Defaulted(int count, int limit = 10);\n"
    "void Redeclared(int);\n"
    "void Redeclared(int amount);\n"
    "void f() {\n"
    "    MetodoTeste(1, 2, \"\");\n"
    "    auto list = List<int>();\n"
    "    list.Push(2);\n"
    "    Outer(Inner(3), (4 + 5));\n"
    "    int first = 1;\n"
    "    MetodoTeste(first, 2, \"\");\n"
    "    Unnamed(1, \"\");\n"
    "    Ambiguous(1);\n"
    "    Variadic(1, 2, 3);\n"
    "    Defaulted(2);\n"
    "    Redeclared(3);\n"
    "    MetodoTeste(\n"
    "        1,\n"
    "        2,\n"
    "        \"\");\n"
    "    // MetodoTeste(1, 2, \"\");\n"
    "}\n"
    "int MetodoTeste(unsigned int first, int second, const char* desc) { return 1; }\n"
)
full_range = {"start": {"line": 0, "character": 0},
              "end": {"line": 30, "character": 0}}
all_hints = run(text=parameter_source, hint_range=full_range)
parameter_hints = [hint for hint in all_hints if hint["kind"] == 2]
expected = [
    (12, "1", "first:"), (12, "2", "second:"), (12, '""', "desc:"),
    (14, "2", "value:"),
    (15, "Inner", "left:"), (15, "3", "number:"), (15, "(4", "right:"),
    (17, "2", "second:"), (17, '""', "desc:"),
    (20, "1", "count:"), (21, "2", "count:"), (22, "3", "amount:"),
    (24, "1", "first:"), (25, "2", "second:"), (26, '""', "desc:"),
]
source_lines = parameter_source.splitlines()
assert [(hint["position"], hint["label"]) for hint in parameter_hints] == [
    ({"line": line, "character": source_lines[line].index(argument)}, label)
    for line, argument, label in expected
], parameter_hints
assert all(hint["paddingRight"] for hint in parameter_hints), parameter_hints
positions = [(hint["position"]["line"], hint["position"]["character"])
             for hint in all_hints]
assert positions == sorted(positions), all_hints
assert run(disabled=True, text=parameter_source, hint_range=full_range) == []

partial_range = {"start": {"line": 12, "character": source_lines[12].index("2")},
                 "end": {"line": 12, "character": source_lines[12].index('""')}}
partial = run(text=parameter_source, hint_range=partial_range)
assert [hint["label"] for hint in partial] == ["second:"], partial

multiline_range = {"start": {"line": 25, "character": 0},
                   "end": {"line": 26, "character": 0}}
multiline = run(text=parameter_source, hint_range=multiline_range)
assert [hint["label"] for hint in multiline] == ["second:"], multiline

example_source = (
    "#include <cstdint>\n"
    "#include <string>\n"
    "int MetodoTeste(std::uint32_t first, int second, const std::string& desc);\n"
    "int main() { MetodoTeste(1, 2, \"\"); }\n"
    "int MetodoTeste(std::uint32_t first, int second, const std::string& desc) { return 1; }\n"
)
example = run(text=example_source, hint_range=full_range)
assert [hint["label"] for hint in example] == ["first:", "second:", "desc:"], example

assert run(text=("struct Provider {};\n"
                 "void F(Provider& provider) { auto invalid = provider.GetService<int>(); }\n"),
           hint_range=full_range) == []

with tempfile.TemporaryDirectory(prefix="heimdall-template-hints-") as directory:
    workspace = Path(directory)
    header = workspace / "services.hpp"
    header.write_text(
        "namespace LIB_NAMESPACE { template<class T> struct Arc {};\n"
        "struct Provider { template<class T> Arc<T> GetService(); }; }\n"
        "namespace APP_NAMESPACE { struct ComponentManager {}; struct SystemManager {};\n"
        "struct Publisher {}; template<class T> using Action = std::function<void(T&)>;\n"
        "struct Extension { std::vector<Action<ComponentManager>> callbacks;\n"
        "std::map<int, Publisher*> publishers; void Use(skr::Provider&); }; }\n")
    commands = workspace / "compile_commands.json"
    main = workspace / "main.cpp"
    commands.write_text(json.dumps([{
        "directory": str(workspace), "file": str(main),
        "arguments": ["c++", "-std=c++20", "-DLIB_NAMESPACE=skr", "-DAPP_NAMESPACE=app",
                      "-c", str(main)],
    }]))
    indexed_source = (
        f'#include "{header.as_posix()}"\n'
        "namespace APP_NAMESPACE { void Extension::Use(skr::Provider& provider) {\n"
        "auto component = provider.GetService<ComponentManager>();\n"
        "auto system = provider.GetService<SystemManager>();\n"
        "for (auto& func : callbacks) { func(component); }\n"
        "for (auto [key, publisher] : publishers) {}\n"
        "} }\n")
    main.write_text(indexed_source)
    indexed_hints = run(text=indexed_source, hint_range=full_range,
                        initialization_options={"compileCommands": str(commands)},
                        document_uri=main.as_uri())
    assert [(hint["position"]["line"], hint["label"]) for hint in indexed_hints
            if hint["kind"] == 1] == [
        (2, ": skr::Arc<ComponentManager>"),
        (3, ": skr::Arc<SystemManager>"),
        (4, ": Action<ComponentManager>&"),
        (5, ": const int"),
        (5, ": Publisher*"),
    ], indexed_hints
