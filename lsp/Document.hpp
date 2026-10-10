#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <Heimdall/SourceSnapshot.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall::lsp
{

	struct Position
	{
		std::size_t line      = 0;
		std::size_t character = 0;
	};

	struct Document
	{
		std::string text;
		std::int64_t version = 0;
	};

	// Per-snapshot line index: byte offset <-> LSP (line, UTF-16 character).
	// Built once per document version; lookups are O(log L + line width) instead
	// of the previous O(file size) scan per diagnostic/position.
	class LineIndex
	{
	public:
		void Build(std::string_view text);

        /** Builds positions directly over immutable pieces without flattening text. */
        void Build(std::shared_ptr<const heimdall::SourceSnapshot> source);

        // Points the index at a different buffer holding the same text (for
        // example after the string was moved into shared storage).
        void Rebind(std::string_view text) noexcept
		{
            mSource.reset();
            m_text = text;
        }

		// Incremental update after `old_length` bytes at `offset` were replaced
		// by `new_length` bytes; `text` is the buffer after the edit. Only the
		// line starts inside the edit are rescanned, the rest are shifted.
		void Update(
			std::string_view text,
			std::size_t offset,
			std::size_t old_length,
			std::size_t new_length);

        /** Updates the line table using only replacement text from the new snapshot. */
        void Update(std::shared_ptr<const heimdall::SourceSnapshot> source, std::size_t offset,
                    std::size_t oldLength, std::size_t newLength);

        Position ToPosition(std::size_t offset) const;

        std::size_t OffsetFromPosition(Position position) const;

		std::size_t LineCount() const noexcept
		{
			return m_line_starts.size();
		}

	private:
      void UpdateLines(std::size_t offset, std::size_t oldLength, std::string_view inserted);
      static std::size_t Utf16Width(std::string_view text, std::size_t i,
                                    std::size_t stop) noexcept;

      std::string_view                                m_text;
      std::shared_ptr<const heimdall::SourceSnapshot> mSource;
      std::vector<std::uint32_t>                      m_line_starts;
    };

	Position ToPosition(std::string_view text, std::size_t offset);

	std::size_t OffsetFromPosition(std::string_view text, Position position);

	std::filesystem::path PathFromUri(std::string_view uri);

	std::string UriFromPath(const std::filesystem::path& path);

	void AppendPosition(Position position, std::string& out);

} // namespace heimdall::lsp
