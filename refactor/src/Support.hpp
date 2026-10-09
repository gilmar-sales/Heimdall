#pragma once
#include <Heimdall/Refactoring.hpp>

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
} // namespace heimdall::refactor_detail
