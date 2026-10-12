import sys
import tempfile
import unittest
import subprocess
from unittest.mock import MagicMock
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from LspComparison import (Client, assess_result, create_workspace, locations,
                           document_diagnostics, normalize_result, percentile,
                           quality_workspace, reflection_inventory, response_count)
from LspComparisonRegressionSpec import RegressionCase, Session, hover_text, marked, occurrence


class LspComparisonSpec(unittest.TestCase):
    def test_regression_marker_uses_utf16_cursor_units(self):
        text, position = marked('int value;\nconst char* emoji = "😀"; $value;\n')

        self.assertNotIn("$", text)
        self.assertEqual(position, {"line": 1, "character": 26})

    def test_regression_marker_requires_exactly_one_cursor(self):
        for text in ["int value;", "int $value; $value;"]:
            with self.assertRaises(ValueError):
                marked(text)

    def test_regression_ranges_distinguish_repeated_identifiers(self):
        self.assertEqual(occurrence("int value;\nvalue;\n", "value", 1),
                         {"start": {"line": 1, "character": 0},
                          "end": {"line": 1, "character": 5}})

    def test_regression_hover_accepts_all_lsp_content_shapes(self):
        for contents in ["Widget", {"kind": "markdown", "value": "Widget"},
                         [{"language": "cpp", "value": "Widget"}]]:
            self.assertEqual(hover_text({"contents": contents}), "Widget")
        self.assertEqual(hover_text(None), "")

    def test_regression_definition_does_not_hide_duplicate_locations(self):
        location = {"uri": "file:///Main.cpp", "range": {
            "start": {"line": 1, "character": 0}, "end": {"line": 1, "character": 5}}}
        case = RegressionCase()

        with self.assertRaises(AssertionError):
            case.assert_locations([location, location], [location])

    def test_regression_readiness_ignores_other_documents_and_stale_versions(self):
        session = Session.__new__(Session)
        session.source = Path(tempfile.gettempdir()).resolve() / "Main.cpp"
        session.client = MagicMock()
        session.client.notifications = [
            {"method": "textDocument/publishDiagnostics", "params": {
                "uri": session.source.as_uri(), "version": 1, "diagnostics": []}},
            {"method": "textDocument/publishDiagnostics", "params": {
                "uri": "file:///Other.cpp", "version": 2, "diagnostics": []}}]

        self.assertEqual(session.diagnostics(1), [])
        self.assertIsNone(session.diagnostics(2))

    def test_shutdown_reaps_server_when_exit_wait_expires(self):
        client = Client.__new__(Client)
        client.process = MagicMock()
        client.process.poll.side_effect = [None, 0]
        client.process.wait.side_effect = [subprocess.TimeoutExpired("server", 3), None]
        client.request = MagicMock()
        client.notify = MagicMock()
        client.stop = MagicMock()
        client.reader = MagicMock()
        client.monitor = MagicMock()
        client.stderr = MagicMock()

        client.close()

        client.process.kill.assert_called_once()
        self.assertEqual(client.process.wait.call_count, 2)
        client.stderr.close.assert_called_once()

    def test_unobserved_diagnostics_are_not_reported_as_clean(self):
        self.assertEqual(document_diagnostics([], "file:///Main.cpp")["status"], "not_observed")

    def test_diagnostics_select_latest_publication_for_requested_document(self):
        notifications = [{"method": "textDocument/publishDiagnostics", "params": {
            "uri": uri, "version": version, "diagnostics": diagnostics}}
            for uri, version, diagnostics in [("file:///Main.cpp", 1, [{"message": "old"}]),
                                               ("file:///Other.cpp", 2, []),
                                               ("file:///Main.cpp", 3, [])]]

        result = document_diagnostics(notifications, "file:///Main.cpp")

        self.assertEqual(result, {"status": "observed", "version": 3, "diagnostics": []})

    def test_reflection_inventory_does_not_claim_semantic_evaluation(self):
        inventory = reflection_inventory("auto x = ^^Entity; using T = [:x:]; std::meta::info i;")

        self.assertEqual(inventory["reflection_operators"], 1)
        self.assertEqual(inventory["splice_openers"], 1)
        self.assertEqual(inventory["meta_mentions"], 1)
        self.assertIn("not semantic", inventory["limitation"])

    def test_definition_rejects_unselected_overload(self):
        def location(line):
            return {"uri": "file:///Main.cpp", "range": {
                "start": {"line": line, "character": 4},
                "end": {"line": line, "character": 10}}}

        expected = {"kind": "definition", "file": "Main.cpp", "line": 1}

        self.assertTrue(assess_result([location(1)], expected))
        self.assertFalse(assess_result([location(1), location(2)], expected))
        self.assertFalse(assess_result([], expected))

    def test_location_links_use_target_selection_range(self):
        selection = {"start": {"line": 1, "character": 4},
                     "end": {"line": 1, "character": 10}}

        self.assertEqual(locations([{"targetUri": "file:///Main.cpp",
                                     "targetSelectionRange": selection}]),
                         [{"uri": "file:///Main.cpp", "range": selection}])

    def test_shadowed_references_reject_extra_occurrences(self):
        entries = [{"uri": "file:///Main.cpp", "range": {
            "start": {"line": line, "character": 0}}} for line in [6, 10]]
        expected = {"kind": "references", "lines": [6, 10]}

        self.assertTrue(assess_result(entries, expected))
        self.assertFalse(assess_result(entries + [entries[0]], expected))

    def test_template_hover_requires_substituted_type(self):
        expected = {"kind": "hover", "contains": "Widget"}

        self.assertFalse(assess_result({"contents": "payload: T"}, expected))
        self.assertTrue(assess_result({"contents": "payload: Widget"}, expected))

    def test_quality_corpus_and_responses_are_reproducible(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            source, text, cases = quality_workspace(root, "c++")

            self.assertEqual(source.read_text(), text)
            self.assertEqual(len(cases), 7)
            self.assertEqual(normalize_result({"uri": source.as_uri()}, root),
                             {"uri": "workspace:///Main.cpp"})
            for case in cases:
                position = case["params"]["position"]
                self.assertLessEqual(position["character"],
                                     len(text.splitlines()[position["line"]]))

    def test_percentile_uses_nearest_rank(self):
        self.assertEqual(percentile([30, 10, 20], 0.95), 30)
        self.assertEqual(percentile([10], 0.95), 10)
        self.assertEqual(percentile(list(range(1, 21)), 0.95), 19)

    def test_completion_accepts_both_protocol_shapes(self):
        items = [{"label": "Value"}, {"label": "Other"}]
        self.assertEqual(response_count("textDocument/completion", items), 2)
        self.assertEqual(response_count("textDocument/completion", {"items": items}), 2)
        self.assertEqual(response_count("textDocument/completion", None), 0)

    def test_null_hover_is_not_a_successful_result(self):
        self.assertEqual(response_count("textDocument/hover", None), 0)
        self.assertEqual(response_count("textDocument/hover", {"contents": "Widget"}), 1)

    def test_semantic_tokens_count_five_integer_records(self):
        self.assertEqual(response_count("textDocument/semanticTokens/full", {"data": [0] * 15}), 3)

    def test_unresponsive_server_times_out_and_is_reaped(self):
        with tempfile.TemporaryDirectory(prefix="lsp-comparison-timeout-") as directory:
            client = Client([sys.executable, "-c", "import time; time.sleep(30)"],
                            directory, timeout=0.05, memory_limit=1024)
            try:
                with self.assertRaises(TimeoutError):
                    client.request("initialize", {})
            finally:
                client.process.kill()
                client.process.wait()
                client.close()

            self.assertIsNotNone(client.process.returncode)

    def test_corpus_includes_dependency_and_valid_completion_position(self):
        with tempfile.TemporaryDirectory(prefix="lsp-comparison-test-") as directory:
            root = Path(directory).resolve()
            source, text, position = create_workspace(root, 10, "c++")

            self.assertEqual(source.read_text(), text)
            self.assertEqual(text.splitlines()[position["line"]][position["character"] - 2:
                             position["character"] + 3], "Value")
            self.assertTrue((root / "build/_deps/widget-src/include/Widget.hpp").is_file())
            self.assertTrue((root / "compile_commands.json").is_file())


if __name__ == "__main__":
    unittest.main()
