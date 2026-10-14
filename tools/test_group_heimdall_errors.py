import json
from pathlib import Path
import tempfile
import unittest

from group_heimdall_errors import build_report, categorize, component, normalize_message


class GroupErrorsSpec(unittest.TestCase):
    def test_groups_and_deduplicates_overlapping_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "build" / "_deps" / "assimp-src" / "example.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("auto* p = new unsigned int[3];\n")
            diagnostic = {"file": str(source), "line": 1, "column": 24,
                          "severity": "error", "code": "syntax/parse-error",
                          "message": "expected ';' before 'int'"}
            first = root / "first.json"
            second = root / "second.json"
            first.write_text(json.dumps([diagnostic, {"severity": "warning"}]))
            second.write_text(json.dumps([diagnostic]))

            report = build_report([first, second], root)

            self.assertEqual(report["summary"]["unique_errors"], 1)
            self.assertEqual(report["summary"]["duplicate_errors"], 1)
            self.assertEqual(report["groups"][0]["category"], "new-compound-type")
            self.assertEqual(report["summary"]["by_component"], {"assimp-src": 1})

    def test_missing_source_does_not_drop_diagnostic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "input.json"
            path.write_text(json.dumps([{"file": str(root / "absent.cpp"), "line": 4,
                                        "column": 1, "severity": "error", "code": "syntax/parse-error",
                                        "message": "expected ';' before end of compound statement"}]))

            report = build_report([path], root)

            self.assertEqual(report["summary"]["unique_errors"], 1)
            self.assertEqual(report["groups"][0]["examples"][0]["source_status"], "unavailable")

    def test_normalizes_callees_and_counts(self):
        self.assertEqual(normalize_message("no matching argument count for 'f': expected 2, received 1"),
                         normalize_message("no matching argument count for 'g': expected 3, received 2"))

    def test_does_not_label_all_build_files_as_generated(self):
        self.assertEqual(component("/project/build/sdk/x.cpp", Path("/project")), "build/other")
        self.assertEqual(component("/elsewhere/x.cpp", Path("/project")), "external")

    def test_macro_classification_is_a_hypothesis(self):
        self.assertEqual(categorize("syntax/parse-error", "expected ';'", " MACRO(value)")[0],
                         "macro-context")

    def test_rejects_non_array_input(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "invalid.json"
            path.write_text("{}")

            with self.assertRaises(ValueError):
                build_report([path], Path(directory))


if __name__ == "__main__":
    unittest.main()
