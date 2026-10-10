#pragma once
#include <Heimdall/Lexer.hpp>
#include <Heimdall/Refactoring.hpp>

#include <vector>

namespace heimdall::refactor_detail
{
    bool ValidName(std::string_view name);

    // Verifies that preprocessing preserves the written tokens of the main
    // document. Header expansions are excluded using compiler line markers.
    std::expected<void, RefactoringError> VerifyPreprocessing(
        const AnalysisSnapshot& snapshot,
        DocumentId              document,
        std::string_view        source,
        std::stop_token         stop);

    // Token containing `offset` (binary search; tokens are offset-ordered),
    // or kNone. Shared by the rename and signature paths.
    std::uint32_t FindSelectedToken(const std::vector<Token>& tokens, std::size_t offset);

    struct WorkspaceEntity
    {
        std::shared_ptr<const ProjectSymbolIndex> index;
        EntityId                                  entity = InvalidEntity;
    };

    // Cross-file identity for free functions and globals via the workspace
    // symbol index (per-TU models cannot see other translation units). Only
    // index-supported entities with complete coverage resolve; overloads,
    // members, templates and partial models stay honestly unsupported.
    std::expected<WorkspaceEntity, RefactoringError> ResolveWorkspaceEntity(
        const AnalysisSnapshot& snapshot, DocumentId document, std::size_t offset,
        std::stop_token stop);
} // namespace heimdall::refactor_detail
