#pragma once

#include "CliOptions.hpp"

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticAnalyzer.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall::cli
{

    struct SyntaxDiagnostic
    {
        std::uint32_t line = 1;
        std::uint32_t column = 1;
        std::string code = "syntax/parse-error";
        std::string message;
    };

    struct ParseNodeSummary
    {
        std::string kind;
        std::size_t parent = 0;
        std::size_t offset = 0;
        std::size_t length = 0;
    };

    struct FileResult
    {
        std::filesystem::path path;
        std::string error;
        std::vector<heimdall::Diagnostic> diagnostics;
        std::vector<heimdall::SemanticDiagnostic> semantic_diagnostics;
        std::vector<SyntaxDiagnostic> syntax_diagnostics;
        std::vector<ParseNodeSummary> nodes;
        std::string standard_name = "c++20";
        std::string output;
        bool changed = false;
        bool has_semantic_context = false;
        std::size_t type_count = 0;
        std::size_t declaration_count = 0;
        std::size_t ambiguous_count = 0;
    };

    std::string_view GrammarKindName(heimdall::GrammarKind kind);
    std::string_view StandardName(heimdall::CppStandard standard);

    heimdall::ParserOptions ParserOptionsForFile(const std::filesystem::path & path,
        const Options &options,
        const heimdall::CompileDatabase * database);

    std::vector<SyntaxDiagnostic> ToSyntaxDiagnostics(
        std::string_view source, const std::vector<heimdall::GrammarDiagnostic> & grammar);

    void ProcessFile(const std::filesystem::path & path, const Options &options,
        const heimdall::CompileDatabase * database, FileResult &result);

    void RunParallel(const std::vector<std::filesystem::path> & files, const Options &options,
        const heimdall::CompileDatabase * database, std::vector<FileResult> & results);

} // namespace heimdall::cli
