#pragma once

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/ParseTree.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace heimdall
{

    enum class AsteriskMeaning
    {
        NotApplicable,
        Declaration,
        Multiplication,
        Ambiguous
    };

    struct SemanticDiagnostic
    {
        std::string code;
        std::string message;
        std::size_t offset;
        std::size_t length;
        std::uint32_t line;
        std::uint32_t column;
    };

    // Deliberately local type oracle: records types declared in this translation
    // unit plus built-ins, without opening transitively included headers.
    class SemanticAnalyzer
    {
    public:
        std::unordered_set<std::string> CollectTypeNames(std::string_view source,
            const CompileCommand *command = nullptr) const;

        AsteriskMeaning ClassifyAsteriskStatement(std::string_view statement,
            const std::unordered_set<std::string> & known_types,
            const std::unordered_set<std::string> & known_values = {}) const;

        std::vector<SemanticDiagnostic> AnalyzeUnusedLocals(std::string_view source,
            const CompileCommand *command = nullptr) const;
        // ParseTree-backed overload: reuses the tree's tokens and directives
        // instead of re-lexing + re-processing the same buffer.
        std::vector<SemanticDiagnostic> AnalyzeUnusedLocals(const ParseTree &tree,
            const CompileCommand *command = nullptr) const;

    private:
        std::unordered_set<std::string> CollectTypeNamesFromViews(const std::vector<std::string_view> & tokens,
            const CompileCommand *command) const;
        std::vector<SemanticDiagnostic> AnalyzeUnusedLocalsImpl(std::string_view source,
            const std::vector<Token> & lexed,
            const PreprocessorResult &preprocessing,
            const CompileCommand *command) const;
    };

} // namespace heimdall
