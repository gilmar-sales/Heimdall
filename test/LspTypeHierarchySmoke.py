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


# Cross-file: Derived lives in another open document that includes the base's header.
base_uri = "file:///tmp/heimdall-th/base.hpp"
derived_uri = "file:///tmp/heimdall-th/derived.hpp"
base_text = "struct Base {};\n"
derived_text = '#include "base.hpp"\nstruct Derived : Base {};\n'


def read_message(stream):
    length = None
    while True:
        line = stream.readline()
        if not line:
            raise AssertionError("server closed the stream")
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1])
    return json.loads(stream.read(length))


def interactive(queries):
    process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:
        def send(message):
            process.stdin.write(frame(message))
            process.stdin.flush()

        send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        for doc_uri, text in ((base_uri, base_text), (derived_uri, derived_text)):
            send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": doc_uri, "languageId": "cpp", "version": 1,
                                 "text": text}}})
        pending = {base_uri, derived_uri}
        while pending:  # indexing of an open document precedes its diagnostics
            message = read_message(process.stdout)
            if message.get("method") == "textDocument/publishDiagnostics":
                pending.discard(message["params"]["uri"])
        results = []
        for number, (method, item) in enumerate(queries, start=10):
            send({"jsonrpc": "2.0", "id": number, "method": method, "params": {"item": item}})
            while True:
                message = read_message(process.stdout)
                if message.get("id") == number:
                    results.append(message["result"])
                    break
        send({"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}})
        send({"jsonrpc": "2.0", "method": "exit"})
        return results
    finally:
        process.kill()


def item_at(doc_uri, name, offset):
    return {"name": name, "data": {"uri": doc_uri, "offset": offset}}


subtypes, supertypes = interactive([
    ("typeHierarchy/subtypes", item_at(base_uri, "Base", base_text.index("Base"))),
    ("typeHierarchy/supertypes", item_at(derived_uri, "Derived", derived_text.index("Derived"))),
])
assert [(item["name"], item["uri"]) for item in subtypes] == [("Derived", derived_uri)], subtypes
assert [(item["name"], item["uri"]) for item in supertypes] == [("Base", base_uri)], supertypes
