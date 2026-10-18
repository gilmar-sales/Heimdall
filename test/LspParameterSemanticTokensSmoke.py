import json
import subprocess
import sys
import tempfile
from pathlib import Path

server = sys.argv[1]
workspace = Path(tempfile.mkdtemp(prefix="heimdall-parameter-tokens-"))
source = ("template <typename TValue>\n"
          "struct Node { TValue value; };\n"
          "template <typename TValue>\n"
          "class LinkedList {\n"
          "public:\n"
          "    void Push(const TValue& value) {\n"
          "        const auto node = new Node<TValue>\n"
          "        {\n"
          "            .value = value\n"
          "        };\n"
          "    }\n"
          "};\n")
file = workspace / "main.cpp"
file.write_text(source)
compile_commands = workspace / "compile_commands.json"
compile_commands.write_text(json.dumps([{
    "directory": workspace.as_posix(),
    "command": f"c++ -std=c++20 -c {file.as_posix()}",
    "file": file.as_posix(),
}]))
process = subprocess.Popen([server], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)


def send(message):
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    process.stdin.flush()


def read_message():
    length = None
    while True:
        line = process.stdout.readline()
        if not line:
            raise SystemExit(f"server closed stdout: {process.stderr.read().decode(errors='replace')}")
        if not line.strip():
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1])
    return json.loads(process.stdout.read(length))


def request(identifier, method, params):
    send({"jsonrpc": "2.0", "id": identifier, "method": method, "params": params})
    while True:
        message = read_message()
        if message.get("id") == identifier:
            return message


initialize = request(1, "initialize", {"capabilities": {}, "initializationOptions": {
    "enableSemantic": True, "compileCommands": compile_commands.as_posix()}})
provider = initialize["result"]["capabilities"]["semanticTokensProvider"]
types = provider["legend"]["tokenTypes"]
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
    "uri": file.as_uri(), "languageId": "cpp", "version": 1, "text": source}}})
result = request(2, "textDocument/semanticTokens/full", {
    "textDocument": {"uri": file.as_uri()}})["result"]
data = result["data"]
tokens = []
line = character = 0
for index in range(0, len(data), 5):
    delta_line, delta_character, length, token_type, modifiers = data[index:index + 5]
    line += delta_line
    character = character + delta_character if delta_line == 0 else delta_character
    line_start = sum(len(part) + 1 for part in source.splitlines()[:line])
    tokens.append((line_start + character, types[token_type], modifiers))

declaration = source.index("const TValue& value") + len("const TValue& ")
reference = source.index(".value = value") + len(".value = ")
member_designator = source.index(".value = value") + 1
by_offset = {offset: (token_type, modifiers) for offset, token_type, modifiers in tokens}
if by_offset.get(declaration, (None,))[0] != "parameter":
    process.kill()
    raise SystemExit(f"parameter declaration is not parameter-colored: {by_offset.get(declaration)}")
if by_offset.get(reference) != by_offset.get(declaration):
    process.kill()
    raise SystemExit(f"parameter reference differs from declaration: "
                     f"{by_offset.get(reference)} != {by_offset.get(declaration)}")
if by_offset.get(member_designator, (None,))[0] == "parameter":
    process.kill()
    raise SystemExit("designated member name was incorrectly highlighted as the parameter")
process.kill()
