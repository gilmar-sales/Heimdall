#pragma once

#include <Heimdall/Workspace.hpp>

#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace heimdall
{
    enum class RefactoringErrorCode
    {
        InvalidSelection,
        InvalidName,
        UnsupportedSymbol,
        IncompleteAnalysis,
        MacroContext,
        NameCollision,
        StaleSnapshot,
        ConflictingEdits,
        InvalidEdit,
        SemanticChange,
        Cancelled
    };

    struct RefactoringError
    {
        RefactoringErrorCode code;
        std::string          message;
    };

    struct SourceEdit
    {
        std::size_t offset = 0;
        std::size_t length = 0;
        std::string expected;
        std::string replacement;
    };

    // Pins the complete original content, not just an mtime or a hash. This
    // also makes preview generation independent of mutable frontend buffers.
    struct DocumentEdits
    {
        DocumentId                         document = InvalidDocument;
        std::filesystem::path              path;
        std::int64_t                       version = 0;
        std::shared_ptr<const std::string> original;
        std::vector<SourceEdit>            edits;
        std::optional<ParserOptions>       options;
    };

    struct RefactoringPlan
    {
        std::string                title;
        std::uint64_t              revision = 0;
        std::vector<DocumentEdits> documents;
    };

    struct RefactoredSource
    {
        DocumentId            document = InvalidDocument;
        std::filesystem::path path;
        std::string           source;
    };

    struct SymbolOccurrences
    {
        SymbolId                   symbol = kNone;
        std::size_t                offset = 0;
        std::size_t                length = 0;
        std::string                name;
        std::vector<std::uint32_t> tokens;
    };

    // No filesystem writes. All files are validated before any preview is
    // returned. Frontends are responsible for versioned/transactional apply.
    std::expected<std::vector<RefactoredSource>, RefactoringError> PreviewRefactoring(
        const AnalysisSnapshot& current, const RefactoringPlan& plan, std::stop_token stop = {});

    class RefactoringService
    {
      public:
        static std::expected<SymbolOccurrences, RefactoringError> LocalReferences(
            const AnalysisSnapshot& snapshot,
            DocumentId              document,
            std::size_t             offset,
            bool                    include_declaration = true,
            std::stop_token         stop                = {});

        static std::expected<RefactoringPlan, RefactoringError> RenameLocal(
            const AnalysisSnapshot& snapshot,
            DocumentId              document,
            std::size_t             offset,
            std::string_view        new_name,
            std::stop_token         stop = {});

        // First supported subset: a complete integral/bool constant expression in a
        // standalone return statement. Other selections return a reason.
        static std::expected<RefactoringPlan, RefactoringError> ExtractVariable(
            const AnalysisSnapshot& snapshot,
            DocumentId              document,
            std::size_t             offset,
            std::size_t             length,
            std::string_view        new_name,
            std::stop_token         stop = {});

        // A complete integral literal return expression in a free function.
        // No inputs/outputs, RAII objects or transfers of control are moved.
        static std::expected<RefactoringPlan, RefactoringError> ExtractFunction(
            const AnalysisSnapshot& snapshot,
            DocumentId              document,
            std::size_t             offset,
            std::size_t             length,
            std::string_view        new_name,
            std::stop_token         stop = {});

        // First supported subset: an integral local initialized by a same-type
        // literal, whose reads are complete standalone return expressions.
        static std::expected<RefactoringPlan, RefactoringError> InlineVariable(
            const AnalysisSnapshot& snapshot,
            DocumentId              document,
            std::size_t             offset,
            std::stop_token         stop = {});
    };
} // namespace heimdall
