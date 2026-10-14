#include "SemanticTokens.hpp"

#include <charconv>
#include <cstdint>
#include <iterator>

namespace heimdall::lsp
{

	namespace
	{

		constexpr std::string_view kModifierNames = "\"declaration\",\"inactive\"";

		constexpr unsigned kContinuationMask = 0xC0;
		constexpr unsigned kContinuationBits = 0x80;
		constexpr unsigned kFourByteLead     = 0xF0;
		constexpr std::size_t kSurrogatePair = 2;
		constexpr std::size_t kMaxDigits     = 24;
		constexpr std::size_t kCharsPerToken = 16;

		// UTF-16 code units of UTF-8 text: continuation bytes add none, a four-byte sequence two.
		std::size_t Utf16Units(std::string_view text) noexcept
		{
			std::size_t units = 0;
			for (const char c : text)
			{
				const auto byte = static_cast<unsigned char>(c);
				units +=(byte& kContinuationMask) == kContinuationBits ? 0 : (byte >= kFourByteLead ? kSurrogatePair : 1);
			}

			return units;
		}

		void AppendNumber(std::string& out, std::size_t value)
		{
			char digits[kMaxDigits];
			const auto[end, error] = std::to_chars(std::begin(digits), std::end(digits), value);
			out.append(digits, end);
		}

		struct Encoder
		{
			const LineIndex& lines;
			std::string_view text;
			std::string& out;
			std::size_t previousLine      = 0;
			std::size_t previousCharacter = 0;
			bool first                    = true;

			void Piece(std::size_t begin, std::size_t end, const heimdall::HighlightSpan& span)
			{
				const std::size_t length = Utf16Units(text.substr(begin, end - begin));
				if (length == 0)
				{
					return;
				}

				const Position start        = lines.ToPosition(begin);
				const std::size_t deltaLine = start.line - previousLine;
				const std::size_t deltaChar =
					deltaLine == 0 ? start.character - previousCharacter : start.character;
				if (!first)
				{
					out += ',';
				}

				first = false;
				AppendNumber(out, deltaLine);
				out += ',';
				AppendNumber(out, deltaChar);
				out += ',';
				AppendNumber(out, length);
				out += ',';
				AppendNumber(out, static_cast<std::size_t>(span.kind));
				out += ',';
				AppendNumber(out, span.modifiers);
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
		out.reserve(out.size() + spans.size() * kCharsPerToken);
		Encoder encoder {lines, text, out};
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
