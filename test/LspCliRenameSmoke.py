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
    send("initialize", {"capabilities": {
        "workspace": {"workspaceEdit": {"documentChanges": True}}},
        "initializationOptions": {"compileCommands": sys.argv[2]}}, True)
    send("initialized", {})
    path = Path(sys.argv[3]).resolve()
    uri = path.as_uri()
    text = path.read_bytes().decode("utf-8")
    name = "kExitUsageError"
    index = text.index(name)
    before = text[:index]
    position = {"line": before.count("\n"),
        "character": len(before.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}
    send("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "cpp", "version": 1, "text": text}})
    params = {"textDocument": {"uri": uri}, "position": position}
    prepared = send("textDocument/prepareRename", params, True)
    assert prepared.get("result", {}).get("placeholder") == name, prepared
    renamed = send("textDocument/rename", {**params, "newName": "kUsageErrorExitCode"}, True)
    assert "result" in renamed, renamed
    change = renamed["result"]["documentChanges"][0]
    assert change["textDocument"] == {"uri": uri, "version": 1}, change
    assert len(change["edits"]) == 8, change
    assert all(edit["newText"] == "kUsageErrorExitCode" for edit in change["edits"])

    # Compiler verification must reject hidden macro uses and new macro names.
    for version, source, replacement in [
        (2, "#define HIDDEN value\nint f(int value) { return HIDDEN; }", "argument"),
        (3, "#define argument 7\nint f(int value) { return value; }", "argument"),
        (4, "#define STRINGIFY(x) #x\nint f(int value) { return sizeof(STRINGIFY(value)); }", "argument")]:
        send("textDocument/didChange", {"textDocument": {"uri": uri, "version": version},
            "contentChanges": [{"text": source}]})
        params["position"] = {"line": 1, "character": len("int f(int ")}
        result = send("textDocument/rename", {**params, "newName": replacement}, True)
        assert "error" in result, result

    with tempfile.TemporaryDirectory(prefix="heimdall-rename-header-") as directory:
        header = Path(directory) / "local.hpp"
        header.write_text("// saved header\n", encoding="utf-8")
        source = f'#include "{header.as_posix()}"\nint f(int value) {{ return value; }}'
        send("textDocument/didChange", {"textDocument": {"uri": uri, "version": 5},
            "contentChanges": [{"text": source}]})
        assert "result" in send("textDocument/rename", {**params, "newName": "argument"}, True)
        send("textDocument/didOpen", {"textDocument": {"uri": header.as_uri(),
            "languageId": "cpp", "version": 1, "text": "#define argument 7\n"}})
        result = send("textDocument/rename", {**params, "newName": "argument"}, True)
        assert "error" in result and "unsaved" in result["error"]["message"], result
        send("textDocument/didClose", {"textDocument": {"uri": header.as_uri()}})

    send("shutdown", None, True)
    send("exit", None)
    process.wait(timeout=10)
    assert process.returncode == 0
finally:
    if process.poll() is None:
        process.kill()
        process.wait(timeout=5)
