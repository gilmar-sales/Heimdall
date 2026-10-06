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


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_include_completion_") as temporary:
    root = Path(temporary)
    for relative in ("src/local.hpp", "src/sub/nested.hpp", "inc/shared.hpp", "inc/lib/api.hpp",
                     "quote/only_quoted.hpp"):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("// fixture\n", encoding="utf-8")
    for number in range(1200):
        (root / "inc" / "big").mkdir(parents=True, exist_ok=True)
        (root / "inc" / "big" / f"file{number:04d}.hpp").write_text("//\n", encoding="utf-8")
    # Overflow the 1000-item page with project headers alone: system directory
    # size varies by toolchain (MinGW exceeds one page, Ubuntu GCC does not),
    # so the empty-prefix `angled` case must not depend on it. `zpad_` sorts
    # after `shared.hpp`, keeping the membership assertions below inside the
    # first page (directories first, then files by label; project origins sort
    # before system ones, so the cap drops system entries first).
    for number in range(1010):
        (root / "inc" / f"zpad_{number:04d}.hpp").write_text("//\n", encoding="utf-8")
    source = root / "src" / "main.cpp"
    database = root / "compile_commands.json"
    database.write_text(json.dumps([{
        "directory": str(root),
        "command": f"c++ -I{root / 'inc'} -iquote {root / 'quote'} -c src/main.cpp",
        "file": str(source),
    }]), encoding="utf-8")

    # (document text, cursor line, cursor character)
    cases = {
        "quoted": ('#include "', 0, 10),
        "angled": ("#include <", 0, 10),
        "angled_dir": ("#include <lib/", 0, 14),
        "angled_prefix": ("#include <sha>", 0, 13),
        "quoted_closed": ('#include "lo"', 0, 12),
        "big": ("#include <big/", 0, 14),
        "big_prefix": ("#include <big/file11", 0, 20),
    }
    uri = source.as_uri()
    messages = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "initializationOptions": {"compileCommands": str(database)}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
    ]
    for index, (name, (text, line, character)) in enumerate(cases.items()):
        document_uri = (root / "src" / f"{name}.cpp").as_uri()
        messages.append({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": document_uri, "languageId": "cpp", "version": 1, "text": text}}})
        messages.append({"jsonrpc": "2.0", "id": 10 + index, "method": "textDocument/completion", "params": {
            "textDocument": {"uri": document_uri}, "position": {"line": line, "character": character}}})
    messages += [
        {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": {}},
        {"jsonrpc": "2.0", "method": "exit"},
    ]
    process = subprocess.run([server], input=b"".join(frame(m) for m in messages),
                             capture_output=True, cwd=root, timeout=30, check=False)
    assert process.returncode == 0, process.stderr.decode(errors="replace")
    responses = parse(process.stdout)

    initialize = next(r for r in responses if r.get("id") == 1)
    triggers = initialize["result"]["capabilities"]["completionProvider"]["triggerCharacters"]
    for trigger in ("/", "<", '"'):
        assert trigger in triggers, triggers

    def items(name):
        index = list(cases).index(name)
        return next(r for r in responses if r.get("id") == 10 + index)["result"]["items"]

    def labels(name):
        return [item["label"] for item in items(name)]

    quoted = labels("quoted")
    assert "local.hpp" in quoted and "sub/" in quoted and "only_quoted.hpp" in quoted, quoted
    assert "shared.hpp" in quoted and "lib/" in quoted, quoted

    angled = labels("angled")
    assert "shared.hpp" in angled and "lib/" in angled, angled
    assert "local.hpp" not in angled and "sub/" not in angled and "only_quoted.hpp" not in angled, angled

    assert labels("angled_dir")[0] == "api.hpp", labels("angled_dir")
    edit = items("angled_dir")[0]["textEdit"]
    assert edit["newText"] == "api.hpp>", edit
    assert edit["range"]["start"] == {"line": 0, "character": 14}, edit

    assert labels("angled_prefix")[0] == "shared.hpp", labels("angled_prefix")  # project before system
    edit = items("angled_prefix")[0]["textEdit"]
    assert edit["newText"] == "shared.hpp", edit  # `>` already present
    assert edit["range"]["start"] == {"line": 0, "character": 10}
    assert edit["range"]["end"] == {"line": 0, "character": 13}

    closed = items("quoted_closed")
    assert closed[0]["label"] == "local.hpp", closed
    assert closed[0]["textEdit"]["newText"] == "local.hpp", closed

    directory = next(item for item in items("angled") if item["label"] == "lib/")
    assert directory["kind"] == 19 and directory["textEdit"]["newText"] == "lib/", directory
    assert directory["command"]["command"] == "editor.action.triggerSuggest", directory
    file_item = next(item for item in items("angled") if item["label"] == "shared.hpp")
    assert file_item["kind"] == 17 and file_item["textEdit"]["newText"] == "shared.hpp>", file_item

    def incomplete(name):
        index = list(cases).index(name)
        return next(r for r in responses if r.get("id") == 10 + index)["result"]["isIncomplete"]

    # A page cut by the cap must make the client ask again as the user types,
    # otherwise `vector` is never found after `#include <` listed the first 1000.
    assert incomplete("big") is True
    assert len(items("big")) == 1000
    assert incomplete("big_prefix") is False
    assert labels("big_prefix") == [f"file11{n:02d}.hpp" for n in range(100)], labels("big_prefix")
    assert incomplete("angled") is True  # project headers alone exceed one page
