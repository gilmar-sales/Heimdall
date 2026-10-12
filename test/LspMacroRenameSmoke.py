"""Rename of a macro defined in a header and used in other files, then refusals."""
import json
import pathlib
import subprocess
import sys
import tempfile
import threading


root = pathlib.Path(tempfile.mkdtemp(prefix="heimdall-macro-rename-"))
files = {
    "config.h": "#pragma once\n#define LIMIT 8\n#define STR(x) #x\n",
    "a.cpp": '#include "config.h"\nint a = LIMIT;\n#ifdef LIMIT\nint b = LIMIT + 1;\n#endif\n',
    "b.cpp": '#include "config.h"\nconst char* s = STR(LIMIT);\n',
}
for name, text in files.items():
    (root / name).parent.mkdir(parents=True, exist_ok=True)
    (root / name).write_bytes(text.encode("utf-8"))
    assert (root / name).read_bytes() == text.encode("utf-8")

process = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
condition = threading.Condition()
inbox = []


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
        with condition:
            inbox.append(message)
            condition.notify_all()


threading.Thread(target=reader, daemon=True).start()
next_id = 0


def send(method, params, request=False):
    global next_id
    message = {"jsonrpc": "2.0", "method": method, "params": params}
    if request:
        next_id += 1
        message["id"] = next_id
    data = json.dumps(message).encode()
    process.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)
    process.stdin.flush()
    if not request:
        return None
    with condition:
        assert condition.wait_for(lambda: any(m.get("id") == next_id for m in inbox), timeout=20), method
        return next(m for m in inbox if m.get("id") == next_id)


def wait_for(predicate, what):
    with condition:
        assert condition.wait_for(lambda: any(predicate(m) for m in inbox), timeout=20), what


try:
    send("initialize", {"rootUri": root.as_uri(), "capabilities": {
        "workspace": {"workspaceEdit": {"documentChanges": True}}}}, True)
    send("initialized", {})
    wait_for(lambda m: m.get("method") == "window/logMessage" and "symbol index ready" in m["params"]["message"],
             "index ready")
    uris = {name: (root / name).as_uri() for name in files}
    for name in ("config.h", "a.cpp"):
        send("textDocument/didOpen", {"textDocument": {"uri": uris[name], "languageId": "cpp", "version": 1,
                                                       "text": files[name]}})
        wait_for(lambda m, u=uris[name]: m.get("method") == "textDocument/publishDiagnostics"
                 and m["params"]["uri"] == u, "diagnostics")

    # "Expand macro" writes back what the preprocessor makes of a use.
    actions = send("textDocument/codeAction", {
        "textDocument": {"uri": uris["a.cpp"]},
        "range": {"start": {"line": 1, "character": 9}, "end": {"line": 1, "character": 9}},
        "context": {"diagnostics": []}}, True)["result"]
    expand = [a for a in actions if a.get("kind") == "refactor.inline.macro"]
    assert expand and expand[0]["title"] == "Expand macro LIMIT", actions
    edit = expand[0]["edit"]["documentChanges"][0]["edits"][0]
    assert edit["newText"] == "8" and edit["range"]["start"] == {"line": 1, "character": 8}, edit

    # b.cpp stringifies LIMIT through STR(): renaming would change the string, so it refuses.
    refused = send("textDocument/rename", {"textDocument": {"uri": uris["config.h"]},
                                           "position": {"line": 1, "character": 10},
                                           "newName": "MAXIMUM"}, True)
    assert "error" in refused and "stringif" in refused["error"]["message"], refused

    # Without the stringifying user, the rename covers the definition and every use.
    (root / "b.cpp").write_bytes(b'#include "config.h"\nint c = 1;\n')
    send("workspace/didChangeWatchedFiles", {"changes": [{"uri": uris["b.cpp"], "type": 2}]})
    send("textDocument/didOpen", {"textDocument": {"uri": uris["b.cpp"], "languageId": "cpp", "version": 1,
                                                   "text": '#include "config.h"\nint c = 1;\n'}})
    wait_for(lambda m: m.get("method") == "textDocument/publishDiagnostics"
             and m["params"]["uri"] == uris["b.cpp"], "b diagnostics")
    import time
    for _ in range(80):  # the index is refreshed in the background after the edit
        prepared = send("textDocument/prepareRename", {"textDocument": {"uri": uris["a.cpp"]},
                                                        "position": {"line": 1, "character": 9}}, True)
        if "error" not in prepared:
            break
        time.sleep(0.25)
    assert "error" not in prepared and prepared["result"]["placeholder"] == "LIMIT", prepared
    response = send("textDocument/rename", {"textDocument": {"uri": uris["a.cpp"]},
                                            "position": {"line": 1, "character": 9},
                                            "newName": "MAXIMUM"}, True)
    assert "error" not in response, response
    changes = {pathlib.Path(change["textDocument"]["uri"][7:]).name: change["edits"]
               for change in response["result"]["documentChanges"]}
    assert sorted(changes) == ["a.cpp", "config.h"], sorted(changes)
    assert len(changes["config.h"]) == 1, changes
    assert len(changes["a.cpp"]) == 3, changes
    assert all(edit["newText"] == "MAXIMUM" for edits in changes.values() for edit in edits)
    send("shutdown", {}, True)
    send("exit", {})
    print("ok")
finally:
    process.kill()
