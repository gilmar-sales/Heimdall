#pragma once

#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
        MagicNumber
    };

    struct RuleOverride
    {
        std::string code;
        bool enabled = true;
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
        // Safe fixes preserve behavior and are applied in batch (--fix).
        // Unsafe ones are only offered as editor quick fixes.
        bool fix_is_safe = true;
        std::string fix_title;
    };

    // Include groups recognized by cpp/sort-includes. The configured
    // order of the groups decides where each include belongs.
    enum class IncludeGroup : std::uint8_t
    {
        Angle,  // #include <header>
        Quote   // #include "header"
    };

    struct RuleOptions
    {
        bool null_macro = true;
        bool trailing_whitespace = true;
        bool final_newline = true;
        bool empty_catch = true;
        bool duplicate_include = true;
        bool legacy_typedef = true;
        bool todo_comment = true;
        bool magic_numbers = true;
        // Opt-in: the order is a project convention, so the rule only
        // runs when enabled together with a configured order.
        bool sort_includes = false;
        bool honor_suppressions = true;
        std::vector<RuleOverride> overrides;
        std::vector<IncludeGroup> include_order = {IncludeGroup::Angle,
            IncludeGroup::Quote};
        bool include_case_insensitive = true;
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

    const std::vector<RuleInfo> & RuleCatalog();
    const RuleInfo * FindRuleByCode(std::string_view code);
    const RuleInfo * FindRule(RuleId id);
    bool IsKnownRuleCode(std::string_view code);

    // Edit that deletes the preprocessor directive at [offset, offset+length)
    // together with its indentation and line terminator.
    TextEdit RemoveDirectiveLine(std::string_view source, std::size_t offset, std::size_t length);

    class RuleEngine
    {
    public:
        explicit RuleEngine(RuleOptions options = {}) : m_options(options) {}

        std::vector<Diagnostic> Analyze(std::string_view source) const;
        std::vector<Diagnostic> Analyze(const ParseTree &tree) const;
        // Severity overrides, disabled rules and suppression comments, for
        // diagnostics produced outside core (for example by the semantic layer).
        // Returns them sorted by offset.
        std::vector<Diagnostic> ApplyPolicy(std::vector<Diagnostic> diagnostics, const ParseTree &tree) const
        {
            return ApplyPolicy(std::move(diagnostics), tree.Source(), tree.Tokens());
        }

        static std::string ApplyFixes(std::string_view source, const std::vector<Diagnostic> & diagnostics);

    private:
        // Opt-in rules run when enabled in RuleOptions or through an
        // enabled override (config file or --rule); the last override
        // for the code wins.
        bool RuleEnabled(std::string_view code, bool default_enabled) const;
        std::vector<Diagnostic> ApplyPolicy(std::vector<Diagnostic> diagnostics, std::string_view source,
            const std::vector<Token> & tokens) const;
        std::vector<Diagnostic> AnalyzeImpl(std::string_view source, const std::vector<Token> & tokens,
            const std::vector<PreprocessorDirective> & directives) const;
        RuleOptions m_options;
    };

} // namespace heimdall
