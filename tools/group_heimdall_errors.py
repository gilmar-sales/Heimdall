#!/usr/bin/env python3
"""Group Heimdall JSON diagnostics without modifying the analyzed sources."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys


def normalize_message(message):
    message = re.sub(r"expected \d+, received \d+", "expected <N>, received <N>", message)
    return re.sub(r"for '[^']+'", "for '<callee>'", message)


def component(file, root):
    path = Path(file)
    try:
        relative = path.relative_to(root).as_posix()
    except ValueError:
        return "external"
    parts = relative.split("/")
    if "_deps" in parts:
        index = parts.index("_deps")
        return parts[index + 1] if index + 1 < len(parts) else "_deps"
    if parts[0] == "build":
        return "build/other"
    return parts[0]


def categorize(code, message, source):
    if code == "cpp/call-argument-count":
        return "call-arity", "Investigar sobrecargas, casts, macros e visibilidade de headers."
    if code != "syntax/parse-error":
        return code.split("/", 1)[0], "Investigar a regra correspondente."
    if re.search(r"\bnew\s+(?:unsigned|signed|long|short)\b", source):
        return "new-compound-type", "Possível falha em new-expression com tipo composto."
    if source.lstrip().startswith("#") or re.search(r"\b[A-Z][A-Z0-9_]+\s*\(", source):
        return "macro-context", "Possível expansão ou recuperação de macro; validar contexto."
    if "end of compound statement" in message:
        return "compound-statement", "Investigar statement, construtor e macros anteriores."
    if "expected ';'" in message:
        return "expected-semicolon", "Investigar construção anterior; não inserir ';' automaticamente."
    return "other-syntax", "Reduzir a reprodução e validar o dialeto."


def load_diagnostics(paths):
    seen = set()
    errors = []
    totals = Counter()
    for path in paths:
        with path.open(encoding="utf-8") as stream:
            data = json.load(stream)
        if not isinstance(data, list):
            raise ValueError(f"{path}: esperado um array JSON de diagnósticos")
        for index, diagnostic in enumerate(data):
            if not isinstance(diagnostic, dict):
                raise ValueError(f"{path}: diagnóstico {index} não é um objeto")
            totals["input_diagnostics"] += 1
            if diagnostic.get("severity") != "error":
                continue
            for field in ("file", "code", "message"):
                if not isinstance(diagnostic.get(field), str):
                    raise ValueError(f"{path}: diagnóstico {index}: {field} inválido")
            for field in ("line", "column"):
                value = diagnostic.get(field)
                if type(value) is not int or value < 1:
                    raise ValueError(f"{path}: diagnóstico {index}: {field} inválido")
            totals["input_errors"] += 1
            key = tuple(diagnostic[field] for field in
                        ("file", "line", "column", "code", "message"))
            if key in seen:
                totals["duplicate_errors"] += 1
                continue
            seen.add(key)
            errors.append(dict(diagnostic))
    return errors, totals


def build_report(paths, root, examples=5, read_source=True):
    errors, totals = load_diagnostics(paths)
    groups = {}
    for error in errors:
        error["component"] = component(error["file"], root)
        error["source_line"] = ""
    if read_source:
        by_file = {}
        for error in errors:
            by_file.setdefault(error["file"], []).append(error)
        for file, diagnostics in by_file.items():
            wanted = {error["line"] for error in diagnostics}
            lines = {}
            try:
                with Path(file).open(encoding="utf-8", errors="replace") as stream:
                    for number, line in enumerate(stream, 1):
                        if number in wanted:
                            lines[number] = line.rstrip("\r\n")[:1000]
                        if number >= max(wanted):
                            break
                for error in diagnostics:
                    error["source_line"] = lines.get(error["line"], "")
                    error["source_status"] = "read" if error["line"] in lines else "line-missing"
            except (OSError, ValueError):
                for error in diagnostics:
                    error["source_status"] = "unavailable"
    for error in errors:
        category, hypothesis = categorize(error["code"], error["message"], error["source_line"])
        message = normalize_message(error["message"])
        key = (category, error["code"], message)
        if key not in groups:
            identifier = hashlib.sha256(json.dumps(key).encode()).hexdigest()[:12]
            groups[key] = {"id": identifier, "category": category, "code": error["code"],
                           "normalized_message": message, "hypothesis": hypothesis,
                           "occurrences": []}
        groups[key]["occurrences"].append(error)
    ordered = []
    for group in groups.values():
        occurrences = sorted(group["occurrences"], key=lambda d: (d["file"], d["line"], d["column"]))
        group.update(count=len(occurrences), files=len({d["file"] for d in occurrences}),
                     components=dict(sorted(Counter(d["component"] for d in occurrences).items())),
                     examples=occurrences[:examples], occurrences=occurrences)
        ordered.append(group)
    ordered.sort(key=lambda group: (-group["count"], group["category"], group["normalized_message"]))
    categories = Counter()
    for group in ordered:
        categories[group["category"]] += group["count"]
    return {"root": str(root), "inputs": [str(path) for path in paths],
            "limitations": ["Categorias são hipóteses, não causas confirmadas.",
                            "Linhas são lidas do estado atual, que pode diferir da coleta.",
                            "build/other não significa necessariamente código gerado.",
                            "Contagens incluem apenas os diagnósticos fornecidos, não cobertura de análise."],
            "summary": {**totals, "unique_errors": len(errors),
                        "files_with_errors": len({d["file"] for d in errors}),
                        "groups": len(ordered),
                        "by_category": dict(sorted(categories.items())),
                        "by_component": dict(Counter(d["component"] for d in errors).most_common())},
            "groups": ordered}


def markdown(report):
    summary = report["summary"]
    lines = ["# Erros do Heimdall — triagem", "",
             f"**{summary['unique_errors']} erros únicos**, em **{summary['files_with_errors']} arquivos**, "
             f"agrupados em **{summary['groups']} famílias**.", "",
             f"Duplicatas removidas: {summary.get('duplicate_errors', 0)}.", "",
             *[f"- {text}" for text in report["limitations"]], "", "## Por componente", ""]
    lines.extend(f"- `{name}`: {count}" for name, count in summary["by_component"].items())
    for group in report["groups"]:
        lines.extend(["", f"## {group['id']} — {group['category']}", "",
                      f"{group['count']} ocorrências em {group['files']} arquivos. Regra: `{group['code']}`.",
                      "", group["normalized_message"], "", f"Hipótese: {group['hypothesis']}", "",
                      "Componentes: " + ", ".join(f"{key} ({value})" for key, value in group["components"].items()), "",
                      "Exemplos:"])
        for example in group["examples"]:
            lines.append(f"- `{example['file']}:{example['line']}:{example['column']}`")
            if example["source_line"]:
                lines.extend(["", "```cpp", example["source_line"], "```"])
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help="JSON arrays emitted by Heimdall")
    parser.add_argument("--root", required=True, type=Path, help="Root of the analyzed project")
    parser.add_argument("--output", required=True, type=Path, help="New output directory (never overwritten)")
    parser.add_argument("--examples", type=int, default=5)
    parser.add_argument("--no-source", action="store_true", help="Do not read analyzed source files")
    args = parser.parse_args()
    if args.examples < 1:
        parser.error("--examples must be positive")
    try:
        report = build_report(args.inputs, args.root.resolve(), args.examples, not args.no_source)
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "groups.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        (args.output / "summary.md").write_text(markdown(report), encoding="utf-8")
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report["summary"], ensure_ascii=False, indent=2))
    print(f"Reports: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
