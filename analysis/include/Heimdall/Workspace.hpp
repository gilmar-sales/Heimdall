#pragma once

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/Ids.hpp>
#include <Heimdall/ProjectIndex.hpp>
#include <Heimdall/ProjectSymbolIndex.hpp>
#include <Heimdall/TypeModel.hpp>

#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace heimdall
{
    namespace detail
    {
        struct WorkspaceState;
        struct DocumentAnalysis;
    }

    enum class WorkspaceError
    {
        InvalidDocument,
        StaleVersion,
        InvalidSource,
        IdExhausted
    };

    enum class ProjectLoadError
    {
        Cancelled, LimitReached, StaleSnapshot
    };

    struct ProjectLoadLimits
    {
        std::size_t documents = 4096;
        std::size_t source_bytes = 64u << 20;
    };

    struct ProjectLoadReport
    {
        std::vector<DocumentId> added;
        std::vector<std::filesystem::path> unavailable;
    };

    struct AnalysisMetrics
    {
        std::uint64_t parse_count = 0;
        std::uint64_t bind_count = 0;
        std::uint64_t type_count = 0;
        std::uint64_t cache_hits = 0;
        std::uint64_t parse_ns = 0;
        std::uint64_t bind_ns = 0;
        std::uint64_t type_ns = 0;
        std::size_t semantic_allocations = 0;
        std::size_t type_allocations = 0;
        std::uint64_t project_index_count = 0;
        std::uint64_t project_index_ns = 0;
        std::uint64_t symbol_index_count = 0;
        std::uint64_t symbol_index_ns = 0;
    };

    struct MemoryBudget
    {
        std::size_t source_bytes = 0;
        std::size_t syntax_bytes = 0;
        std::size_t semantic_arena_bytes = 0;
        std::size_t type_arena_bytes = 0;
        std::size_t project_string_bytes = 0;
        std::size_t symbol_index_bytes = 0;
        std::size_t retained_base_bytes = 0;

        std::size_t Total() const noexcept
        {
            return source_bytes + syntax_bytes + semantic_arena_bytes + type_arena_bytes
            + project_string_bytes + symbol_index_bytes + retained_base_bytes;
        }
    };

    // A logically immutable view. Lazy caches are synchronized per document;
    // building a cache never changes the source, options, graph or project view.
    // Unchanged documents share storage; no snapshot manager retains old views.
    class AnalysisSnapshot
    {
    public:
        AnalysisSnapshot() = default;

        std::uint64_t Revision() const noexcept;

        std::vector<DocumentId> Documents() const;

        DocumentId Find(const std::filesystem::path& path) const;

        bool Contains(DocumentId document) const noexcept;

        std::int64_t Version(DocumentId document) const noexcept;

        std::shared_ptr<const std::string> Source(DocumentId document) const;

        const std::filesystem::path& Path(DocumentId document) const;

        const ParserOptions& Options(DocumentId document) const;

        const CompileCommand* Command(DocumentId document) const;

        std::span<const CompileCommand> CompilationCommands() const;

        std::span<const DocumentId> Dependencies(DocumentId document) const;

        std::shared_ptr<const ParseTree> Syntax(DocumentId document) const;

        std::shared_ptr<const SemanticModel> Semantic(DocumentId document) const;

        std::shared_ptr<const TypeModel> Types(DocumentId document) const;

        std::shared_ptr<const HeaderSummary> Summary(DocumentId document) const;

        std::shared_ptr<const ProjectIndex> Project() const;

        std::expected<std::shared_ptr<const ProjectSymbolIndex>, SymbolIndexError> SymbolIndex(
            std::stop_token stop = {}) const;

        AnalysisMetrics Metrics() const;

        MemoryBudget Memory() const;

        // Bridge for frontends with an existing incremental/asynchronous parser.
        // The derived view pins exactly this tree without mutating any old view.
        std::expected<AnalysisSnapshot, WorkspaceError> WithSyntax(DocumentId document,
            std::shared_ptr<const ParseTree> tree, ParserOptions options) const;

        // A borrowed tree's source must outlive the view. Source() is null for
        // such a view; Syntax().Source() remains available without a buffer copy.
        static AnalysisSnapshot FromSyntax(std::shared_ptr<const ParseTree> tree,
            ParserOptions options = {}, std::filesystem::path path = {});

    private:
        friend class Workspace;

        explicit AnalysisSnapshot(std::shared_ptr<const detail::WorkspaceState> state);

        std::shared_ptr<const detail::DocumentAnalysis> Document(DocumentId document) const;

        std::shared_ptr<const detail::WorkspaceState> m_state;
    };

    // Writes are serialized, readers only pin a shared immutable state. Parsing,
    // binding and typing happen outside the workspace lock and remain unit-local.
    class Workspace
    {
    public:
        Workspace();

        AnalysisSnapshot Snapshot() const;

        std::expected<DocumentId, WorkspaceError> Open(
            std::filesystem::path path,
            std::shared_ptr<const std::string> source,
            std::int64_t version = 0,
            ParserOptions options = {});

        std::expected<void, WorkspaceError> Update(DocumentId document,
            std::shared_ptr<const std::string> source, std::int64_t version);

        std::expected<void, WorkspaceError> Close(DocumentId document);

        std::expected<void, WorkspaceError> SetDependencies(DocumentId document,
            std::span<const DocumentId> dependencies);

        std::expected<void, WorkspaceError> SetOptions(DocumentId document, ParserOptions options);

        void SetCompilationDatabase(std::shared_ptr<const CompileDatabase> database);

        // Explicit, transactional import of closed TUs and reachable includes.
        // Existing registered buffers win over disk. Imported contents stay
        // pinned until Update/Close; this is not a filesystem watcher.
        std::expected<ProjectLoadReport, ProjectLoadError> LoadProjectSources(
            ProjectLoadLimits limits = {}, std::stop_token stop = {});

        void SetProjectIndex(std::shared_ptr<const ProjectIndex> index);

    private:
        mutable std::mutex m_mutex;
        std::shared_ptr<const detail::WorkspaceState> m_state;
    };
}
