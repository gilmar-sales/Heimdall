#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <stop_token>
#include <unordered_set>
#include <vector>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/SourceSnapshot.hpp>

namespace heimdall
{
	/** Persistent token slices with offset shifts stored once per slice. */
	class TokenSnapshot
	{
	public:
		/** Splits a lossless token buffer into bounded immutable blocks. */
		[[nodiscard]] static TokenSnapshot Create(std::vector<Token> tokens);

		/** Relexes a bounded window, sharing untouched prefix and suffix blocks. */
		[[nodiscard]] TokenSnapshot Relex(
			const SourceSnapshot& source,
			Lexer::TextEdit edit,
			std::stop_token stop = {}) const;

		/** Token count in this version. */
		[[nodiscard]] std::size_t Size() const noexcept;

		/** Reads a token with its version's absolute byte offset. */
		[[nodiscard]] Token At(std::size_t index) const noexcept;

		/** First token beginning at or after a byte offset. */
		[[nodiscard]] std::size_t LowerBound(std::size_t offset) const noexcept;

		/** Builds the contiguous compatibility buffer once per version. */
		[[nodiscard]] std::shared_ptr<const std::vector<Token>> Materialize() const;

        /** Tokens copied by relexing and fragment compaction, excluding the compatibility buffer.
         */
		[[nodiscard]] std::size_t CopiedTokens() const noexcept;

		/** Conservatively recognizes edits that cannot change preprocessing directives. */
		[[nodiscard]] bool PreservesDirectives(
			const SourceSnapshot& before,
			Lexer::TextEdit edit,
			std::string_view replacement) const;

		/** Live token block and compatibility capacities, counting shared blocks once. */
		[[nodiscard]] std::size_t StorageBytes() const;

		/** Adds capacities whose identities have not been counted by another snapshot. */
		[[nodiscard]] std::size_t StorageBytes(std::unordered_set<const void*>& counted) const;

	private:
		static constexpr std::size_t kBlockTokens              = 512;
		static constexpr std::size_t kInitialWindowBytes       = 256;
		static constexpr std::size_t kDirectiveInspectionBytes = 4096;
		struct Slice
		{
			std::shared_ptr<const std::vector<Token>> tokens;
			std::size_t begin;
			std::size_t end;
			std::ptrdiff_t shift;
		};

		struct Storage
		{
			std::vector<Slice> slices;
			std::vector<std::size_t> ends;
			std::size_t copiedTokens = 0;
			mutable std::mutex mutex;
			mutable std::shared_ptr<const std::vector<Token>> contiguous;
		};

		explicit TokenSnapshot(std::shared_ptr<Storage> storage) : mStorage(std::move(storage)) {}

		static void Append(Storage& storage, Slice slice);

		static void Compact(Storage& storage);

		static void AppendTokens(Storage& storage, const std::vector<Token>& tokens);

		void AppendRange(
			Storage& destination,
			std::size_t begin,
			std::size_t end,
			std::ptrdiff_t shift) const;

		std::shared_ptr<Storage> mStorage;
	};
} // namespace heimdall
