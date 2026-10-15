#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Heimdall/ParseTree.hpp>

namespace heimdall
{

	enum class Severity : std::uint8_t
	{
		Warning,
		Error
	};

	enum class RuleId : std::uint8_t
	{
		NullMacro,
		TrailingWhitespace,
		MissingFinalNewline,
		EmptyCatch,
		DuplicateInclude,
		LegacyTypedef,
		UnusedInclude,
		PreferForwardDeclaration,
		UnsortedIncludes,
		CircularInclude,
		TodoComment,
		MagicNumber,
		ConstantNaming,
		ModernizeOverride,
		ModernizeNullptr,
		NoZeroAsNull,
		ModernizeAuto,
		ModernizeEmplace,
		ModernizeMakeUnique,
		ModernizeMakeShared,
		ModernizeSmartPtr,
		NoNewDelete,
		ModernizeSpan,
		ModernizeStringView,
		ModernizeAlgorithms,
		ModernizeStructuredBindings,
		ModernizeAttributes,
		ModernizeConstevalConstexpr,
		NoImplicitBoolConversion,
		ModernizeRangeLoop,
		ModernizeLoopConvert,
		IncludeWhatYouUse,
		ModernizeFinal,
		ModernizeConst,
		ModernizeConstexpr,
		ApiVirtualDestructor,
		ApiMissingNodiscard,
		ApiPassByValue,
		ApiPassByConstReference,
		ApiConstCorrectness,
		ApiUnsafeDowncast,
		ApiSlicing,
		ApiImplicitConversion,
		ApiExplicitConstructor,
		ApiOverloadHiding,
		ApiVirtualCallInConstructor,
		DesignatedInitOrder,
		NoIntegerToPointer,
		CallArgumentCount,
		CallArgumentType,
		DotOnPointer,
		InaccessibleMember,
		ConflictingDeclaration,
		UndeclaredType,
		UnreachableCode,
		MissingReturn,
		ReturnType,
		DocRequireComment,
		DocDoxygenStyle,
		ConceptRecursion,
		ConceptConstrained,
		ConceptPlacement,
		ConceptSpecialization,
		RequiresForm,
		RequiresParameter,
		ConceptArity,
		RequiresValidity,
		ConceptRedeclaration,
		// Experimental external rules use their string code as identity.
		External,
		/// Proven failures of local template substitution, never speculative candidate notes.
		TemplateSubstitution
	};

	enum class ConstantNameCasing : std::uint8_t
	{
		KPrefix,
		ScreamingSnakeCase
	};

	// Which declarations the doc/* rules look at. Public: what other code can
	// use (public and protected members, namespace-scope entities with external
	// linkage). Private: private members and internal-linkage entities
	// (`static`, anonymous namespace). All: both.
	enum class DocScope : std::uint8_t
	{
		Public,
		Private,
		All
	};

	struct RuleOverride
	{
		std::string code;
		bool enabled      = true;
		Severity severity = Severity::Warning;
	};

	struct TextEdit
	{
		std::size_t offset;
		std::size_t length;
		std::string replacement;
	};

	struct Diagnostic
	{
		RuleId rule;
		Severity severity;
		std::string code;
		std::string message;
		std::size_t offset;
		std::size_t length;
		std::uint32_t line;
		std::uint32_t column;
		bool has_fix;
		TextEdit fix;
		bool fix_is_safe = true;
		std::string fix_title;
		bool has_related_information = false;
		std::size_t related_offset   = 0;
		std::size_t related_length   = 0;
		std::string related_message;
	};

	// Include groups recognized by cpp/sort-includes. The configured
	// order of the groups decides where each include belongs.
	enum class IncludeGroup : std::uint8_t
	{
		Angle, // #include <header>
		Quote  // #include "header"
	};

	struct RuleOptions
	{
		bool null_macro                         = true;
		bool trailing_whitespace                = true;
		bool final_newline                      = true;
		bool empty_catch                        = true;
		bool duplicate_include                  = true;
		bool legacy_typedef                     = true;
		bool todo_comment                       = true;
		bool magic_numbers                      = false;
		ConstantNameCasing constant_name_casing = ConstantNameCasing::KPrefix;
		bool modernize_emplace                  = false;
		bool modernize_make_unique              = false;
		bool modernize_make_shared              = false;
		bool modernize_smart_ptr                = false;
		bool no_new_delete                      = false;
		bool modernize_structured_bindings      = false;
		bool modernize_algorithms               = false;
		bool sort_includes                      = false;
		bool honor_suppressions                 = true;
		std::vector<RuleOverride> overrides;
		std::vector<IncludeGroup> include_order = {IncludeGroup::Angle, IncludeGroup::Quote};
		bool include_case_insensitive           = true;
		std::string main_file_name;
		DocScope doc_scope                  = DocScope::Public;
		std::size_t macro_max_include_depth = 200;
		std::size_t macro_max_macros        = 65536;
	};

	// Stable metadata for every rule the engine can emit. The catalog is the
	// single source of truth for rule code validation in the config loader
	// and the CLI, so new rules only need an entry here.
	struct RuleInfo
	{
		RuleId id;
		std::string_view code;
		std::string_view category;
		Severity default_severity;
		std::string_view layer;
		bool autofix;
		std::string_view summary;
	};

	const std::vector<RuleInfo>& RuleCatalog();

	const RuleInfo* FindRuleByCode(std::string_view code);

	const RuleInfo* FindRule(RuleId id);

	bool IsKnownRuleCode(std::string_view code);

	// Edit that deletes the preprocessor directive at [offset, offset+length)
	// together with its indentation and line terminator.
	TextEdit RemoveDirectiveLine(std::string_view source, std::size_t offset, std::size_t length);

	class RuleEngine
	{
	public:
		explicit RuleEngine(RuleOptions options = {}) : m_options(options) {}

		std::vector<Diagnostic> Analyze(std::string_view source) const;

		std::vector<Diagnostic> Analyze(const ParseTree& tree) const;

		// Severity overrides, disabled rules and suppression comments, for
		// diagnostics produced outside core (for example by the semantic layer).
		// Returns them sorted by offset.
		std::vector<Diagnostic> ApplyPolicy(
			std::vector<Diagnostic> diagnostics,
			const ParseTree& tree) const
		{
			return ApplyPolicy(std::move(diagnostics), tree.Source(), tree.Tokens());
		}

		// Whether an opt-in rule (one that is off unless a config file or --rule
		// enables it) was switched on. For rules emitted outside core.
		bool OptInEnabled(std::string_view code) const
		{
			return RuleEnabled(code, false);
		}

		bool Enabled(std::string_view code, bool default_enabled = true) const
		{
			return RuleEnabled(code, default_enabled);
		}

		DocScope DocumentationScope() const
		{
			return m_options.doc_scope;
		}

		/// Returns the configured rule and include-order options.
		[[nodiscard]] const RuleOptions& Options() const noexcept
		{
			return m_options;
		}

		// Applies the non-overlapping fixes. Fixes marked unsafe (editor quick fixes)
		// are only applied when include_unsafe is set.
		static std::string ApplyFixes(
			std::string_view source,
			const std::vector<Diagnostic>& diagnostics,
			bool include_unsafe = false);

		// Canonical include-section text: every sortable run of literal
		// includes is ordered by the sort key and regrouped with one blank
		// line between bands. Unlike Analyze (which only reports misordered
		// sections), this also consolidates bands that are already ordered
		// but scattered across stray blank lines. Only the sort-includes
		// subset of options is read (sort flag, order, case, file name).
		static std::string SortIncludes(std::string_view source, const RuleOptions& options);

	private:
		// Opt-in rules run when enabled in RuleOptions or through an
		// enabled override (config file or --rule); the last override
		// for the code wins.
		bool RuleEnabled(std::string_view code, bool default_enabled) const;

		std::vector<Diagnostic> ApplyPolicy(
			std::vector<Diagnostic> diagnostics,
			std::string_view source,
			const std::vector<Token>& tokens) const;

		std::vector<Diagnostic> AnalyzeImpl(
			std::string_view source,
			const std::vector<Token>& tokens,
			const std::vector<PreprocessorDirective>& directives) const;

		RuleOptions m_options;
	};

} // namespace heimdall
