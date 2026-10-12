"""Isolated strict regressions; requires only Python and the built Heimdall LSP."""

import argparse
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from LspComparison import Client, locations


def marked(text):
    if text.count("$") != 1:
        raise ValueError("a regression source must contain exactly one cursor marker")
    offset = text.index("$")
    prefix = text[:offset]
    position = {"line": prefix.count("\n"),
                "character": len(prefix.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}
    return text.replace("$", ""), position


def occurrence(text, token, index=0):
    offset = -1
    for _ in range(index + 1):
        offset = text.index(token, offset + 1)
    prefix = text[:offset]
    start = {"line": prefix.count("\n"),
             "character": len(prefix.rsplit("\n", 1)[-1].encode("utf-16-le")) // 2}
    return {"start": start, "end": dict(start, character=start["character"] +
                                        len(token.encode("utf-16-le")) // 2)}


def hover_text(result):
    if not result:
        return ""
    contents = result.get("contents", "")
    if isinstance(contents, list):
        return "\n".join(item if isinstance(item, str) else item.get("value", "")
                         for item in contents)
    return contents if isinstance(contents, str) else contents.get("value", "")


class Session:
    def __init__(self, server, root, source, text, database):
        self.source = source
        self.text = text
        self.client = Client([server], root, timeout=20, memory_limit=4096)
        try:
            self.client.request("initialize", {
                "processId": os.getpid(), "rootUri": root.as_uri(),
                "capabilities": {"workspace": {"workspaceEdit": {"documentChanges": True}}},
                "initializationOptions": {"enableSemantic": True,
                    "compileCommands": str(database), "workspaceSymbols": True,
                    "workspaceDiagnostics": True}})
            self.client.notify("initialized", {})
            self.open(text, 1)
            self.ready(1)
        except BaseException:
            self.close()
            raise

    def open(self, text, version):
        self.client.notify("textDocument/didOpen", {"textDocument": {
            "uri": self.source.as_uri(), "languageId": "cpp", "version": version,
            "text": text}})

    def request(self, method, position=None, **extra):
        params = {"textDocument": {"uri": self.source.as_uri()}, **extra}
        if position is not None:
            params["position"] = position
        return self.client.request(method, params)[1]

    def diagnostics(self, version):
        publications = [message["params"] for message in list(self.client.notifications)
                        if message.get("method") == "textDocument/publishDiagnostics"
                        and message.get("params", {}).get("uri") == self.source.as_uri()
                        and message["params"].get("version") == version]
        return publications[-1]["diagnostics"] if publications else None

    def ready(self, version):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            completion = self.request("textDocument/completion", {"line": 0, "character": 0})
            incomplete = isinstance(completion, dict) and completion.get("isIncomplete", False)
            if not incomplete and self.diagnostics(version) is not None:
                return
            time.sleep(0.02)
        raise TimeoutError("document diagnostics/header indexing did not become ready")

    def change(self, text, version):
        self.client.notify("textDocument/didChange", {
            "textDocument": {"uri": self.source.as_uri(), "version": version},
            "contentChanges": [{"text": text}]})
        self.ready(version)

    def close(self):
        self.client.close()


class RegressionCase(unittest.TestCase):
    server = None
    compiler = None

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="lsp-regression-", dir="/tmp/opencode"
                                               if Path("/tmp/opencode").is_dir() else None)
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def start(self, annotated, headers=None, reflection=False):
        text, self.position = marked(annotated)
        source = self.write("Main.cpp", text)
        for name, header in (headers or {}).items():
            self.write(name, header)
        arguments = [self.compiler, "-std=c++26" if reflection else "-std=c++20",
                     "-I" + str(self.root), "-I" + str(self.root / "build/_deps/widget-src/include")]
        if reflection:
            arguments.append("-freflection")
        arguments += ["-c", str(source)]
        database = self.write("compile_commands.json", json.dumps([
            {"directory": str(self.root), "file": str(source), "arguments": arguments}]))
        self.write(".heimdall.json", json.dumps({"root": True}))
        self.session = Session(self.server, self.root, source, text, database)
        self.addCleanup(self.session.close)
        return text

    def query(self, method, **extra):
        return self.session.request(method, self.position, **extra)

    def assert_locations(self, response, expected):
        actual = locations(response)
        key = lambda item: (item["uri"], item["range"]["start"]["line"],
                            item["range"]["start"]["character"],
                            item["range"]["end"]["line"], item["range"]["end"]["character"])
        self.assertEqual(sorted(actual, key=key), sorted(expected, key=key), response)

    def location(self, relative, text, token, index=0):
        return {"uri": (self.root / relative).as_uri(), "range": occurrence(text, token, index)}


class SyntheticRegressions(RegressionCase):
    widget = "#pragma once\nstruct Widget { int Value() const { return 42; } };\n"

    def test_dependency_member_definition_after_header_index_is_ready(self):
        self.start('#include "Widget.hpp"\nint main() { Widget object; return object.$Value(); }\n',
                   {"build/_deps/widget-src/include/Widget.hpp": self.widget})

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("build/_deps/widget-src/include/Widget.hpp",
                                                     self.widget, "Value")])

    def test_member_completion_includes_dependency_method(self):
        self.start('#include "Widget.hpp"\nint main() { Widget object; object.$ }\n',
                   {"build/_deps/widget-src/include/Widget.hpp": self.widget})

        result = self.query("textDocument/completion")

        items = result if isinstance(result, list) else result["items"]
        matches = [item for item in items if item["label"] == "Value"]
        self.assertEqual(len(matches), 1, result)
        self.assertIn("int", matches[0].get("detail", ""))

    def test_definition_selects_int_overload_from_argument(self):
        text = self.start("int Select(int value) { return value; }\n"
                          "double Select(double value) { return value; }\n"
                          "int main() { return $Select(1); }\n")

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("Main.cpp", text, "Select")])

    def test_signature_help_exposes_both_overloads(self):
        self.start("int Select(int value) { return value; }\n"
                   "double Select(double value) { return value; }\n"
                   "int main() { return Select($1); }\n")

        result = self.query("textDocument/signatureHelp")

        self.assertEqual(len(result["signatures"]), 2, result)
        labels = [signature["label"] for signature in result["signatures"]]
        self.assertTrue(any("int value" in label for label in labels), labels)
        self.assertTrue(any("double value" in label for label in labels), labels)

    def shadowed(self):
        return self.start("int main() {\n    int $value = 1;\n"
                          "    { int value = 2; value += 1; }\n    return value;\n}\n")

    def test_local_references_exclude_shadowing_variable(self):
        text = self.shadowed()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        self.assert_locations(result, [self.location("Main.cpp", text, "value", index)
                                       for index in [0, 3]])

    def test_local_rename_excludes_shadowing_variable_and_preserves_versions(self):
        text = self.shadowed()

        result = self.query("textDocument/rename", newName="renamedValue")

        self.assertIsNotNone(result)
        edits = []
        for change in result.get("documentChanges", []):
            self.assertEqual(change["textDocument"]["uri"], self.session.source.as_uri())
            self.assertEqual(change["textDocument"]["version"], 1)
            edits += change["edits"]
        self.assertEqual(len(edits), 2, result)
        self.assertEqual(sorted(edits, key=lambda item: item["range"]["start"]["line"]),
                         [{"range": occurrence(text, "value", index), "newText": "renamedValue"}
                          for index in [0, 3]])

    def test_template_member_hover_substitutes_type_argument(self):
        self.start("struct Widget {};\ntemplate<class T> struct Box { T payload; };\n"
                   "int main() { Box<Widget> box; box.$payload; }\n")

        result = self.query("textDocument/hover")

        self.assertIn("Widget", hover_text(result), result)
        self.assertNotIn("payload: T", hover_text(result), result)

    def registry(self):
        header = "#pragma once\nstruct ThreadPool { bool IsRunning() const { return true; } };\n"
        self.start('#include "ThreadPool.hpp"\nstruct Registry { ThreadPool* mThreadPool;\n'
                   'void ExecuteTasks() { const bool wasRunning = mThreadPool->$IsRunning(); }\n};\n',
                   {"ThreadPool.hpp": header})
        return header

    def test_registry_pointer_member_definition_resolves_header_method(self):
        header = self.registry()

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("ThreadPool.hpp", header, "IsRunning")])

    def test_registry_pointer_member_hover_exposes_return_type(self):
        self.registry()

        result = self.query("textDocument/hover")

        self.assertIn("bool", hover_text(result), result)
        self.assertIn("IsRunning", hover_text(result), result)

    def reflection_operand(self):
        return self.start("using Entity = unsigned int;\nconstexpr auto info = ^^$Entity;\n",
                          reflection=True)

    def test_reflected_operand_hover_resolves_entity_alias(self):
        self.reflection_operand()

        result = self.query("textDocument/hover")

        self.assertIn("Entity", hover_text(result), result)
        self.assertIn("unsigned int", hover_text(result), result)

    def test_reflected_operand_definition_resolves_alias(self):
        text = self.reflection_operand()

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("Main.cpp", text, "Entity")])

    def test_reflected_operand_references_include_reflection_expression(self):
        text = self.reflection_operand()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        self.assert_locations(result, [self.location("Main.cpp", text, "Entity", index)
                                       for index in [0, 1]])

    def splice(self):
        return self.start("using Entity = unsigned int;\n"
                          "consteval auto ComponentsTupleInfo() { return ^^Entity; }\n"
                          "using Reflected = [:$ComponentsTupleInfo():];\n", reflection=True)

    def test_splice_call_definition_targets_function_not_namespace(self):
        text = self.splice()

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("Main.cpp", text, "ComponentsTupleInfo")])

    def test_splice_call_hover_describes_function(self):
        self.splice()

        result = self.query("textDocument/hover")

        self.assertIn("ComponentsTupleInfo", hover_text(result), result)

    def test_splice_call_references_include_declaration_and_splice(self):
        text = self.splice()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        self.assert_locations(result, [self.location("Main.cpp", text, "ComponentsTupleInfo", index)
                                       for index in [0, 1]])

    def test_reflection_syntax_does_not_publish_parse_errors(self):
        self.splice()

        diagnostics = self.session.diagnostics(1)

        self.assertIsNotNone(diagnostics)
        self.assertFalse([item for item in diagnostics if item.get("severity") == 1], diagnostics)

    def test_namespace_scope_references_include_static_assert_uses(self):
        text = self.start("constexpr int $xs = 1;\nstatic_assert(xs == 1);\n"
                          "static_assert(xs != 0);\nstatic_assert(xs > 0);\n")

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        self.assert_locations(result, [self.location("Main.cpp", text, "xs", index)
                                       for index in range(4)])

    def macro_call(self):
        return self.start("#define RUNTIME_CHECK(...) do { if (!(__VA_ARGS__)) {} } while (false)\n"
                          "namespace hana { int transform(int input) { return input; } }\n"
                          "int main() { RUNTIME_CHECK(hana::$transform(1) == 1); }\n")

    def test_assertion_macro_preserves_call_definition_source_range(self):
        text = self.macro_call()

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("Main.cpp", text, "transform")])

    def test_assertion_macro_preserves_call_hover(self):
        self.macro_call()

        result = self.query("textDocument/hover")

        self.assertIn("transform", hover_text(result), result)
        self.assertIn("int", hover_text(result), result)

    def test_assertion_macro_preserves_call_references_source_ranges(self):
        text = self.macro_call()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        self.assert_locations(result, [self.location("Main.cpp", text, "transform", index)
                                       for index in [0, 1]])

    def test_iwyu_accepts_public_header_exporting_macro_from_detail_header(self):
        self.start('#include <library/define_struct.hpp>\nDEFINE_STRUCT($Person);\n', {
            "library/define_struct.hpp": "#pragma once\n#include <library/detail/struct_macros.hpp>\n",
            "library/detail/struct_macros.hpp": "#pragma once\n#define DEFINE_STRUCT(Name) struct Name {}\n"})

        diagnostics = self.session.diagnostics(1)

        self.assertIsNotNone(diagnostics)
        self.assertFalse([item for item in diagnostics
                          if item.get("code") == "cpp/include-what-you-use"], diagnostics)

    def test_iwyu_still_reports_genuinely_indirect_header_use(self):
        self.start('#include "Wrapper.hpp"\n$Hidden object;\n', {
            "Wrapper.hpp": '#pragma once\n#include "Hidden.hpp"\nstruct Wrapper {};\n',
            "Hidden.hpp": "#pragma once\nstruct Hidden {};\n"})

        diagnostics = self.session.diagnostics(1)

        self.assertTrue(any(item.get("code") == "cpp/include-what-you-use"
                            and "Hidden.hpp" in item["message"] for item in diagnostics), diagnostics)


class HanaTemplateRegressions(RegressionCase):
    tuple_header = '''#pragma once
namespace library::hana {
template<class First, class Second, class Third>
struct Tuple { First first; Second second; Third third; };
template<class First, class Second, class Third>
constexpr auto make_tuple(First first, Second second, Third third) {
    return Tuple<First, Second, Third> {first, second, third};
}
template<unsigned index, class First, class Second, class Third>
constexpr decltype(auto) at_c(const Tuple<First, Second, Third>& tuple) {
    if constexpr (index == 0) { return (tuple.first); }
    else if constexpr (index == 1) { return (tuple.second); }
    else { return (tuple.third); }
}
}
'''
    tuple_source = '''#include "Tuple.hpp"
namespace hana = library::hana;
constexpr auto xs = hana::make_tuple(0, '1', 2.0);
static_assert(hana::at_c<0>(xs) == 0);
static_assert(hana::at_c<1>(xs) == '1');
static_assert(hana::at_c<2>(xs) == 2.0);
int main() {}
'''
    macros = '''#pragma once
#define CHECK_IMPL(...) do { if (!(__VA_ARGS__)) {} } while (false)
#define RUNTIME_CHECK(...) CHECK_IMPL(__VA_ARGS__)
'''

    def tuple_call(self):
        return self.start(self.tuple_source.replace("at_c<0>", "$at_c<0>"),
                          {"Tuple.hpp": self.tuple_header})

    def tuple_variable(self):
        return self.start(self.tuple_source.replace("auto xs", "auto $xs"),
                          {"Tuple.hpp": self.tuple_header})

    def test_tuple_call_definition_selects_instantiated_const_overload(self):
        self.tuple_call()

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("Tuple.hpp", self.tuple_header, "at_c")])

    def test_tuple_call_hover_exposes_instantiated_return_type(self):
        self.tuple_call()

        result = self.query("textDocument/hover")

        compact = "".join(hover_text(result).split())
        self.assertIn("at_c", compact, result)
        self.assertIn("constint&", compact, result)

    def test_tuple_call_references_include_all_three_indices(self):
        text = self.tuple_call()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        expected = [self.location("Main.cpp", text, "at_c", index) for index in range(3)]
        for item in expected:
            self.assertIn(item, locations(result), result)

    def test_tuple_variable_hover_deduces_concrete_element_types(self):
        self.tuple_variable()

        result = self.query("textDocument/hover")

        self.assertIn("Tuple<int,char,double>", "".join(hover_text(result).split()), result)

    def test_tuple_variable_references_include_declaration_and_static_asserts(self):
        text = self.tuple_variable()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        self.assert_locations(result, [self.location("Main.cpp", text, "xs", index)
                                       for index in range(4)])

    def accessor(self):
        return self.start('''#include "Accessors.hpp"
#include "Checks.hpp"
DEFINE_STRUCT(Person);
int main() {
    constexpr auto get_age = library::GetAge<Person>();
    Person john {30};
    RUNTIME_CHECK($get_age(john) == 30);
}
''', {"Checks.hpp": self.macros,
      "library/define_struct.hpp": '#pragma once\n#include "detail/struct_macros.hpp"\n',
      "library/detail/struct_macros.hpp": "#pragma once\n#define DEFINE_STRUCT(Name) struct Name { int age; }\n",
      "Accessors.hpp": '''#pragma once
#include "library/define_struct.hpp"
namespace library {
template<class Record> struct AgeAccessor {
    constexpr int operator()(const Record& record) const { return record.age; }
};
template<class Record> constexpr auto GetAge() { return AgeAccessor<Record> {}; }
}
'''})

    def test_macro_accessor_definition_resolves_local_callable(self):
        text = self.accessor()

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("Main.cpp", text, "get_age")])

    def test_macro_accessor_hover_has_deduced_type_not_auto_placeholder(self):
        self.accessor()

        result = self.query("textDocument/hover")

        self.assertIn("get_age", hover_text(result), result)
        self.assertNotIn("constexpr auto", hover_text(result), result)
        self.assertIn("AgeAccessor", hover_text(result), result)

    def test_macro_accessor_references_include_declaration_and_call(self):
        text = self.accessor()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        self.assert_locations(result, [self.location("Main.cpp", text, "get_age", index)
                                       for index in range(2)])

    def test_public_define_struct_include_does_not_require_private_detail_header(self):
        self.start('#include "library/define_struct.hpp"\nDEFINE_STRUCT($Person);\n', {
            "library/define_struct.hpp": '#pragma once\n#include "detail/struct_macros.hpp"\n',
            "library/detail/struct_macros.hpp": "#pragma once\n#define DEFINE_STRUCT(Name) struct Name { int age; }\n"})

        diagnostics = self.session.diagnostics(1)

        self.assertIsNotNone(diagnostics)
        self.assertFalse([item for item in diagnostics
                          if item.get("code") == "cpp/include-what-you-use"
                          and "DEFINE_STRUCT" in item["message"]], diagnostics)

    def transform(self):
        return self.start('''#include "Transform.hpp"
#include "Checks.hpp"
namespace hana = library::hana;
int main() {
    auto to_value = [](auto value) { return value; };
    RUNTIME_CHECK(hana::$transform(1, to_value) == 1);
    RUNTIME_CHECK(hana::transform(2, to_value) == 2);
    RUNTIME_CHECK(hana::transform(3, to_value) == 3);
    RUNTIME_CHECK(hana::transform(4, to_value) == 4);
}
''', {"Checks.hpp": self.macros, "Transform.hpp": '''#pragma once
namespace library::hana {
template<class Input, class Function>
constexpr auto transform(Input input, Function function) { return function(input); }
}
'''})

    def test_transform_inside_assertion_macro_has_definition(self):
        self.transform()

        result = self.query("textDocument/definition")

        header = (self.root / "Transform.hpp").read_text(encoding="utf-8")
        self.assert_locations(result, [self.location("Transform.hpp", header, "transform")])

    def test_transform_inside_assertion_macro_has_hover(self):
        self.transform()

        result = self.query("textDocument/hover")

        self.assertIn("transform", hover_text(result), result)

    def test_transform_inside_assertion_macro_references_cover_calls(self):
        text = self.transform()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        expected = [self.location("Main.cpp", text, "transform", index) for index in range(4)]
        actual = locations(result)
        for item in expected:
            self.assertIn(item, actual, result)

    def test_comment_only_edit_preserves_tuple_definition_and_hover(self):
        self.tuple_call()
        original = self.session.source.read_bytes()

        self.session.change(self.session.text + "\n// regression revision\n", 2)
        definition = self.query("textDocument/definition")
        hover = self.query("textDocument/hover")

        self.assertTrue(locations(definition), definition)
        self.assertIn("constint&", "".join(hover_text(hover).split()), hover)
        self.assertEqual(self.session.source.read_bytes(), original)


class FreyrReflectionRegressions(RegressionCase):
    pool_header = '''#pragma once
namespace skr {
template<class T> struct Arc {
    T* pointer;
    T* operator->() const { return pointer; }
};
}
namespace fr {
struct ThreadPool { bool IsRunning() const { return true; } };
}
'''
    entity_header = '''#pragma once
#define FREYR_NAMESPACE fr
namespace FREYR_NAMESPACE {
using Entity = unsigned int;
}
'''

    def registry(self):
        return self.start('''#include "ThreadPool.hpp"
namespace fr {
struct Registry {
    skr::Arc<ThreadPool> mThreadPool;
    void ExecuteTasks() {
        const bool wasRunning = mThreadPool->$IsRunning();
    }
};
}
''', {"ThreadPool.hpp": self.pool_header})

    def test_registry_is_running_definition_resolves_thread_pool_method(self):
        self.registry()

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("ThreadPool.hpp", self.pool_header, "IsRunning")])

    def test_registry_is_running_hover_exposes_bool_result(self):
        self.registry()

        result = self.query("textDocument/hover")

        self.assertIn("IsRunning", hover_text(result), result)
        self.assertIn("bool", hover_text(result), result)

    def test_registry_is_running_references_include_queried_call(self):
        text = self.registry()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        self.assertIn(self.location("Main.cpp", text, "IsRunning"), locations(result))

    def reflected_entity(self):
        return self.start('''#include "Entity.hpp"
namespace FREYR_NAMESPACE {
template<class F> consteval auto ReflectEntity() { return ^^$Entity; }
}
''', {"Entity.hpp": self.entity_header}, reflection=True)

    def test_reflected_entity_hover_resolves_header_alias(self):
        self.reflected_entity()

        result = self.query("textDocument/hover")

        self.assertIn("Entity", hover_text(result), result)
        self.assertIn("unsigned int", hover_text(result), result)

    def test_reflected_entity_definition_resolves_entity_header(self):
        self.reflected_entity()

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("Entity.hpp", self.entity_header, "Entity")])

    def test_reflected_entity_references_include_reflected_operand(self):
        text = self.reflected_entity()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        target = occurrence(text, "^^Entity")
        target["start"]["character"] += 2
        self.assertIn({"uri": self.session.source.as_uri(), "range": target}, locations(result))

    def splice(self):
        return self.start('''#include "Entity.hpp"
namespace FREYR_NAMESPACE {
namespace meta::detail {
template<class F> consteval auto ComponentsTupleInfo() { return ^^F; }
}
using Components = [:meta::detail::$ComponentsTupleInfo<Entity>():];
}
''', {"Entity.hpp": self.entity_header}, reflection=True)

    def test_splice_definition_resolves_components_tuple_function(self):
        text = self.splice()

        result = self.query("textDocument/definition")

        self.assert_locations(result, [self.location("Main.cpp", text, "ComponentsTupleInfo")])

    def test_splice_hover_describes_components_tuple_function(self):
        self.splice()

        result = self.query("textDocument/hover")

        self.assertIn("ComponentsTupleInfo", hover_text(result), result)

    def test_splice_references_include_the_function_and_alias_use(self):
        text = self.splice()

        result = self.query("textDocument/references", context={"includeDeclaration": True})

        for index in range(2):
            self.assertIn(self.location("Main.cpp", text, "ComponentsTupleInfo", index), locations(result))

    def test_reflection_header_has_no_syntax_error_diagnostics(self):
        self.splice()

        diagnostics = self.session.diagnostics(1)

        self.assertIsNotNone(diagnostics)
        self.assertFalse([item for item in diagnostics if item.get("severity") == 1], diagnostics)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    executable = "heimdall-lsp.exe" if os.name == "nt" else "heimdall-lsp"
    parser.add_argument("--heimdall", default=str(Path(__file__).resolve().parents[1] /
                                                 "build/lsp" / executable))
    parser.add_argument("--compiler", default="c++",
                        help="compiler name recorded in the fixture database; never executed")
    parser.add_argument("--filter", default="", help="substring of a test ID")
    args = parser.parse_args()
    server = shutil.which(args.heimdall)
    if not server:
        parser.error("server must exist")
    RegressionCase.server = str(Path(server).resolve())
    RegressionCase.compiler = args.compiler
    classes = [SyntheticRegressions, HanaTemplateRegressions, FreyrReflectionRegressions]
    suite = unittest.TestSuite(test for cls in classes
                               for test in unittest.defaultTestLoader.loadTestsFromTestCase(cls)
                               if args.filter in test.id())
    if suite.countTestCases() == 0:
        parser.error("filter did not select any regression")
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
