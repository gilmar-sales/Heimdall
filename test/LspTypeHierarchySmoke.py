import json
import subprocess
import sys


server = sys.argv[1]
uri = "file:///tmp/heimdall-type-hierarchy.cpp"
source = (
    "struct Parent1 {};\n"
    "struct Parent2 {};\n"
    "struct Child : Parent1, Parent2 {};\n"
    "struct Grandchild : Child {};\n"
    "int main() { Child child; }\n"
)


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode()
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body


def run(extra):
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "cpp", "version": 1,
                             "text": source}}},
        *extra,
        {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ]
    process = subprocess.run([server], input=b"".join(map(frame, messages)),
                             capture_output=True, timeout=10, check=False)
    if process.returncode:
        raise AssertionError(process.stderr.decode(errors="replace"))
    stream, responses = process.stdout, {}
    while stream:
        header, stream = stream.split(b"\r\n\r\n", 1)
        length = int(header.decode().split(":", 1)[1].strip())
        body, stream = stream[:length], stream[length:]
        message = json.loads(body)
        if "id" in message:
            responses[message["id"]] = message["result"]
    return responses


def prepare(line, character):
    return {"jsonrpc": "2.0", "id": 2, "method": "textDocument/prepareTypeHierarchy",
            "params": {"textDocument": {"uri": uri},
                       "position": {"line": line, "character": character}}}


def relation(method, item):
    return {"jsonrpc": "2.0", "id": 3, "method": method, "params": {"item": item}}


first = run([prepare(2, 9)])
assert first[1]["capabilities"]["typeHierarchyProvider"] is True
items = first[2]
assert [item["name"] for item in items] == ["Child"], items
child = items[0]
assert child["selectionRange"]["start"] == {"line": 2, "character": 7}, child

via_variable = run([prepare(4, 20)])[2]
assert [item["name"] for item in via_variable] == ["Child"], via_variable

parents = run([relation("typeHierarchy/supertypes", child)])[3]
assert [item["name"] for item in parents] == ["Parent1", "Parent2"], parents

children = run([relation("typeHierarchy/subtypes", child)])[3]
assert [item["name"] for item in children] == ["Grandchild"], children

assert run([prepare(4, 2)])[2] is None
