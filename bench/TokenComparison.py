"""Compare semantic tokens from Heimdall and clangd using their own LSP legends."""

import argparse
import hashlib
import json
import os
import shutil
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True

from LspComparison import Client

TOKEN_TYPES = ["namespace", "type", "class", "enum", "interface", "struct",
               "typeParameter", "parameter", "variable", "property", "enumMember",
               "event", "function", "method", "macro", "keyword", "modifier",
               "comment", "string", "number", "regexp", "operator", "decorator",
               "concept", "bracket", "label", "doxygen", "unknown"]
TOKEN_MODIFIERS = ["declaration", "definition", "readonly", "static", "deprecated",
                   "abstract", "async", "modification", "documentation", "defaultLibrary",
                   "inactive", "globalScope", "fileScope", "classScope", "functionScope",
                   "deduced", "virtual", "dependentName", "usedAsMutableReference",
                   "usedAsMutablePointer", "constructorOrDestructor", "userDefined"]
LEXICAL_TYPES = {"keyword", "comment", "string", "number", "doxygen"}


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def executable_metadata(command, root):
    path = Path(shutil.which(command[0]) or command[0])
    if not path.is_absolute():
        path = root / path
    path = path.resolve(strict=True)
    return {"command": command, "path": str(path), "sha256": fingerprint(path)}


def diagnostics_for(notifications, uri, version):
    matching = [message["params"] for message in list(notifications)
                if message.get("method") == "textDocument/publishDiagnostics"
                and message.get("params", {}).get("uri") == uri
                and message["params"].get("version") == version]
    if not matching:
        return {"status": "unknown", "version": version, "diagnostics": None}
    return {"status": "observed", "version": version,
            "diagnostics": matching[-1]["diagnostics"]}


def wait_ready(client, uri, version, timeout, completion_barrier=True):
    deadline = time.monotonic() + timeout
    original_timeout = client.timeout
    incomplete = None
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"analysis of {uri} version {version} did not become ready; "
                                   "versioned diagnostics and complete completion are required; "
                                   f"diagnostics={diagnostics_for(client.notifications, uri, version)['status']}, "
                                   f"completion_incomplete={incomplete}")
            client.timeout = remaining
            completion = []
            if completion_barrier:
                completion = client.request("textDocument/completion", {
                    "textDocument": {"uri": uri}, "position": {"line": 0, "character": 0}})[1]
            diagnostics = diagnostics_for(client.notifications, uri, version)
            incomplete = isinstance(completion, dict) and completion.get("isIncomplete", False)
            if completion is not None and not incomplete and diagnostics["status"] == "observed":
                return {"status": "ready", "version": version,
                        "method": "versioned_diagnostics_and_complete_completion" if completion_barrier
                                  else "versioned_diagnostics_and_full_token_response",
                        "diagnostics": diagnostics}
            time.sleep(min(0.02, max(0, deadline - time.monotonic())))
    finally:
        client.timeout = original_timeout

def decode_tokens(result, legend, text):
    if not isinstance(result, dict) or not isinstance(result.get("data"), list):
        raise ValueError("full semantic token result must contain a data array")
    data = result["data"]
    if len(data) % 5:
        raise ValueError("semantic token data must contain groups of five integers")
    rows = text.split("\n")
    tokens = []
    line = character = 0
    for offset in range(0, len(data), 5):
        delta_line, delta_character, length, kind, bits = data[offset:offset + 5]
        if any(type(value) is not int or value < 0 for value in data[offset:offset + 5]):
            raise ValueError("semantic token fields must be nonnegative integers")
        line += delta_line
        character = delta_character if delta_line else character + delta_character
        types = legend["tokenTypes"]
        modifiers = legend["tokenModifiers"]
        if kind >= len(types) or bits >> len(modifiers) or line >= len(rows) or not length:
            raise ValueError("invalid semantic token type, modifiers, line or length")
        encoded = rows[line].removesuffix("\r").encode("utf-16-le")
        if (character + length) * 2 > len(encoded):
            raise ValueError("semantic token extends past the source line")
        spelling = encoded[character * 2:(character + length) * 2].decode("utf-16-le")
        tokens.append({"line": line, "character": character, "length": length,
                       "text": spelling, "type": types[kind],
                       "modifiers": sorted(name for index, name in enumerate(modifiers)
                                           if bits & (1 << index))})
    return tokens


def compare_tokens(heimdall, clangd):
    def index(tokens):
        indexed = {}
        for token in tokens:
            key = (token["line"], token["character"], token["length"])
            if key in indexed:
                raise ValueError(f"duplicate semantic token range: {key}")
            indexed[key] = token
        return indexed

    actual, reference = index(heimdall), index(clangd)
    differences = []
    matched = 0
    same_type = 0
    modifiers_only = 0
    type_changes = 0
    for key in sorted(actual.keys() | reference.keys()):
        left, right = actual.get(key), reference.get(key)
        if left == right:
            matched += 1
            same_type += 1
            continue
        status = "extra" if right is None else "missing" if left is None else "classification"
        differences.append({"status": status, "heimdall": left, "clangd": right})
        if left and right:
            if left["type"] == right["type"]:
                same_type += 1
                modifiers_only += 1
            else:
                type_changes += 1
    common = len(actual.keys() & reference.keys())
    return {"heimdall_tokens": len(heimdall), "clangd_tokens": len(clangd),
            "matched": matched, "differences": differences,
            "metrics": {"common_ranges": common, "same_type": same_type,
                        "modifiers_only": modifiers_only, "type_changes": type_changes,
                        "reference_range_coverage": common / len(reference) if reference else None,
                        "common_range_type_agreement": same_type / common if common else None,
                        "lexical_extras": sum(d["status"] == "extra" and
                                              d["heimdall"]["type"] in LEXICAL_TYPES
                                              for d in differences)}}


def collect(command, root, database, sources, timeout):
    metadata = executable_metadata(command, root)
    client = Client(command, root, timeout, memory_limit=4096)
    try:
        initialization = client.request("initialize", {
            "processId": os.getpid(), "rootUri": root.as_uri(),
            "capabilities": {"general": {"positionEncodings": ["utf-16"]},
                "textDocument": {"publishDiagnostics": {"versionSupport": True},
                    "semanticTokens": {"requests": {"full": True},
                    "tokenTypes": TOKEN_TYPES, "tokenModifiers": TOKEN_MODIFIERS,
                    "formats": ["relative"]}}},
            "initializationOptions": {"enableSemantic": True,
                                      "compileCommands": str(database)}})[1]
        capabilities = initialization["capabilities"]
        if capabilities.get("positionEncoding", "utf-16") != "utf-16":
            raise ValueError("server did not select UTF-16 positions")
        provider = capabilities.get("semanticTokensProvider")
        if not isinstance(provider, dict) or "full" not in provider or provider["full"] is False:
            raise ValueError("server does not support full semantic tokens")
        metadata.update(server_info=initialization.get("serverInfo"),
                        legend=provider["legend"], position_encoding="utf-16")
        client.notify("initialized", {})
        results, documents = {}, {}
        for source, text in sources:
            client.notify("textDocument/didOpen", {"textDocument": {
                "uri": source.as_uri(), "languageId": "cpp", "version": 1, "text": text}})
            server_name = initialization.get("serverInfo", {}).get("name", "").lower()
            documents[str(source)] = wait_ready(client, source.as_uri(), 1, timeout,
                                                completion_barrier=server_name != "clangd")
            result = client.request("textDocument/semanticTokens/full", {
                "textDocument": {"uri": source.as_uri()}})[1]
            if result is None:
                raise ValueError(f"server returned no semantic token result for {source}")
            results[str(source)] = decode_tokens(result, provider["legend"], text)
            client.notify("textDocument/didClose", {"textDocument": {"uri": source.as_uri()}})
        return {"tokens": results, "metadata": metadata, "documents": documents}
    finally:
        client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--heimdall", required=True, help="path to heimdall-lsp")
    parser.add_argument("--clangd", default="clangd")
    parser.add_argument("--compile-commands", required=True, type=Path)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        database = args.compile_commands.resolve(strict=True)
        if not database.is_file() or database.name != "compile_commands.json":
            raise ValueError("--compile-commands must point to compile_commands.json")
        sources = [(path.resolve(strict=True), path.read_bytes().decode("utf-8"))
                   for path in args.files]
        root = args.root.resolve(strict=True)
        database_hash = fingerprint(database)
        heimdall = collect([args.heimdall], root, database, sources, args.timeout)
        clangd = collect([args.clangd, "--log=error", "--background-index=false",
                          "--clang-tidy=false", "--compile-commands-dir=" + str(database.parent)],
                         root, database, sources, args.timeout)
        report = {"schema_version": 2, "position_encoding": "utf-16", "reference": "clangd",
                  "workspace": str(root),
                  "compile_database": {"path": str(database), "sha256": database_hash},
                  "sources": {str(path): {"sha256": hashlib.sha256(text.encode("utf-8")).hexdigest()}
                              for path, text in sources},
                  "servers": {"heimdall": {key: value for key, value in heimdall.items()
                                           if key != "tokens"},
                              "clangd": {key: value for key, value in clangd.items()
                                         if key != "tokens"}},
                  "limitations": ["Server taxonomies and system header discovery may differ; "
                                   "differences are not automatically defects."],
                  "raw_tokens": {"heimdall": heimdall["tokens"], "clangd": clangd["tokens"]},
                  "files": {str(path): compare_tokens(heimdall["tokens"][str(path)],
                                                      clangd["tokens"][str(path)])
                            for path, _ in sources}}
        output = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            args.output.write_text(output, encoding="utf-8")
        else:
            print(output, end="")
        return int(any(item["differences"] for item in report["files"].values()))
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print(f"Token comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
