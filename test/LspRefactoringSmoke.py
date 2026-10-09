"""Versioned local rename, references, UTF-16 and conservative rejection."""
import json
import subprocess
import sys
import threading
import tempfile
import time
from pathlib import Path


process = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
condition = threading.Condition()
messages = {}


def reader():
    while True:
        header = b""
        while not header.endswith(b"\r\n\r\n"):
            char = process.stdout.read(1)
            if not char:
                return
            header += char
        size = int(header.split(b":", 1)[1].strip())
        message = json.loads(process.stdout.read(size))
        if "id" in message:
            with condition:
                messages[message["id"]] = message
                condition.notify_all()


threading.Thread(target=reader, daemon=True).start()
next_id = 0


def send(method, params, request=False):
    global next_id
    message = {"jsonrpc": "2.0", "method": method, "params": params}
    if request:
        next_id += 1
        message["id"] = next_id
    data = json.dumps(message, ensure_ascii=False).encode("utf-8")
    process.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)
    process.stdin.flush()
    if not request:
        return None
    with condition:
        assert condition.wait_for(lambda: next_id in messages, timeout=15), method
        return messages.pop(next_id)


try:
    versioned = "--unversioned" not in sys.argv
    caps = send("initialize", {"capabilities": {
        "workspace": {"workspaceEdit": {"documentChanges": versioned}}}}, True)["result"]["capabilities"]
    if not versioned:
        assert caps["renameProvider"] is False, caps
        send("initialized", {})
        uri = "file:///C:/heimdall-unversioned-refactor.cpp"
        send("textDocument/didOpen", {"textDocument": {"uri": uri, "languageId": "cpp",
            "version": 1, "text": "int f(int value) { return value; }"}})
        position = {"line": 0, "character": 10}
        params = {"textDocument": {"uri": uri}, "position": position}
        assert send("textDocument/prepareRename", params, True)["result"] is None
        assert send("textDocument/rename", {**params, "newName": "argument"}, True)["result"] is None
        assert len(send("textDocument/references", {**params,
            "context": {"includeDeclaration": True}}, True)["result"]) == 2
        assert send("textDocument/codeAction", {"textDocument": {"uri": uri},
            "range": {"start": position, "end": position},
            "context": {"diagnostics": [], "only": ["refactor"]}}, True)["result"] == []
        send("shutdown", None, True)
        send("exit", None)
        process.wait(timeout=10)
        assert process.returncode == 0
        sys.exit(0)
    assert caps["renameProvider"]["prepareProvider"]
    assert caps["referencesProvider"]
    assert "refactor.extract.function" in caps["codeActionProvider"]["codeActionKinds"]
    send("initialized", {})
    uri = "file:///C:/heimdall-refactor-fixture.cpp"
    # The emoji shifts UTF-16 positions relative to bytes and codepoints.
    text = 'int f(int value) { /* 😀 */ return value + value; }\r\n'
    send("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "cpp", "version": 1, "text": text}})

    def at(needle, last=False):
        index = text.rfind(needle) if last else text.find(needle)
        return {"line": 0, "character": len(text[:index].encode("utf-16-le")) // 2}

    params = {"textDocument": {"uri": uri}, "position": at("value", True)}
    prepared = send("textDocument/prepareRename", params, True)["result"]
    assert prepared["placeholder"] == "value", prepared
    assert prepared["range"]["start"] == at("value", True), prepared
    refs = send("textDocument/references", {**params, "context": {"includeDeclaration": False}}, True)["result"]
    assert len(refs) == 2, refs
    renamed = send("textDocument/rename", {**params, "newName": "argument"}, True)["result"]
    change = renamed["documentChanges"][0]
    assert change["textDocument"] == {"uri": uri, "version": 1}, change
    assert len(change["edits"]) == 3, change
    assert all(edit["newText"] == "argument" for edit in change["edits"])
    assert change["edits"][-1]["range"]["start"] == at("value", True)
    error = send("textDocument/rename", {**params, "newName": "int"}, True)
    assert "error" in error, error

    text = "int f(int value) { auto fn = [&value] { return value; }; return value; }"
    send("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2},
        "contentChanges": [{"text": text}]})
    params["position"] = at("value")
    assert "error" in send("textDocument/prepareRename", params, True)
    assert "error" in send("textDocument/rename", {**params, "newName": "argument"}, True)

    text = "int f() { return 1 + 2; }"
    send("textDocument/didChange", {"textDocument": {"uri": uri, "version": 3},
        "contentChanges": [{"text": text}]})
    start = at("1 + 2")
    end = {"line": 0, "character": start["character"] + 5}
    actions = send("textDocument/codeAction", {"textDocument": {"uri": uri},
        "range": {"start": start, "end": end}, "context": {"diagnostics": [], "only": ["refactor.extract"]}}, True)["result"]
    assert len(actions) == 1 and actions[0]["kind"] == "refactor.extract.variable", actions
    assert actions[0]["edit"]["documentChanges"][0]["textDocument"]["version"] == 3
    assert len(actions[0]["edit"]["documentChanges"][0]["edits"]) == 2
    # Specific kinds must not offer a different extraction or unrelated fixes.
    assert send("textDocument/codeAction", {"textDocument": {"uri": uri},
        "range": {"start": start, "end": end}, "context": {"diagnostics": [],
        "only": ["refactor.extract.function"]}}, True)["result"] == []

    text = "int f() { int value = 42; return value; }"
    send("textDocument/didChange", {"textDocument": {"uri": uri, "version": 4},
        "contentChanges": [{"text": text}]})
    start = at("value")
    actions = send("textDocument/codeAction", {"textDocument": {"uri": uri},
        "range": {"start": start, "end": start}, "context": {"diagnostics": [], "only": ["refactor.inline"]}}, True)["result"]
    assert len(actions) == 1 and actions[0]["kind"] == "refactor.inline.variable", actions
    assert actions[0]["edit"]["documentChanges"][0]["textDocument"]["version"] == 4

    def apply_ascii(source, edits):
        # Compiler fixtures below intentionally have ASCII and a single line.
        for edit in sorted(edits, key=lambda e: e["range"]["start"]["character"], reverse=True):
            a = edit["range"]["start"]
            b = edit["range"]["end"]
            assert a["line"] == b["line"] == 0
            source = source[:a["character"]] + edit["newText"] + source[b["character"]:]
        return source

    compiler = Path(sys.argv[2])
    with tempfile.TemporaryDirectory(prefix="heimdall-refactor-compile-") as temporary:
        for version, source, needle, length, kind, title, expected in [
            (5, "constexpr int f() { return 1 + 2; }", "1 + 2", 5, "refactor.extract.variable", "Extract variable", 3),
            (6, "constexpr int f() { int value = 42; return value; }", "value", 0, "refactor.inline.variable", "Inline variable", 42),
            (7, "constexpr int f() { return 42; }", "42", 2, "refactor.extract.function", "Extract function", 42),
        ]:
            text = source
            send("textDocument/didChange", {"textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": text}]})
            start = at(needle)
            end = {"line": 0, "character": start["character"] + length}
            actions = send("textDocument/codeAction", {"textDocument": {"uri": uri},
                "range": {"start": start, "end": end}, "context": {"diagnostics": [], "only": [kind]}}, True)["result"]
            action = next((a for a in actions if a["title"].startswith(title)), None)
            assert action is not None, (source, actions)
            assert len(actions) == 1 and action["kind"] == kind, actions
            transformed = apply_ascii(source, action["edit"]["documentChanges"][0]["edits"])
            fixture = Path(temporary) / f"case{version}.cpp"
            fixture.write_text(transformed + f"\nstatic_assert(f() == {expected});\n", encoding="utf-8")
            args = ([str(compiler), "/nologo", "/std:c++20", "/Zs", str(fixture)] if compiler.stem.lower() == "cl"
                else [str(compiler), "-std=c++20", "-fsyntax-only", str(fixture)])
            compiled = subprocess.run(args, capture_output=True, text=True, timeout=30)
            assert compiled.returncode == 0, (transformed, compiled.stdout, compiled.stderr)

    header_uri = "file:///C:/heimdall-refactor-lib.hpp"
    send("textDocument/didOpen", {"textDocument": {"uri": header_uri, "languageId": "cpp", "version": 1,
        "text": "namespace dirty { int first; }"}})
    send("textDocument/didChange", {"textDocument": {"uri": uri, "version": 8}, "contentChanges": [{
        "text": '#include "heimdall-refactor-lib.hpp"\nvoid f() { dirty:: }'}]})

    def wait_member(wanted, absent):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            result = send("textDocument/completion", {"textDocument": {"uri": uri},
                "position": {"line": 1, "character": len("void f() { dirty::")}}, True)["result"]
            labels = {item["label"] for item in result["items"]}
            if wanted in labels and absent not in labels:
                return
            time.sleep(0.02)
        raise AssertionError((wanted, absent, result))

    wait_member("first", "fresh")
    send("textDocument/didChange", {"textDocument": {"uri": header_uri, "version": 2}, "contentChanges": [{
        "text": "namespace dirty { int fresh; }"}]})
    wait_member("fresh", "first")
    send("textDocument/didChange", {"textDocument": {"uri": uri, "version": 9}, "contentChanges": [{
        "text": '#include "heimdall-refactor-lib.hpp"\nint f() { return dirty::fresh; }'}]})
    location = send("textDocument/definition", {"textDocument": {"uri": uri},
        "position": {"line": 1, "character": len("int f() { return dirty::")}}, True)["result"]
    assert location == [{"uri": header_uri, "range": {
        "start": {"line": 0, "character": len("namespace dirty { int ")},
        "end": {"line": 0, "character": len("namespace dirty { int fresh")}}}], location
    send("shutdown", None, True)
    send("exit", None)
    process.wait(timeout=10)
    assert process.returncode == 0
finally:
    if process.poll() is None:
        process.kill()
        process.wait(timeout=5)
