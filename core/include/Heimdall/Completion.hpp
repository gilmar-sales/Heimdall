#pragma once

#include <Heimdall/ParseTree.hpp>
#include <Heimdall/TemplateDeduction.hpp>

#include <Heimdall/TypeNames.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heimdall
{

	enum class CompletionKind : std::uint8_t
	{
		Keyword,
		Type,
		Namespace,
		Function,
		Variable,
		Macro,
		Directive
	};

	// Access level of a class member; namespace-scope names are Public.
	enum class MemberAccess : std::uint8_t
	{
		Public,
		Protected,
		Private
	};

	struct CompletionItem
	{
		std::string label;
		CompletionKind kind = CompletionKind::Keyword;
		// Short signature shown beside the label (`int add(int left, int right)`).
		std::string detail;
		// Doc comment shown in the side popup (sent as LSP markdown).
		std::string documentation;
		// Source location of the declaring name, filled for indexed declarations
		// (navigation: go to definition / implementation). `file` indexes the
		// owning IncludeIndex::Files(); -1 means the buffer the index was built
		// from.
		std::int32_t file    = -1;
		std::uint32_t offset = 0;
		bool has_location    = false;
		// Function with a body, record/enum with a body, variable or alias.
		bool is_definition = false;
		// Functions: number of parameters (overload matching for navigation).
		std::uint32_t param_count = 0;
		// Type as written, for member access (`obj.`): the declared type of a
		// variable/field, the return type of a function, or the aliased type of
		// a `using`/`typedef` (`basic_string<char>` for `std::string`).
		std::string type_text;
		// Variables: the type with its declarator (`char*`, `int[4]`), prefixed
		// `static ` for non-instance storage and `?` when it cannot be spelled
		// (bitfields). Enums: the underlying type, empty when implicit.
		std::string layout_type;
		// Filled by CompletionEngine::Hover when the layout is known.
		bool has_layout           = false;
		std::uint64_t size_bytes  = 0;
		std::uint64_t align_bytes = 0;
		// Data members: byte offset inside the owning record (`offsetof`).
		bool has_field_offset      = false;
		std::uint64_t field_offset = 0;
		// Hover on an alias: resolved underlying type, displayed as <type>.
		std::string type_origin;
		// Declared `[[deprecated]]` (LSP completion tag).
		bool deprecated = false;
		// Text the typed word is matched against when it differs from the label
		// (override suggestions are labeled with the whole signature).
		std::string filter_text;
		MemberAccess access = MemberAccess::Public;
		// Member functions callable on a const object (`const`, `static`, or an
		// explicit object taken by value or const reference).
		bool is_const = false;
		// Member functions declared `virtual` or overriding a virtual (`override`/`final`).
		bool is_virtual = false;
		// Member functions taking an explicit object parameter (`this Self& self`).
		bool explicit_object = false;
		// Functions defined `= delete`: not callable, so signature help skips them.
		bool deleted = false;
		/// Template parameter names used to substitute explicit call and alias arguments.
		std::vector<std::string> templateParams;
		/// Shared return metadata, populated only for functions with deduced return types.
		std::shared_ptr<const FunctionDeduction> deduction;
	};

	// One named scope and its direct members. `path` is the qualified path from
	// the translation unit (`{"std", "chrono"}`); an empty path denotes the
	// global scope. `kind` describes the scope itself when listed as a member
	// (Namespace for namespaces, Type for records). Backbone for header indexing:
	// headers are parsed exactly like sources and their scopes merged by path.
	struct IndexedScope
	{
		std::vector<std::string> path;
		CompletionKind kind = CompletionKind::Type;
		std::vector<CompletionItem> members;
		// Base classes of a record scope as written (`ns::Base`, template
		// arguments dropped); member access walks them for inherited members.
		std::vector<std::string> bases;
		// Template parameter names of a record template (`T`, `E` for `expected<T, E>`).
		std::vector<std::string> template_params;
		// The record's layout cannot be derived from the members indexed here
		// (virtuals, bitfields, anonymous members, packing, `alignas`, hidden fields).
		bool layout_unknown = false;
		/// A conversion operator may return a reference; conservative argument checks must defer.
		bool hasReferenceConversion = false;
	};

	using ScopeIndex = std::vector<IndexedScope>;

	// One callable offered by signature help. `label` reads like
	// `add(int left, int right = 0) -> int`; each parameter is a [begin, end) byte
	// range of the label.
	struct SignatureInfo
	{
		std::string label;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> parameters;
		std::string documentation;
		/** Parameter names in argument order; empty entries denote unnamed parameters. */
		std::vector<std::string> parameterNames;
	};

	// What the word at the cursor completes, for frontends that add candidates of
	// their own (workspace symbols with include insertion).
	enum class CompletionContextKind : std::uint8_t
	{
		Unqualified, // `na|`
		Qualified,   // `ns::na|`, `::na|`
		Other        // member access, preprocessor, comments, new names
	};

	struct CompletionContext
	{
		CompletionContextKind kind = CompletionContextKind::Other;
		// Qualified: the written scope path (`{"ns"}`); empty for `::na`.
		std::vector<std::string> qualifier;
		std::string prefix;
	};

	// Overloads of the call around the cursor and the argument being typed.
	struct SignatureHelp
	{
		std::vector<SignatureInfo> signatures;
		std::uint32_t active_signature = 0;
		std::uint32_t active_parameter = 0;
		// Offset of the call's opening `(` or `{`; npos when the cursor is in no call.
		std::size_t call_open = std::string_view::npos;
	};

	// Lexical + local ParseTree completion engine (core, STL only).
	// Works on incomplete code: ParseTree diagnostics are ignored, candidates
	// are collected from lexer identifiers, DeclaredName nodes and macros,
	// then filtered by the identifier prefix immediately before `offset`.
	class CompletionEngine
	{
	public:
		/// Infers an auto variable or range-for binding using local and indexed declarations.
		[[nodiscard]] static std::string InferredType(const ParseTree& tree,
			std::size_t nameToken, const ScopeIndex* external = nullptr);

		/// Resolves a visible variable's type, expanding indexed aliases.
		[[nodiscard]] static std::string VariableType(const ParseTree& tree,
			std::size_t nameToken, const ScopeIndex* external = nullptr);

		static std::string PrefixAt(std::string_view source, std::size_t offset);

		static CompletionContext ContextAt(std::string_view source, std::size_t offset);

		static std::vector<CompletionItem> Complete(std::string_view source, std::size_t offset);

		static std::vector<CompletionItem> Complete(
			std::string_view source, const ParserOptions& options, std::size_t offset);

		static std::vector<CompletionItem> Complete(
			std::string_view source,
			const ParserOptions& options,
			std::size_t offset,
			const ScopeIndex* external);

		static std::vector<CompletionItem> Complete(
			const ParseTree& tree,
			const ParserOptions& options,
			std::size_t offset,
			const ScopeIndex* external = nullptr);

		// Names of the types the index declares at namespace level (class members are
		// left out): what the grammar parser needs to tell declarations from products
		// and casts from groups. Last component only, so `std::string` yields `string`.
		static std::shared_ptr<const TypeNameOracle> TypeNamesOf(const ScopeIndex& index);

		static ScopeIndex IndexScopes(std::string_view source, const ParserOptions& options);

		static ScopeIndex IndexScopes(const ParseTree& tree);

		// Signatures of the function, constructor or aggregate whose argument list
		// contains `offset` (`f(1, |`, `obj.m(|`, `S s{|`, `new S(|`).
		static SignatureHelp Signatures(
			std::string_view source,
			const ParserOptions& options,
			std::size_t offset,
			const ScopeIndex* external = nullptr);

		static SignatureHelp Signatures(
			const ParseTree& tree,
			const ParserOptions& options,
			std::size_t offset,
			const ScopeIndex* external = nullptr);

		static std::optional<CompletionItem> Hover(
			std::string_view source,
			const ParserOptions& options,
			std::size_t offset,
			const ScopeIndex* external = nullptr);

		static std::optional<CompletionItem> Hover(
			const ParseTree& tree,
			const ParserOptions& options,
			std::size_t offset,
			const ScopeIndex* external = nullptr);
	};

} // namespace heimdall
