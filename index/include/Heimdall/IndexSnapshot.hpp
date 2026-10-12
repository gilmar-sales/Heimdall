#pragma once

#include <Heimdall/FileShard.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
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

	/**
	 * What each occurrence of one shard refers to once names are looked up through the
	 * include closure. `state` is Resolved (one entity), Ambiguous (several fit),
	 * Unresolved (nothing visible declares it) or Dependent.
	 */
	struct ShardResolution
	{
		std::vector<std::uint32_t> begin; // CSR over `targets`, one row per occurrence
		std::vector<SymbolRef> targets;
		std::vector<Resolution> state;

		[[nodiscard]] std::span<const SymbolRef> Targets(std::uint32_t occurrence) const noexcept
		{
			if (occurrence + 1 >= begin.size())
			{
				return {};
			}

			return {targets.data() + begin[occurrence], begin[occurrence + 1] - begin[occurrence]};
		}
	};

	/** Immutable view over a set of shards; queries are thread-safe. */
	class IndexSnapshot
	{
	public:
		[[nodiscard]] static std::shared_ptr<const IndexSnapshot> Build(
			std::vector<std::shared_ptr<const FileShard>> shards, std::uint64_t revision = 0);

		/** Store generation pinned by this snapshot; standalone snapshots default to zero. */
		[[nodiscard]] std::uint64_t Revision() const noexcept
		{
			return mRevision;
		}

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

		/** Scoped lookup of every occurrence of `shard`; computed once, on first use. */
		[[nodiscard]] const ShardResolution& Resolved(std::uint32_t shard) const;

		/** Shards reachable from `shard` through `#include`, including itself. */
		[[nodiscard]] std::vector<bool> Visible(std::uint32_t shard) const;

		[[nodiscard]] SymbolHash HashOf(SymbolRef symbol) const noexcept;

		/** Every declaration and definition carrying the identity `hash`. */
		[[nodiscard]] std::vector<SymbolRef> Declarations(SymbolHash hash) const;

		/** Exact qualified-name directory lookup, without fuzzy ranking or result truncation. */
		[[nodiscard]] std::vector<SymbolRef> Named(std::string_view qualified) const;

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
		std::uint64_t mRevision = 0;
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
		// Forward include graph in CSR form: shards that shard i directly includes.
		std::vector<std::uint32_t> mIncludesBegin;
		std::vector<std::uint32_t> mIncluded;
		// Symbols by hash of their qualified name, and members by hash of their name.
		std::vector<std::uint64_t> mQualHash;
		std::vector<std::uint32_t> mQualShard;
		std::vector<std::uint32_t> mQualSymbol;
		std::vector<std::uint64_t> mMemberHash;
		std::vector<std::uint32_t> mMemberShard;
		std::vector<std::uint32_t> mMemberSymbol;
		// Lazy per-shard resolution.
		mutable std::vector<ShardResolution> mResolution;
		mutable std::unique_ptr<std::once_flag[] > mResolveOnce;

		void ResolveShard(std::uint32_t shard, ShardResolution& out) const;
	};
} // namespace heimdall::index
