#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <Heimdall/SyntaxHighlight.hpp>

#include "Document.hpp"

namespace heimdall::lsp
{

	/// Byte range of a `textDocument/semanticTokens/range` request.
	struct ByteRange
	{
		std::size_t begin = 0;
		std::size_t end   = 0;
	};

	/// `SemanticTokensLegend` JSON object advertised in the server capabilities.
	[[nodiscard]] std::string SemanticTokensLegend();

	/// `SemanticTokens` result: relative-encoded `data` in UTF-16 columns. A span that crosses
	/// lines is split per line, and `range` keeps only the spans that overlap it.
	void AppendSemanticTokens(
		std::span<const heimdall::HighlightSpan> spans,
		std::string_view text,
		const LineIndex& lines,
		std::optional<ByteRange> range,
		std::string& out);

} // namespace heimdall::lsp
