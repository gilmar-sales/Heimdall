#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <Heimdall/Lexer.hpp>

namespace heimdall
{
	enum class SourceEditError
	{
		InvalidRange,
		SizeLimit
	};

	/** Immutable text pieces with a shared, demand-driven contiguous compatibility buffer. */
	class SourceSnapshot
	{
	public:
		/** Copies input into bounded chunks so deleted text can be reclaimed independently. */
		[[nodiscard]] static std::expected<SourceSnapshot, SourceEditError> Create(
			std::string_view source);

		/** Shares untouched pieces, compacting small fragments when metadata exceeds its budget. */
		[[nodiscard]] std::expected<SourceSnapshot, SourceEditError> Edit(
			std::size_t offset,
			std::size_t oldLength,
			std::string_view replacement) const;

		/** Number of bytes in this version. */
		[[nodiscard]] std::size_t Size() const noexcept;

		/** Reads a bounded range without materializing the document. */
		[[nodiscard]] std::string Read(std::size_t offset, std::size_t length) const;

		/** Creates the contiguous buffer at most once, retaining it for this version. */
		[[nodiscard]] std::shared_ptr<const std::string> Materialize() const;

		/** Number of immutable pieces in the version. */
		[[nodiscard]] std::size_t ChunkCount() const noexcept;

		/** Borrows a piece; this snapshot must outlive the returned view. */
		[[nodiscard]] std::string_view Chunk(std::size_t index) const noexcept;

		/** Absolute byte offset of a piece. */
		[[nodiscard]] std::size_t ChunkOffset(std::size_t index) const noexcept;

		/** Bytes copied to create this version, excluding compatibility materialization. */
		[[nodiscard]] std::size_t CopiedBytes() const noexcept;

		/** Byte offset immediately after the preceding newline, or zero. */
		[[nodiscard]] std::size_t LineStart(std::size_t offset) const noexcept;

		/** Byte offset of the next newline, or the end of the document. */
		[[nodiscard]] std::size_t LineEnd(std::size_t offset) const noexcept;

		/** Live piece and compatibility capacities, counting shared chunks once. */
		[[nodiscard]] std::size_t StorageBytes() const;

		/** Adds capacities whose identities have not been counted by another snapshot. */
		[[nodiscard]] std::size_t StorageBytes(std::unordered_set<const void*>& counted) const;

	private:
		static constexpr std::size_t kChunkBytes = 4096;
		struct Piece
		{
			std::shared_ptr<const std::string> text;
			std::size_t offset;
			std::size_t length;
		};

		struct Storage
		{
			std::vector<Piece> pieces;
			std::vector<std::size_t> ends;
			std::size_t copiedBytes = 0;
			mutable std::mutex mutex;
			mutable std::shared_ptr<const std::string> contiguous;
		};

		explicit SourceSnapshot(std::shared_ptr<Storage> storage) : mStorage(std::move(storage)) {}

		void AppendRange(Storage& destination, std::size_t offset, std::size_t length) const;

		static void Append(Storage& storage, Piece piece);

		static void Compact(Storage& storage);

		std::shared_ptr<Storage> mStorage;
	};
} // namespace heimdall
