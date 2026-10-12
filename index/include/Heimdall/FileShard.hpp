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
		Dependent,
		Ambiguous // several visible declarations fit; only produced by snapshot resolution
	};

	/** Arity bound meaning "unknown" for non-functions and "unbounded" for variadics. */
	inline constexpr std::uint8_t kNoArity = 0xFF;
	/** Unknown/non-scalar argument or parameter; never used to rank overloads. */
	inline constexpr std::uint8_t kUnknownBuiltin = 0xFF;

	/** Occurrence flag bits. */
	inline constexpr std::uint8_t kOccurrenceMember = 1; // written after `.` or `->`

	enum class RelationKind : std::uint8_t
	{
		BaseOf,       // subject = base symbol hash, object = derived symbol hash
		BaseOfByName, // subject = hash of the written base name, object = derived hash
	};

	enum class UsingKind : std::uint8_t
	{
		Namespace,   // using namespace path;
		Declaration, // using path::name;
		Alias        // using name = path...;
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
		/** Full identity key verified on hash hits; signature spelling is stored separately. */
		std::vector<StringId> identity;
		std::vector<StringId> signature;
		std::vector<std::uint32_t> container; // local index of the owning class/namespace
		std::vector<std::uint32_t> flags;     // heimdall::SymbolFlag bits
		std::vector<std::uint32_t> nameOffset;
		std::vector<std::uint32_t> nameLength;
		std::vector<heimdall::SymbolKind> kind;
		std::vector<Role> role;
		std::vector<std::uint8_t> paramMin; // functions: required parameters, else kNoArity
		std::vector<std::uint8_t> paramMax; // functions: accepted parameters, kNoArity if variadic
		std::vector<std::uint32_t> paramTypeBegin;
		std::vector<std::uint32_t> paramTypeCount;

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
		std::vector<std::uint32_t> scope; // innermost ScopeColumns entry, or kInvalid
		std::vector<StringId> qualifier;  // written `a::b` prefix, kInvalid when none
		/** Receiver class/type spelling from typing or a simple declared type; kInvalid when unknown. */
		std::vector<StringId> receiver;
		/** Scope and position where the receiver's type name was declared. */
		std::vector<std::uint32_t> receiverScope;
		std::vector<std::uint32_t> receiverOffset;
		std::vector<std::uint8_t> argCount; // arguments of a call, kNoArity when not a call
		std::vector<std::uint8_t> flags;    // kOccurrence* bits
		std::vector<std::uint32_t> argTypeBegin;
		std::vector<std::uint32_t> argTypeCount;

		[[nodiscard]] std::size_t Size() const noexcept
		{
			return offset.size();
		}
	};

	/** Namespace and class extents (and out-of-line member bodies), sorted by `begin`. */
	struct ScopeColumns
	{
		std::vector<std::uint32_t> begin;
		std::vector<std::uint32_t> end;
		std::vector<std::uint32_t> parent; // enclosing entry or kInvalid
		std::vector<StringId> qualified;   // `a::B`; empty for none

		[[nodiscard]] std::size_t Size() const noexcept
		{
			return begin.size();
		}
	};

	/** `using` directives, declarations and aliases at namespace or class scope. */
	struct UsingColumns
	{
		/** Source offset of the introducing using directive/declaration. */
		std::vector<std::uint32_t> offset;
		std::vector<std::uint32_t> scope; // ScopeColumns entry or kInvalid for the global scope
		std::vector<UsingKind> kind;
		std::vector<StringId> name; // alias name or the declared name
		std::vector<StringId> path; // written target path

		[[nodiscard]] std::size_t Size() const noexcept
		{
			return scope.size();
		}
	};

	/** Inheritance edges sorted by subject. */
	struct RelationColumns
	{
		std::vector<SymbolHash> subject;
		std::vector<SymbolHash> object;
		std::vector<RelationKind> kind;
		std::vector<std::uint32_t> offset; // written base name, kInvalid for exact relations

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

		[[nodiscard]] const ScopeColumns& Scopes() const noexcept
		{
			return mScopes;
		}

		[[nodiscard]] const UsingColumns& Usings() const noexcept
		{
			return mUsings;
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

		/** Exact resolved target for each include, or kInvalid when unavailable. */
		[[nodiscard]] std::span<const StringId> IncludeTargets() const noexcept
		{
			return mIncludeTargets;
		}

		/** Source positions corresponding to Includes(). */
		[[nodiscard]] std::span<const std::uint32_t> IncludeOffsets() const noexcept
		{
			return mIncludeOffsets;
		}

		/** False when include visibility still uses spelling heuristics or an opaque directive. */
		[[nodiscard]] bool ExactIncludes() const noexcept
		{
			return mExactIncludes;
		}

		/** The parser finished without syntax diagnostics; not a complete C++ type-check claim. */
		[[nodiscard]] bool SyntaxComplete() const noexcept
		{
			return mSyntaxComplete;
		}

		/** BuiltinType encodings, or kUnknownBuiltin, for a function's parameters. */
		[[nodiscard]] std::span<const std::uint8_t> ParameterTypes(std::uint32_t symbol) const noexcept
		{
			return std::span(mParameterTypes).subspan(mSymbols.paramTypeBegin[symbol], mSymbols.paramTypeCount[symbol]);
		}

		/** BuiltinType encodings, or kUnknownBuiltin, for a call's supplied arguments. */
		[[nodiscard]] std::span<const std::uint8_t> ArgumentTypes(std::uint32_t occurrence) const noexcept
		{
			return std::span(mArgumentTypes).subspan(mOccurrences.argTypeBegin[occurrence], mOccurrences.argTypeCount[occurrence]);
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

		/** Exact identity spelling behind the hash accelerator. */
		[[nodiscard]] std::string_view Identity(std::uint32_t symbol) const noexcept
		{
			return mStrings.Text(mSymbols.identity[symbol]);
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
		ScopeColumns mScopes;
		UsingColumns mUsings;
		std::vector<StringId> mIncludes;
		std::vector<StringId> mIncludeTargets;
		std::vector<std::uint32_t> mIncludeOffsets;
		bool mExactIncludes = true;
		bool mSyntaxComplete = true;
		std::vector<std::uint8_t> mParameterTypes;
		std::vector<std::uint8_t> mArgumentTypes;
		std::vector<std::uint32_t> mSymbolOccurrenceBegin; // CSR over mSymbolOccurrenceList
		std::vector<std::uint32_t> mSymbolOccurrenceList;
		std::vector<std::uint32_t> mRelationsByObject;
	};
} // namespace heimdall::index
