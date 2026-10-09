#include <Heimdall/Refactoring.hpp>

#include <algorithm>
#include <unordered_set>

namespace heimdall
{
    std::expected<std::vector<RefactoredSource>, RefactoringError> PreviewRefactoring(
        const AnalysisSnapshot& current, const RefactoringPlan& plan, std::stop_token stop)
    {
        if (current.Revision() != plan.revision)
        {
            return std::unexpected(
                RefactoringError { RefactoringErrorCode::StaleSnapshot,
                                   "Workspace changed; request the refactoring again" });
        }

        std::unordered_set<DocumentId> seen;
        std::vector<RefactoredSource>  result;
        for (const auto& document : plan.documents)
        {
            if (stop.stop_requested())
            {
                return std::unexpected(
                    RefactoringError { RefactoringErrorCode::Cancelled, "Request cancelled" });
            }

            const auto source = current.Source(document.document);
            if (!source || !document.original ||
                current.Version(document.document) != document.version ||
                current.Path(document.document) != document.path || *source != *document.original)
            {
                return std::unexpected(RefactoringError {
                    RefactoringErrorCode::StaleSnapshot, "Document content or version changed" });
            }

            if (!seen.insert(document.document).second)
            {
                return std::unexpected(RefactoringError {
                    RefactoringErrorCode::ConflictingEdits, "Duplicate document in edit plan" });
            }

            if (document.options)
            {
                const auto& expected = *document.options;
                const auto& actual   = current.Options(document.document);
                const auto  expected_names =
                    expected.type_names ? expected.type_names->Fingerprint() : 0;
                const auto actual_names = actual.type_names ? actual.type_names->Fingerprint() : 0;
                if (expected.standard != actual.standard || expected.Macros() != actual.Macros() ||
                    expected_names != actual_names)
                {
                    return std::unexpected(RefactoringError {
                        RefactoringErrorCode::StaleSnapshot,
                        "Parser configuration changed; request the refactoring again" });
                }
            }

            std::vector<const SourceEdit*> edits;
            for (const auto& edit : document.edits)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(
                        RefactoringError { RefactoringErrorCode::Cancelled, "Request cancelled" });
                }

                if (edit.offset > source->size() || edit.length > source->size() - edit.offset ||
                    edit.expected.size() != edit.length ||
                    source->compare(edit.offset, edit.length, edit.expected) != 0)
                {
                    return std::unexpected(
                        RefactoringError { RefactoringErrorCode::InvalidEdit,
                                           "Edit range or expected content is invalid" });
                }

                edits.push_back(&edit);
            }

            std::sort(edits.begin(), edits.end(),
                      [](const auto* a, const auto* b) { return a->offset < b->offset; });
            for (std::size_t i = 1; i < edits.size(); ++i)
            {
                if (edits[i]->offset == edits[i - 1]->offset ||
                    edits[i]->offset < edits[i - 1]->offset + edits[i - 1]->length)
                {
                    return std::unexpected(RefactoringError {
                        RefactoringErrorCode::ConflictingEdits,
                        "Overlapping edits or unordered insertions at the same position" });
                }
            }

            auto& output = result.emplace_back(document.document, document.path, *source).source;
            for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(
                        RefactoringError { RefactoringErrorCode::Cancelled, "Request cancelled" });
                }

                output.replace((*edit)->offset, (*edit)->length, (*edit)->replacement);
            }
        }

        return result;
    }
} // namespace heimdall
