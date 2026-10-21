import json
import os
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading


def frame(message):
    body = json.dumps(message).encode()
    return f"Content-Length: {len(body)}\r\n\r\n".encode() + body


def receive(stream, messages):
    try:
        while True:
            length = 0
            while True:
                line = stream.readline()
                if not line:
                    raise EOFError("LSP closed stdout")
                if line == b"\r\n":
                    break
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":", 1)[1])
            messages.put(json.loads(stream.read(length)))
    except Exception as error:
        messages.put(error)


server = str(pathlib.Path(sys.argv[1]).resolve())
environment = dict(os.environ)
environment.pop("HEIMDALL_TRACE", None)
with tempfile.TemporaryDirectory(prefix="heimdall-profile-") as scratch:
    directory = pathlib.Path(scratch)
    root = directory / "workspace"
    root.mkdir()
    (root / "Main.cpp").write_text("int Main() { return 0; }\n")
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen(
            [server], cwd=directory, env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors)
        messages = queue.Queue()
        reader = threading.Thread(target=receive, args=(process.stdout, messages), daemon=True)
        reader.start()

        def send(message):
            process.stdin.write(frame(message))
            process.stdin.flush()

        def wait_for(predicate):
            while True:
                message = messages.get(timeout=40)
                if isinstance(message, Exception):
                    raise message
                if predicate(message):
                    return message

        try:
            send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "rootUri": root.as_uri(), "capabilities": {},
                "initializationOptions": {"workspaceSymbols": True}}})
            wait_for(lambda message: message.get("id") == 1)
            send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
            wait_for(lambda message: message.get("method") == "window/logMessage" and
                     "symbol index ready" in message.get("params", {}).get("message", ""))

            trace = root / "heimdall.pftrace"
            assert process.poll() is None, "Server exited before trace verification"
            assert trace.is_file(), "Trace must exist before LSP shutdown"
            assert trace.stat().st_size > 0, "Trace must contain profiling data"
            assert not (directory / "heimdall.pftrace").exists(), "Trace used server cwd"

            send({"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": {}})
            wait_for(lambda message: message.get("id") == 2)
            send({"jsonrpc": "2.0", "method": "exit"})
            assert process.wait(timeout=10) == 0
        except Exception:
            errors.seek(0)
            print(errors.read().decode(errors="replace"), file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
            reader.join(timeout=5)
            process.stdin.close()
            process.stdout.close()
