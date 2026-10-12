#pragma once

#include <Heimdall/FileShard.hpp>

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace heimdall::index
{
	/** A symbol inside a specific shard of a snapshot. */
	struct SymbolRef
	{
		std::uint32_t shard  = kInvalid;
		std::uint32_t symbol = kInvalid;

		[[nodiscard]] bool Valid() const noexcept
		{
			return shard != kInvalid;
		}
	};

	/** One use of a symbol. `Unresolved` means matched by written name and include visibility. */
	struct Location
	{
		std::uint32_t shard   = kInvalid;
		std::uint32_t offset  = 0;
		std::uint32_t length  = 0;
		Role role             = Role::Reference;
		Resolution resolution = Resolution::Resolved;
	};

	/** A class related to the query class; `exact` is false when linked through a written name. */
	struct RelatedClass
	{
		SymbolRef symbol;
		bool exact = true;
	};

	/** Immutable view over a set of shards; every query is lock-free. */
	class IndexSnapshot
	{
	public:
		[[nodiscard]] static std::shared_ptr<const IndexSnapshot> Build(
			std::vector<std::shared_ptr<const FileShard>> shards);

		[[nodiscard]] std::span<const std::shared_ptr<const FileShard>> Shards() const noexcept
		{
			return mShards;
		}

		/** Shard index for `path`, or kInvalid. */
		[[nodiscard]] std::uint32_t ShardFor(std::string_view path) const noexcept;

		/** Symbol declared or referenced (resolved) at a byte offset of a shard. */
		[[nodiscard]] SymbolRef SymbolAt(std::uint32_t shard, std::uint32_t offset) const noexcept;

		/**
		 * Declarations a name written at `offset` may refer to, when the shard could not
		 * resolve it locally: same spelling, declared in a file this shard includes.
		 */
		[[nodiscard]] std::vector<SymbolRef> Candidates(
			std::uint32_t shard, std::uint32_t offset) const;

		[[nodiscard]] SymbolHash HashOf(SymbolRef symbol) const noexcept;

		/** Every declaration and definition carrying the identity `hash`. */
		[[nodiscard]] std::vector<SymbolRef> Declarations(SymbolHash hash) const;

		/** Only definitions; empty when the entity is declared but never defined. */
		[[nodiscard]] std::vector<SymbolRef> Definitions(SymbolHash hash) const;

		/** Declarations, then resolved uses, then name-matched uses in files that see them. */
		[[nodiscard]] std::vector<Location> References(SymbolHash hash) const;

		/** Direct derived classes across all shards. */
		[[nodiscard]] std::vector<RelatedClass> Subtypes(SymbolHash hash) const;

		/** Direct bases of `derived`; a base known only by name yields every class so named. */
		[[nodiscard]] std::vector<RelatedClass> Supertypes(SymbolHash derived) const;

		/** Fuzzy name search, best matches first. */
		[[nodiscard]] std::vector<SymbolRef> Search(
			std::string_view query, std::size_t limit) const;

		[[nodiscard]] std::size_t StorageBytes() const noexcept;

	private:
		std::vector<std::shared_ptr<const FileShard>> mShards;
		// Directory sorted by hash: which shard declares which symbol.
		std::vector<SymbolHash> mDirHash;
		std::vector<std::uint32_t> mDirShard;
		std::vector<std::uint32_t> mDirSymbol;
		// Name-matched uses sorted by name hash.
		std::vector<std::uint64_t> mUnresolvedName;
		std::vector<std::uint32_t> mUnresolvedShard;
		std::vector<std::uint32_t> mUnresolvedOccurrence;
		// Reverse include graph in CSR form: shards that directly include shard i.
		std::vector<std::uint32_t> mIncludersBegin;
		std::vector<std::uint32_t> mIncluders;
		// Shard indices sorted by path.
		std::vector<std::uint32_t> mByPath;
	};
} // namespace heimdall::index
