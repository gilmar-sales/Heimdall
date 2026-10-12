import json
import subprocess
import sys
import tempfile
from pathlib import Path

server = sys.argv[1]


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def parse(stream):
    responses = []
    while stream:
        header, stream = stream.split(b"\r\n\r\n", 1)
        length = int(header.decode("ascii").split(":", 1)[1].strip())
        body, stream = stream[:length], stream[length:]
        responses.append(json.loads(body))
    return responses


text = (
    "/// Greets someone.\n"
    "void greet(const char* name = \"olá\", int times = 1);\n"
    "void greet(int id);\n"
    "struct Point { Point(int x, int y); };\n"
    "int main() {\n"
    "    greet(\"a\", 2);\n"
    "    Point p(1, 2);\n"
    "    return 0;\n"
    "}\n"
)

with tempfile.TemporaryDirectory(prefix="heimdall_lsp_signature_help_") as temporary:
    root = Path(temporary)
    source = root / "main.cpp"
    source.write_text(text, encoding="utf-8")
    uri = source.as_uri()

    def at(request_id, line, character):
        return {"jsonrpc": "2.0", "id": request_id, "method": "textDocument/signatureHelp",
                "params": {"textDocument": {"uri": uri},
                           "position": {"line": line, "character": character}}}

    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": uri, "languageId": "cpp", "version": 1, "text": text}}},
        at(2, 5, 15),  # greet("a", |2)
        at(3, 6, 15),  # Point p(1, |2)
        at(4, 7, 11),  # return |0: no call
        {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ]
    process = subprocess.run([server], input=b"".join(frame(m) for m in messages),
                             capture_output=True, cwd=root, timeout=30, check=False)
    assert process.returncode == 0, process.stderr.decode(errors="replace")
    responses = {r["id"]: r for r in parse(process.stdout) if "id" in r}

    capabilities = responses[1]["result"]["capabilities"]
    assert "(" in capabilities["signatureHelpProvider"]["triggerCharacters"], capabilities

    greet = responses[2]["result"]
    labels = [s["label"] for s in greet["signatures"]]
    assert labels == ["greet(const char* name = \"olá\", int times = 1) -> void"], labels
    assert greet["activeParameter"] == 1, greet
    assert greet["signatures"][0]["documentation"]["value"] == "Greets someone.", greet
    label = labels[0]
    # Ranges count UTF-16 units: `á` is two bytes but one unit.
    first, second = (p["label"] for p in greet["signatures"][0]["parameters"])
    assert label[first[0]:first[1]] == "const char* name = \"olá\"", first
    assert label[second[0]:second[1]] == "int times = 1", second

    point = responses[3]["result"]
    assert [s["label"] for s in point["signatures"]] == ["Point(int x, int y)"], point
    assert point["activeParameter"] == 1, point

    assert responses[4]["result"] is None, responses[4]

print("signature help smoke passed")
