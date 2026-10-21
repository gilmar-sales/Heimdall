import sys
import unittest
from unittest.mock import MagicMock, patch
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from TokenComparison import (TOKEN_TYPES, TOKEN_MODIFIERS, collect, compare_tokens,
                             decode_tokens, diagnostics_for, wait_ready)


class TokenComparisonSpec(unittest.TestCase):
    def test_readiness_ignores_wrong_documents_and_stale_versions(self):
        notifications = [{"method": "textDocument/publishDiagnostics", "params": {
            "uri": uri, "version": version, "diagnostics": []}}
            for uri, version in [("file:///other.cpp", 1), ("file:///main.cpp", 0)]]

        self.assertEqual(diagnostics_for(notifications, "file:///main.cpp", 1)["status"],
                         "unknown")
        notifications.append({"method": "textDocument/publishDiagnostics", "params": {
            "uri": "file:///main.cpp", "version": 1, "diagnostics": [{"message": "error"}]}})
        self.assertEqual(len(diagnostics_for(notifications, "file:///main.cpp", 1)
                             ["diagnostics"]), 1)

    def test_unversioned_diagnostics_are_explicitly_unknown(self):
        notifications = [{"method": "textDocument/publishDiagnostics", "params": {
            "uri": "file:///main.cpp", "diagnostics": []}}]

        self.assertEqual(diagnostics_for(notifications, "file:///main.cpp", 1),
                         {"status": "unknown", "version": 1, "diagnostics": None})

    def test_readiness_requires_complete_completion_and_current_diagnostics(self):
        client = MagicMock(timeout=30)
        client.notifications = [{"method": "textDocument/publishDiagnostics", "params": {
            "uri": "file:///main.cpp", "version": 1, "diagnostics": []}}]
        client.request.side_effect = [(0, {"isIncomplete": True}, 0), (0, [], 0)]

        with patch("TokenComparison.time.sleep"):
            ready = wait_ready(client, "file:///main.cpp", 1, 1)

        self.assertEqual(ready["status"], "ready")
        self.assertEqual(client.request.call_count, 2)
        self.assertEqual(client.timeout, 30)

    def test_readiness_times_out_without_claiming_clean_document(self):
        client = MagicMock(timeout=30, notifications=[])
        client.request.return_value = (0, [], 0)

        with patch("TokenComparison.time.monotonic", side_effect=[0, 0.1, 2, 2]), \
             patch("TokenComparison.time.sleep"), self.assertRaises(TimeoutError):
            wait_ready(client, "file:///main.cpp", 1, 1)
        self.assertEqual(client.timeout, 30)

    def test_server_failure_restores_timeout(self):
        client = MagicMock(timeout=30)
        client.request.side_effect = RuntimeError("closed stdout")

        with self.assertRaises(RuntimeError):
            wait_ready(client, "file:///main.cpp", 1, 1)
        self.assertEqual(client.timeout, 30)

    def test_clangd_barrier_does_not_depend_on_completion_truncation(self):
        client = MagicMock(timeout=30)
        client.notifications = [{"method": "textDocument/publishDiagnostics", "params": {
            "uri": "file:///main.cpp", "version": 1, "diagnostics": []}}]

        ready = wait_ready(client, "file:///main.cpp", 1, 1, completion_barrier=False)

        self.assertEqual(ready["status"], "ready")
        client.request.assert_not_called()

    def test_initialization_failure_closes_server(self):
        client = MagicMock()
        client.request.side_effect = RuntimeError("server closed stdout")

        with patch("TokenComparison.Client", return_value=client), \
             patch("TokenComparison.executable_metadata", return_value={}), \
             self.assertRaises(RuntimeError):
            collect(["server"], Path.cwd(), Path("compile_commands.json"), [], 1)

        client.close.assert_called_once()

    def test_missing_data_is_not_an_empty_success(self):
        for result in (None, {}, {"edits": []}, {"data": "bad"}):
            with self.subTest(result=result), self.assertRaises(ValueError):
                decode_tokens(result, {"tokenTypes": [], "tokenModifiers": []}, "")

    def test_success_records_legend_and_diagnostics_and_is_repeatable(self):
        clients = [MagicMock(), MagicMock()]
        for client in clients:
            client.request.side_effect = [(0, {"serverInfo": {"name": "clangd"},
                "capabilities": {"semanticTokensProvider": {"full": {}, "legend": {
                    "tokenTypes": ["variable"], "tokenModifiers": []}}}}, 0),
                (0, {"data": [0, 0, 1, 0, 0]}, 0)]
        source = Path.cwd() / "main.cpp"
        ready = {"status": "ready", "diagnostics": {"status": "observed", "diagnostics": []}}

        with patch("TokenComparison.Client", side_effect=clients), \
             patch("TokenComparison.executable_metadata", return_value={"sha256": "hash"}), \
             patch("TokenComparison.wait_ready", return_value=ready):
            results = [collect(["server"], Path.cwd(), Path("compile_commands.json"),
                               [(source, "x")], 1) for _ in range(2)]

        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0]["metadata"]["legend"]["tokenTypes"], ["variable"])
        self.assertEqual(results[0]["documents"][str(source)], ready)
        for client in clients:
            client.close.assert_called_once()

    def test_negotiates_real_capabilities_and_closes_unsupported_server(self):
        client = MagicMock()
        client.request.return_value = (0, {"capabilities": {}}, 0)

        with patch("TokenComparison.Client", return_value=client), \
             patch("TokenComparison.executable_metadata", return_value={}), \
             self.assertRaises(ValueError):
            collect(["server"], Path.cwd(), Path("compile_commands.json"), [], 1)

        params = client.request.call_args.args[1]
        supported = params["capabilities"]["textDocument"]["semanticTokens"]
        self.assertEqual(supported["tokenTypes"], TOKEN_TYPES)
        self.assertEqual(supported["tokenModifiers"], TOKEN_MODIFIERS)
        client.close.assert_called_once()

    def test_metrics_distinguish_type_and_modifier_changes(self):
        legend = {"tokenTypes": ["variable", "parameter"], "tokenModifiers": ["readonly"]}
        left = decode_tokens({"data": [0, 0, 1, 0, 0, 0, 2, 1, 0, 0]}, legend, "a b")
        right = decode_tokens({"data": [0, 0, 1, 0, 1, 0, 2, 1, 1, 0]}, legend, "a b")

        metrics = compare_tokens(left, right)["metrics"]

        self.assertEqual(metrics["modifiers_only"], 1)
        self.assertEqual(metrics["type_changes"], 1)
        self.assertEqual(metrics["common_range_type_agreement"], 0.5)
        self.assertIsNone(compare_tokens([], [])["metrics"]["reference_range_coverage"])

    def test_legends_are_compared_by_name(self):
        left = decode_tokens({"data": [0, 0, 3, 0, 1]},
                             {"tokenTypes": ["variable", "function"],
                              "tokenModifiers": ["readonly", "static"]}, "foo")
        right = decode_tokens({"data": [0, 0, 3, 1, 2]},
                              {"tokenTypes": ["function", "variable"],
                               "tokenModifiers": ["static", "readonly"]}, "foo")

        self.assertEqual(compare_tokens(left, right)["matched"], 1)

    def test_relative_positions_and_utf16_spelling(self):
        tokens = decode_tokens({"data": [0, 2, 3, 0, 0, 1, 0, 1, 0, 0]},
                               {"tokenTypes": ["variable"], "tokenModifiers": []},
                               "😀foo\r\nx")

        self.assertEqual([token["text"] for token in tokens], ["foo", "x"])
        self.assertEqual(tokens[1]["character"], 0)

    def test_reports_missing_extra_and_classification(self):
        legend = {"tokenTypes": ["variable", "parameter"], "tokenModifiers": []}
        left = decode_tokens({"data": [0, 0, 1, 0, 0, 0, 2, 1, 0, 0]}, legend, "a b c")
        right = decode_tokens({"data": [0, 0, 1, 1, 0, 0, 4, 1, 0, 0]}, legend, "a b c")

        differences = compare_tokens(left, right)["differences"]

        self.assertEqual([item["status"] for item in differences],
                         ["classification", "extra", "missing"])

    def test_invalid_streams_are_rejected(self):
        legend = {"tokenTypes": ["variable"], "tokenModifiers": []}
        for data in ([0], [0, 0, 1, 1, 0], [0, 0, 1, 0, 1], [0, 0, 2, 0, 0],
                     [1, 0, 1, 0, 0], [0, -1, 1, 0, 0], [0, 0, 0, 0, 0]):
            with self.subTest(data=data), self.assertRaises(ValueError):
                decode_tokens({"data": data}, legend, "x")

    def test_duplicate_ranges_are_rejected(self):
        token = {"line": 0, "character": 0, "length": 1}

        with self.assertRaises(ValueError):
            compare_tokens([token, token], [])


if __name__ == "__main__":
    unittest.main()
