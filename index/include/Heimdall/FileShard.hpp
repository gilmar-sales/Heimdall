#pragma once

#include <Heimdall/SemanticModel.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall::index
{
	/** Stable 64-bit identity of a declaration; equal across files for the same entity. */
	using SymbolHash = std::uint64_t;
	using StringId   = std::uint32_t;

	inline constexpr std::uint32_t kInvalid = ~0u;

	enum class Role : std::uint8_t
	{
		Declaration,
		Definition,
		Reference
	};

	enum class Resolution : std::uint8_t
	{
		Resolved,   // bound to a symbol declared in this shard
		Unresolved, // written name only; may match declarations in other files
		Dependent
	};

	enum class RelationKind : std::uint8_t
	{
		BaseOf,       // subject = base symbol hash, object = derived symbol hash
		BaseOfByName, // subject = hash of the written base name, object = derived hash
	};

	/** Interned text of one shard; ids index a flat offset table. */
	class StringTable
	{
	public:
		[[nodiscard]] StringId Intern(std::string_view text);

		[[nodiscard]] std::string_view Text(StringId id) const noexcept;

		[[nodiscard]] std::size_t Size() const noexcept
		{
			return mBegin.size();
		}

		[[nodiscard]] std::size_t Bytes() const noexcept;

	private:
		std::string mBlob;
		std::vector<std::uint32_t> mBegin;
		std::vector<std::uint32_t> mLength;
	};

	/** Declarations of one file revision, one column per property. */
	struct SymbolColumns
	{
		std::vector<SymbolHash> hash;
		std::vector<std::uint64_t> nameMask; // characters present in the name, see CharacterMask
		std::vector<StringId> name;
		std::vector<StringId> qualified;
		std::vector<std::uint32_t> container; // local index of the owning class/namespace
		std::vector<std::uint32_t> flags;     // heimdall::SymbolFlag bits
		std::vector<std::uint32_t> nameOffset;
		std::vector<std::uint32_t> nameLength;
		std::vector<heimdall::SymbolKind> kind;
		std::vector<Role> role;

		[[nodiscard]] std::size_t Size() const noexcept
		{
			return hash.size();
		}
	};

	/** Identifier uses sorted by offset; `symbol` is a local symbol index or kInvalid. */
	struct OccurrenceColumns
	{
		std::vector<std::uint32_t> offset;
		std::vector<std::uint32_t> length;
		std::vector<std::uint32_t> symbol;
		std::vector<StringId> name; // written spelling; always set
		std::vector<Role> role;
		std::vector<Resolution> resolution;

		[[nodiscard]] std::size_t Size() const noexcept
		{
			return offset.size();
		}
	};

	/** Inheritance edges sorted by subject. */
	struct RelationColumns
	{
		std::vector<SymbolHash> subject;
		std::vector<SymbolHash> object;
		std::vector<RelationKind> kind;

		[[nodiscard]] std::size_t Size() const noexcept
		{
			return subject.size();
		}
	};

	/** One bit per character class; a query mask must be a subset of the name mask. */
	[[nodiscard]] std::uint64_t CharacterMask(std::string_view text) noexcept;

	/** FNV-1a over bytes; also used for content and identity hashing. */
	[[nodiscard]] std::uint64_t HashBytes(
		std::string_view bytes, std::uint64_t seed = 0xcbf29ce484222325ull) noexcept;

	/**
	 * Immutable index of one file revision. Built once by ShardBuilder and shared
	 * between snapshots through shared_ptr<const FileShard>.
	 */
	class FileShard
	{
	public:
		[[nodiscard]] const std::string& Path() const noexcept
		{
			return mPath;
		}

		[[nodiscard]] std::uint64_t ContentHash() const noexcept
		{
			return mContentHash;
		}

		[[nodiscard]] std::uint64_t ConfigHash() const noexcept
		{
			return mConfigHash;
		}

		[[nodiscard]] const StringTable& Strings() const noexcept
		{
			return mStrings;
		}

		[[nodiscard]] const SymbolColumns& Symbols() const noexcept
		{
			return mSymbols;
		}

		[[nodiscard]] const OccurrenceColumns& Occurrences() const noexcept
		{
			return mOccurrences;
		}

		[[nodiscard]] const RelationColumns& Relations() const noexcept
		{
			return mRelations;
		}

		/** `#include` spellings in source order. */
		[[nodiscard]] std::span<const StringId> Includes() const noexcept
		{
			return mIncludes;
		}

		/** Occurrence indices of a local symbol, ascending by offset. */
		[[nodiscard]] std::span<const std::uint32_t> OccurrencesOf(
			std::uint32_t symbol) const noexcept;

		/** Occurrence index covering `offset`, or kInvalid. */
		[[nodiscard]] std::uint32_t OccurrenceAt(std::uint32_t offset) const noexcept;

		/** Local symbol declared at or referenced by `offset`, or kInvalid. */
		[[nodiscard]] std::uint32_t SymbolAt(std::uint32_t offset) const noexcept;

		/** Relation index range whose subject equals `subject`. */
		[[nodiscard]] std::pair<std::size_t, std::size_t> RelationsFrom(
			SymbolHash subject) const noexcept;

		/** Relation indices whose object equals `object`. */
		[[nodiscard]] std::span<const std::uint32_t> RelationsTo(SymbolHash object) const noexcept;

		[[nodiscard]] std::string_view Name(std::uint32_t symbol) const noexcept
		{
			return mStrings.Text(mSymbols.name[symbol]);
		}

		[[nodiscard]] std::string_view Qualified(std::uint32_t symbol) const noexcept
		{
			return mStrings.Text(mSymbols.qualified[symbol]);
		}

		/** Owned storage, for budgets and benchmarks. */
		[[nodiscard]] std::size_t StorageBytes() const noexcept;

	private:
		friend class ShardBuilderImpl;

		/** Sorts relations, builds the per-symbol occurrence lists and reverse relation order. */
		void Finalize();

		std::string mPath;
		std::uint64_t mContentHash = 0;
		std::uint64_t mConfigHash  = 0;
		StringTable mStrings;
		SymbolColumns mSymbols;
		OccurrenceColumns mOccurrences;
		RelationColumns mRelations;
		std::vector<StringId> mIncludes;
		std::vector<std::uint32_t> mSymbolOccurrenceBegin; // CSR over mSymbolOccurrenceList
		std::vector<std::uint32_t> mSymbolOccurrenceList;
		std::vector<std::uint32_t> mRelationsByObject;
	};
} // namespace heimdall::index
