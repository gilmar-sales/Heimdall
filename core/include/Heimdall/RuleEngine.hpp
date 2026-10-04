#pragma once

#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
        LegacyTypedef
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
    };

    struct RuleOptions
    {
        bool null_macro = true;
        bool trailing_whitespace = true;
        bool final_newline = true;
        bool empty_catch = true;
        bool duplicate_include = true;
        bool legacy_typedef = true;
        bool honor_suppressions = true;
        std::vector<RuleOverride> overrides;
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

    class RuleEngine
    {
    public:
        explicit RuleEngine(RuleOptions options = {}) : m_options(options) {}

        std::vector<Diagnostic> Analyze(std::string_view source) const;
        std::vector<Diagnostic> Analyze(const ParseTree &tree) const;
        static std::string ApplyFixes(std::string_view source, const std::vector<Diagnostic> & diagnostics);

    private:
        std::vector<Diagnostic> AnalyzeImpl(std::string_view source, const std::vector<Token> & tokens,
            const std::vector<PreprocessorDirective> & directives) const;
        RuleOptions m_options;
    };

} // namespace heimdall
