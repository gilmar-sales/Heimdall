"""Deterministic semantic highlight checks; clangd differential is optional."""

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from TokenComparison import collect


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--heimdall", required=True)
    parser.add_argument("--clangd")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="token-parity-") as directory:
        root = Path(directory).resolve()
        fixture = Path(__file__).parent / "fixtures/token_parity"
        for file in fixture.iterdir():
            shutil.copyfile(file, root / file.name)
        source = root / "Main.cpp"
        text = source.read_text(encoding="utf-8")
        database = root / "compile_commands.json"
        database.write_text(json.dumps([{"directory": str(root), "file": str(source),
            "arguments": ["clang++", "-std=c++20", "-I" + str(root), "-c", str(source)]}]),
            encoding="utf-8")
        (root / ".heimdall.json").write_text('{"root":true,"rules":{}}', encoding="utf-8")
        result = collect([str(Path(args.heimdall).resolve())], root, database,
                         [(source, text)], 30)
        tokens = result["tokens"][str(source)]

        def token_at(tokens, phrase, name):
            prefix = text[:text.index(phrase) + phrase.index(name)]
            line = prefix.count("\n")
            column = len(prefix.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2
            return next(token for token in tokens
                        if token["line"] == line and token["character"] == column)

        checks = [("int sum", "sum", "variable"), ("sum += index", "index", "variable"),
                  ("object.value", "value", "property"), ("first::Kind::Count", "Kind", "enum"),
                  ("first::Kind::Count", "Count", "enumMember"),
                  ("first::Alias::Count", "Count", "enumMember"),
                  ("second::Kind other", "Kind", "class")]
        for phrase, name, kind in checks:
            actual = token_at(tokens, phrase, name)
            assert actual["type"] == kind, actual
        assert "readonly" not in token_at(tokens, "const int* pointee", "pointee")["modifiers"]
        assert "readonly" in token_at(tokens, "pointer = &value", "pointer")["modifiers"]
        if args.clangd:
            reference = collect([args.clangd, "--log=error", "--background-index=false",
                                 "--compile-commands-dir=" + str(root)], root, database,
                                [(source, text)], 30)["tokens"][str(source)]
            for phrase, name, _ in checks:
                assert token_at(tokens, phrase, name)["type"] == \
                       token_at(reference, phrase, name)["type"], (phrase, name)
        print(f"Token parity checks passed ({len(checks)} categories, 2 constness checks)")


if __name__ == "__main__":
    main()
