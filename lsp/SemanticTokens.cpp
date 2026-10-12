#include "SemanticTokens.hpp"

#include <cstdint>

namespace heimdall::lsp
{

	namespace
	{

		constexpr std::string_view kModifierNames = "\"declaration\"";

		struct Encoder
		{
			const LineIndex& lines;
			std::string& out;
			std::size_t previousLine      = 0;
			std::size_t previousCharacter = 0;
			bool first                    = true;

			void Piece(std::size_t begin, std::size_t end, const heimdall::HighlightSpan& span)
			{
				const Position start = lines.ToPosition(begin);
				const Position stop  = lines.ToPosition(end);
				if (stop.character <= start.character)
				{
					return;
				}

				const std::size_t deltaLine = start.line - previousLine;
				const std::size_t deltaChar =
					deltaLine == 0 ? start.character - previousCharacter : start.character;
				if (!first)
				{
					out += ',';
				}

				first = false;
				out += std::to_string(deltaLine);
				out += ',';
				out += std::to_string(deltaChar);
				out += ',';
				out += std::to_string(stop.character - start.character);
				out += ',';
				out += std::to_string(static_cast<unsigned>(span.kind));
				out += ',';
				out += std::to_string(static_cast<unsigned>(span.modifiers));
				previousLine      = start.line;
				previousCharacter = start.character;
			}
		};

	} // namespace

	std::string SemanticTokensLegend()
	{
		std::string legend = "{\"tokenTypes\":[";
		for (std::size_t i = 0; i < static_cast<std::size_t>(heimdall::HighlightKind::Count); ++i)
		{
			if (i != 0)
			{
				legend += ',';
			}

			legend += '"';
			legend += heimdall::HighlightKindName(static_cast<heimdall::HighlightKind>(i));
			legend += '"';
		}

		legend += "],\"tokenModifiers\":[";
		legend += kModifierNames;
		legend += "]}";
		return legend;
	}

	void AppendSemanticTokens(
		std::span<const heimdall::HighlightSpan> spans,
		std::string_view text,
		const LineIndex& lines,
		std::optional<ByteRange> range,
		std::string& out)
	{
		out += "{\"data\":[";
		Encoder encoder {lines, out};
		for (const heimdall::HighlightSpan& span : spans)
		{
			const std::size_t end = span.offset + span.length;
			if (end > text.size() ||(range && (end <= range->begin || span.offset >= range->end)))
			{
				continue;
			}

			std::size_t begin = span.offset;
			while (begin < end)
			{
				std::size_t stop = text.find('\n', begin);
				if (stop == std::string_view::npos || stop > end)
				{
					stop = end;
				}

				std::size_t pieceEnd = stop;
				if (pieceEnd > begin && text[pieceEnd - 1] == '\r')
				{
					--pieceEnd;
				}

				encoder.Piece(begin, pieceEnd, span);
				begin = stop + 1;
			}
		}

		out += "]}";
	}

} // namespace heimdall::lsp
