import json
import subprocess
import sys
import tempfile
from pathlib import Path


def frame(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def position(text, needle, last=False):
    offset = text.rfind(needle) if last else text.find(needle)
    assert offset >= 0, needle
    prefix = text[:offset]
    return {"line": prefix.count("\n"),
            "character": len(prefix.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}


with tempfile.TemporaryDirectory(prefix="heimdall-doxygen-flow-") as directory:
    root = Path(directory).resolve()
    (root / ".heimdall.json").write_text(json.dumps({"root": True, "rules": {
        "cpp/missing-return": "error",
        "cpp/return-type": "error",
        "cpp/unreachable-code": "warning",
    }}), encoding="utf-8")
    uri = (root / "docs.cpp").as_uri()
    broken_uri = (root / "broken.cpp").as_uri()
    source = (
        "/** @brief Describe Node. */\n"
        "class Node {};\n"
        "/**\n"
        " * @brief Calculates 😀 a value using @ref Node.\n"
        " * @param[in] a First operand.\n"
        " * @param b Second operand.\n"
        " * @returns A sum.\n"
        " * @see Node\n"
        " */\n"
        "int Calculate(int a, int b) { return a + b; }\n"
        "int Use() { return Calculate(1, 2); }\n"
    )
    broken = (
        "int Missing(bool c) { if (c) return; }\n"
        "void Dead() { return; int dead = 10; dead++; }\n"
    )
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "capabilities": {"workspace": {"workspaceEdit": {"documentChanges": True}}},
            "initializationOptions": {"enableSemantic": True, "workspaceDiagnostics": False,
                                      "workspaceSymbols": False}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
    ]
    for document_uri, text in [(uri, source), (broken_uri, broken)]:
        messages.append({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": document_uri, "languageId": "cpp", "version": 1,
                             "text": text}}})

    requests = [
        (2, "textDocument/hover", position(source, "Calculate(1", last=True), {}),
        (3, "textDocument/definition", position(source, "a First"), {}),
        (4, "textDocument/definition", position(source, "Node.\n"), {}),
        (5, "textDocument/rename", position(source, "a, int"), {"newName": "first"}),
        (6, "textDocument/rename", position(source, "Node {};"), {"newName": "Element"}),
        (7, "textDocument/documentLink", None, {}),
    ]
    for request_id, method, cursor, extra in requests:
        params = {"textDocument": {"uri": uri}, **extra}
        if cursor is not None:
            params["position"] = cursor
        messages.append({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
    messages.extend([
        {"jsonrpc": "2.0", "id": 8, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ])
    process = subprocess.run([sys.argv[1]], input=b"".join(map(frame, messages)),
                             capture_output=True, cwd=root, timeout=20, check=False)
    assert process.returncode == 0, process.stderr.decode(errors="replace")
    stream = process.stdout
    responses = []
    while stream:
        header, stream = stream.split(b"\r\n\r\n", 1)
        length = int(header.decode("ascii").split(":", 1)[1].strip())
        body, stream = stream[:length], stream[length:]
        responses.append(json.loads(body))
    by_id = {item["id"]: item for item in responses if "id" in item}
    for request_id in range(1, 9):
        assert request_id in by_id and "error" not in by_id[request_id], by_id.get(request_id)

    capabilities = by_id[1]["result"]["capabilities"]
    assert capabilities["documentLinkProvider"]["resolveProvider"] is False, capabilities
    hover = by_id[2]["result"]["contents"]
    assert hover["kind"] == "markdown", hover
    for text in ["Calculates 😀 a value", "First operand.", "Second operand.", "A sum."]:
        assert text in hover["value"], hover
    for tag in ["@brief", "@param", "@returns", "@see", "@ref"]:
        assert tag not in hover["value"], hover
    for section in ["\n\n**Parameters**\n\n", "\n\n**Returns**\n\n",
                    "\n\n**See also**\n\n"]:
        assert section in hover["value"], hover

    for request_id, target in [(3, "a, int"), (4, "Node {};")]:
        locations = by_id[request_id]["result"]
        assert len(locations) == 1 and locations[0]["uri"] == uri, locations
        assert locations[0]["range"]["start"] == position(source, target), locations

    def edits_of(request_id):
        result = by_id[request_id]["result"]
        if "documentChanges" in result:
            assert len(result["documentChanges"]) == 1, result
            change = result["documentChanges"][0]
            assert change["textDocument"]["uri"] == uri, change
            return change["edits"]
        return result["changes"][uri]

    parameter_edits = edits_of(5)
    assert len(parameter_edits) == 3, parameter_edits
    assert any(edit["range"]["start"] == position(source, "a First") and
               edit["newText"] == "first" for edit in parameter_edits), parameter_edits
    class_edits = edits_of(6)
    assert len(class_edits) == 4, class_edits
    assert any(edit["range"]["start"] == position(source, "Node. */") and
               edit["newText"] == "Element" for edit in class_edits), class_edits

    links = by_id[7]["result"]
    assert len(links) == 4, links
    for needle, destination in [("a First", "a, int"), ("b Second", "b) {"),
                                ("Node.\n", "Node {};"), ("Node\n", "Node {};")]:
        start = position(source, destination)
        target = f"{uri}#L{start['line'] + 1},{start['character'] + 1}"
        assert any(link["range"]["start"] == position(source, needle) and
                   link["target"] == target for link in links), (needle, links)

    publications = [item["params"]["diagnostics"] for item in responses
                    if item.get("method") == "textDocument/publishDiagnostics" and
                    item["params"]["uri"] == broken_uri]
    assert publications, responses
    diagnostics = publications[-1]
    for code, severity in [("cpp/missing-return", 1), ("cpp/return-type", 1),
                           ("cpp/unreachable-code", 2)]:
        assert any(item["code"] == code and item["severity"] == severity
                   for item in diagnostics), diagnostics
