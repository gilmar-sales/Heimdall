import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))
from LspComparison import Client, create_workspace, percentile, response_count


class LspComparisonSpec(unittest.TestCase):
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
