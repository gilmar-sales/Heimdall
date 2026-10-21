"""End-to-end check of C++20 module support in the language server: the document outline
lists the module declaration, and go-to-definition on an `import` jumps to the module
interface unit found in the workspace (not to the implementation unit)."""
import json
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

server = sys.argv[1]
SYMBOL_KIND_MODULE = 2


class Client:
    def __init__(self, workspace):
        self.process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, cwd=workspace)
        self.inbox = []
        self.lock = threading.Lock()
        self.next_id = 100
        threading.Thread(target=self._read, daemon=True).start()
        params = {"workspaceFolders": [{"uri": workspace.as_uri(), "name": "workspace"}],
                  "capabilities": {"textDocument": {"documentSymbol": {
                      "hierarchicalDocumentSymbolSupport": True}}}}
        self.send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": params})
        self.wait_response(1)
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

    def shutdown(self):
        self.send({"jsonrpc": "2.0", "id": 9000, "method": "shutdown", "params": {}})
        self.wait_response(9000)
        self.notify("exit", {})
        self.process.stdin.close()
        self.process.wait(timeout=15)


def position_of(text, needle):
    offset = text.index(needle)
    line = text.count("\n", 0, offset)
    return {"line": line, "character": offset - (text.rfind("\n", 0, offset) + 1)}


with tempfile.TemporaryDirectory(prefix="heimdall_lsp_modules_", ignore_cleanup_errors=True) as tmp:
    workspace = Path(tmp).resolve()
    util_interface = workspace / "util.cppm"
    util_impl = workspace / "util_impl.cpp"
    core_part = workspace / "core_detail.cppm"
    main = workspace / "core.cppm"
    util_interface.write_text("export module app.util;\nexport int Twice(int value);\n", encoding="utf-8")
    util_impl.write_text("module app.util;\nint Twice(int value) { return value * 2; }\n",
                         encoding="utf-8")
    core_part.write_text("export module app.core:detail;\nexport int Hidden();\n", encoding="utf-8")
    main_text = ("export module app.core;\nimport app.util;\nexport import :detail;\n"
                 "import <vector>;\nexport int Use() { return Twice(2); }\n")
    main.write_text(main_text, encoding="utf-8")

    client = Client(workspace)
    assert client.wait_log("symbol index ready"), "the workspace symbol index never became ready"
    client.notify("textDocument/didOpen", {"textDocument": {
        "uri": main.as_uri(), "languageId": "cpp", "version": 1, "text": main_text}})

    outline = client.request("textDocument/documentSymbol", {"textDocument": {"uri": main.as_uri()}})
    module = next((s for s in outline if s["name"] == "app.core"), None)
    assert module is not None and module["kind"] == SYMBOL_KIND_MODULE, outline

    def definition(needle):
        return client.request("textDocument/definition", {
            "textDocument": {"uri": main.as_uri()}, "position": position_of(main_text, needle)},
            until=lambda r: bool(r))

    named = definition("app.util")
    assert len(named) == 1 and named[0]["uri"].endswith("util.cppm"), named
    assert named[0]["range"]["start"]["line"] == 0, named

    partition = definition(":detail")
    assert len(partition) == 1 and partition[0]["uri"].endswith("core_detail.cppm"), partition

    header = client.request("textDocument/definition", {
        "textDocument": {"uri": main.as_uri()}, "position": position_of(main_text, "vector")})
    assert not header, header

    symbols = client.request("workspace/symbol", {"query": "app.util"},
                             until=lambda r: any(s["kind"] == SYMBOL_KIND_MODULE for s in r))
    assert sum(1 for s in symbols if s["kind"] == SYMBOL_KIND_MODULE) == 2, symbols

    client.shutdown()
print("LspModuleSmoke: ok")
