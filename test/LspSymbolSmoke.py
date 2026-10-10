"""End-to-end check of textDocument/documentSymbol (Outline) and workspace/symbol
(Go to Symbol in Workspace) against real files, open buffers and file-watcher events.

Semantic analysis stays off and no compile database is given: both features must work
from syntax alone."""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import unquote, urlparse

server = sys.argv[1]

SYMBOL_KIND = {"Namespace": 3, "Class": 5, "Method": 6, "Field": 8, "Constructor": 9, "Enum": 10,
               "Function": 12, "Variable": 13, "EnumMember": 22, "Struct": 23}


def uri_path(uri):
    path = unquote(urlparse(uri).path)
    if os.name == "nt" and len(path) >= 3 and path[0] == "/" and path[2] == ":":
        path = path[1:]
    return os.path.normcase(os.path.normpath(path))


def same_file(uri, path):
    return uri_path(uri) == os.path.normcase(os.path.normpath(str(path)))


class Client:
    def __init__(self, workspace, options=None, hierarchical=True):
        self.process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, cwd=workspace)
        self.inbox = []
        self.lock = threading.Lock()
        self.next_id = 100
        threading.Thread(target=self._read, daemon=True).start()
        capabilities = {"textDocument": {"documentSymbol": {
            "hierarchicalDocumentSymbolSupport": hierarchical}}}
        params = {"workspaceFolders": [{"uri": workspace.as_uri(), "name": "workspace"}],
                  "capabilities": capabilities}
        if options is not None:
            params["initializationOptions"] = options
        self.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": params})
        self.initialize = self.wait_response(1)
        self.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

    def _read(self):
        stream = self.process.stdout
        while True:
            header = b""
            while not header.endswith(b"\r\n\r\n"):
                chunk = stream.read(1)
                if not chunk:
                    return
                header += chunk
            length = int(header.decode("ascii").split(":", 1)[1].strip())
            body = stream.read(length)
            with self.lock:
                self.inbox.append(json.loads(body))

    def send(self, message):
        body = json.dumps(message, separators=(",", ":")).encode("utf-8")
        self.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
        self.process.stdin.flush()

    def wait_response(self, request_id, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                found = next((m for m in self.inbox if m.get("id") == request_id), None)
            if found is not None:
                return found
            time.sleep(0.01)
        raise SystemExit(f"timeout waiting for response {request_id}")

    def wait_log(self, needle, timeout=15.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if any(m.get("method") == "window/logMessage" and needle in m["params"]["message"]
                       for m in self.inbox):
                    return True
            time.sleep(0.02)
        return False

    def request(self, method, params, until=None, timeout=10.0):
        """Send a request; with `until`, repeat it until the predicate accepts the result
        (open buffers are indexed by a background worker after each edit)."""
        deadline = time.time() + timeout
        while True:
            self.next_id += 1
            current = self.next_id
            self.send({"jsonrpc": "2.0", "id": current, "method": method, "params": params})
            response = self.wait_response(current, timeout)
            if "error" in response:
                raise SystemExit(f"{method} failed: {response['error']}")
            result = response["result"]
            if until is None or until(result) or time.time() >= deadline:
                return result
            time.sleep(0.05)

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def open(self, path, text, version=1):
        self.notify("textDocument/didOpen", {"textDocument": {
            "uri": path.as_uri(), "languageId": "cpp", "version": version, "text": text}})

    def change(self, path, version, changes):
        self.notify("textDocument/didChange", {
            "textDocument": {"uri": path.as_uri(), "version": version}, "contentChanges": changes})

    def close(self, path):
        self.notify("textDocument/didClose", {"textDocument": {"uri": path.as_uri()}})

    def symbols(self, query, until=None):
        return self.request("workspace/symbol", {"query": query}, until=until)

    def outline(self, path):
        return self.request("textDocument/documentSymbol", {"textDocument": {"uri": path.as_uri()}})

    def shutdown(self):
        self.send({"jsonrpc": "2.0", "id": 9000, "method": "shutdown", "params": {}})
        self.wait_response(9000)
        self.notify("exit", {})
        self.process.stdin.close()
        self.process.wait(timeout=15)


def names(items):
    return [item["name"] for item in items]


def child(symbol, name):
    return next(item for item in symbol["children"] if item["name"] == name)


def inside(inner, outer):
    def key(position):
        return (position["line"], position["character"])
    return key(outer["start"]) <= key(inner["start"]) and key(inner["end"]) <= key(outer["end"])


def check_ranges(symbols):
    for symbol in symbols:
        assert inside(symbol["selectionRange"], symbol["range"]), symbol
        check_ranges(symbol["children"])


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_symbols_") as temporary:
    workspace = Path(temporary).resolve()
    header = workspace / "include" / "shapes.hpp"
    source = workspace / "src" / "shapes.cpp"
    main = workspace / "src" / "main.cpp"
    emoji = workspace / "src" / "emoji.cpp"
    generated = workspace / "build" / "generated.cpp"
    for path in (header, source, main, emoji, generated):
        path.parent.mkdir(parents=True, exist_ok=True)

    header_text = ("namespace geo\n{\nstruct Circle\n{\n    Circle();\n    double Area() const;\n"
                   "    double radius;\n};\nenum class Kind { Round, Flat };\n}\n")
    header.write_text(header_text, encoding="utf-8")
    source.write_text('#include "../include/shapes.hpp"\nnamespace geo\n{\n'
                      "double Circle::Area() const { return 3.14; }\n}\n", encoding="utf-8")
    main.write_text("int main() { return 0; }\n", encoding="utf-8")
    emoji.write_text("/* \U0001F600 */ int emoji_target;\r\nint second_line;\r\n", encoding="utf-8")
    generated.write_text("int GeneratedSymbol;\n", encoding="utf-8")

    # ---- capabilities, Outline and the workspace index (hierarchical client) ----------------
    client = Client(workspace)
    capabilities = client.initialize["result"]["capabilities"]
    assert capabilities.get("documentSymbolProvider") is True, capabilities
    assert capabilities.get("workspaceSymbolProvider") is True, capabilities
    assert client.wait_log("symbol index ready"), "the workspace symbol index never became ready"

    # Outline: hierarchy, kinds and ranges.
    client.open(header, header_text)
    outline = client.outline(header)
    assert names(outline) == ["geo"], outline
    geo = outline[0]
    assert geo["kind"] == SYMBOL_KIND["Namespace"], geo
    assert names(geo["children"]) == ["Circle", "Kind"], geo
    circle = child(geo, "Circle")
    assert circle["kind"] == SYMBOL_KIND["Struct"], circle
    assert names(circle["children"]) == ["Circle", "Area", "radius"], circle
    assert child(circle, "Circle")["kind"] == SYMBOL_KIND["Constructor"], circle
    area = child(circle, "Area")
    assert area["kind"] == SYMBOL_KIND["Method"] and area["detail"] == "() const", area
    assert area["selectionRange"] == {"start": {"line": 5, "character": 11},
                                      "end": {"line": 5, "character": 15}}, area
    assert child(circle, "radius")["kind"] == SYMBOL_KIND["Field"], circle
    kind = child(geo, "Kind")
    assert kind["kind"] == SYMBOL_KIND["Enum"] and names(kind["children"]) == ["Round", "Flat"], kind
    assert child(kind, "Round")["kind"] == SYMBOL_KIND["EnumMember"], kind
    check_ranges(outline)

    # Outline follows edits (incremental sync) and answers for the edited version.
    client.change(header, 2, [{"range": {"start": {"line": 5, "character": 4},
                                         "end": {"line": 5, "character": 4}},
                               "text": "double Perimeter() const;\n    "}])
    after_edit = client.request(
        "textDocument/documentSymbol", {"textDocument": {"uri": header.as_uri()}},
        until=lambda result: "Perimeter" in names(child(result[0], "Circle")["children"]))
    members = names(child(after_edit[0], "Circle")["children"])
    assert members == ["Circle", "Perimeter", "Area", "radius"], members
    assert child(child(after_edit[0], "Circle"), "Area")["selectionRange"]["start"]["line"] == 6

    # An unknown or empty document is an empty outline, not an error.
    assert client.request("textDocument/documentSymbol",
                          {"textDocument": {"uri": (workspace / "nope.cpp").as_uri()}}) == []
    client.open(workspace / "src" / "empty.cpp", "")
    assert client.outline(workspace / "src" / "empty.cpp") == []

    # Workspace symbols come from several files, and from nothing under build/.
    circles = client.symbols("Circle")
    assert any(same_file(item["location"]["uri"], header) and item["name"] == "Circle"
               and item["containerName"] == "geo" and item["kind"] == SYMBOL_KIND["Struct"]
               for item in circles), circles
    assert any(same_file(item["location"]["uri"], source) and item["name"] == "Circle::Area"
               for item in client.symbols("Area")), client.symbols("Area")
    assert client.symbols("GeneratedSymbol") == []
    assert client.symbols("") == []
    assert [item["name"] for item in client.symbols("main")] == ["main"]
    exact_first = client.symbols("geo::Circle")
    assert exact_first and exact_first[0]["name"] == "Circle", exact_first

    # Ranges are LSP positions (UTF-16 units, CRLF) of the declaration name.
    target = client.symbols("emoji_target")
    assert len(target) == 1 and same_file(target[0]["location"]["uri"], emoji), target
    assert target[0]["location"]["range"] == {"start": {"line": 0, "character": 13},
                                              "end": {"line": 0, "character": 25}}, target
    second = client.symbols("second_line")
    assert second[0]["location"]["range"]["start"] == {"line": 1, "character": 4}, second

    # Results are deterministic.
    assert client.symbols("a") == client.symbols("a")

    # An open buffer shadows the file on disk: unsaved symbols appear and vanish with it.
    main_text = "int main() { return 0; }\nint BufferOnly;\n"
    client.open(main, main_text)
    buffer_hits = client.symbols("BufferOnly", until=bool)
    assert len(buffer_hits) == 1 and buffer_hits[0]["location"]["uri"] == main.as_uri(), buffer_hits
    assert buffer_hits[0]["location"]["range"]["start"] == {"line": 1, "character": 4}, buffer_hits
    client.change(main, 2, [{"range": {"start": {"line": 1, "character": 4},
                                       "end": {"line": 1, "character": 14}}, "text": "Renamed"}])
    renamed = client.symbols("Renamed", until=bool)
    assert len(renamed) == 1, renamed
    assert client.symbols("BufferOnly", until=lambda result: not result) == []
    client.close(main)
    assert client.symbols("Renamed", until=lambda result: not result) == []
    assert [item["name"] for item in client.symbols("main")] == ["main"]

    # File-system events: created, changed and deleted files, and a deleted folder.
    late = workspace / "src" / "late.cpp"
    late.write_text("int LateSymbol;\n", encoding="utf-8")
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": late.as_uri(), "type": 1}]})
    assert client.symbols("LateSymbol", until=bool), "created file was not indexed"
    late.write_text("int ChangedSymbol;\n", encoding="utf-8")
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": late.as_uri(), "type": 2}]})
    assert client.symbols("ChangedSymbol", until=bool), "changed file was not re-indexed"
    assert client.symbols("LateSymbol", until=lambda result: not result) == []
    late.unlink()
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": late.as_uri(), "type": 3}]})
    assert client.symbols("ChangedSymbol", until=lambda result: not result) == []
    folder = workspace / "src" / "pkg"
    folder.mkdir()
    (folder / "inner.cpp").write_text("int InnerSymbol;\n", encoding="utf-8")
    client.notify("workspace/didChangeWatchedFiles",
                  {"changes": [{"uri": (folder / "inner.cpp").as_uri(), "type": 1}]})
    assert client.symbols("InnerSymbol", until=bool)
    (folder / "inner.cpp").unlink()
    folder.rmdir()
    client.notify("workspace/didChangeWatchedFiles", {"changes": [{"uri": folder.as_uri(), "type": 3}]})
    assert client.symbols("InnerSymbol", until=lambda result: not result) == []
    # Events outside the workspace or for other file types are ignored.
    client.notify("workspace/didChangeWatchedFiles", {"changes": [
        {"uri": (workspace.parent / "elsewhere.cpp").as_uri(), "type": 1},
        {"uri": (workspace / "notes.txt").as_uri(), "type": 1}]})

    # A request cancelled right away ends with a result or RequestCancelled, never silence.
    client.next_id += 1
    cancelled = client.next_id
    client.send({"jsonrpc": "2.0", "id": cancelled, "method": "workspace/symbol",
                 "params": {"query": "a"}})
    client.notify("$/cancelRequest", {"id": cancelled})
    response = client.wait_response(cancelled)
    assert "result" in response or response["error"]["code"] == -32800, response

    client.shutdown()

    # ---- a client without hierarchical support gets SymbolInformation[] ---------------------
    flat_client = Client(workspace, hierarchical=False)
    flat_client.open(header, header_text)
    flat = flat_client.outline(header)
    assert [item["name"] for item in flat] == ["geo", "Circle", "Circle", "Area", "radius", "Kind",
                                               "Round", "Flat"], flat
    area_info = next(item for item in flat if item["name"] == "Area")
    assert area_info["containerName"] == "geo::Circle", area_info
    assert area_info["location"]["uri"] == header.as_uri(), area_info
    assert area_info["kind"] == SYMBOL_KIND["Method"], area_info
    flat_client.shutdown()

    # ---- the workspace index can be switched off --------------------------------------------
    quiet = Client(workspace, options={"workspaceSymbols": False})
    assert quiet.initialize["result"]["capabilities"]["workspaceSymbolProvider"] is False
    assert quiet.initialize["result"]["capabilities"]["documentSymbolProvider"] is True
    assert quiet.symbols("Circle") == []
    quiet.open(header, header_text)
    assert names(quiet.outline(header)) == ["geo"]
    assert not quiet.wait_log("symbol index ready", timeout=1.0)
    assert quiet.symbols("Circle") == []
    quiet.shutdown()

print("LspSymbolSmoke: ok")
