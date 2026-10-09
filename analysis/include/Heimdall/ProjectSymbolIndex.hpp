#pragma once

#include <Heimdall/Ids.hpp>
#include <Heimdall/TypeModel.hpp>

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace heimdall
{
    class AnalysisSnapshot;
    using EntityId                          = std::uint32_t;
    inline constexpr EntityId InvalidEntity = kNone;

    enum class OccurrenceResolution
    {
        Resolved,
        Unresolved,
        Ambiguous,
        Dependent,
        Unsupported
    };

    enum class OccurrenceRole
    {
        Declaration,
        Definition,
        Reference,
        Call
    };

    enum class EntityLinkage
    {
        Local,
        Internal,
        External
    };

    enum class SymbolIndexIssue
    {
        ParseErrors,
        MissingInclude,
        MissingTranslationUnit,
        Preprocessing,
        UnsupportedSyntax,
        ConflictingDeclarations,
        ConfigurationVariants
    };

    enum class SymbolIndexError
    {
        Cancelled,
        LimitReached
    };

    struct SymbolIndexLimits
    {
        std::size_t documents   = 4096;
        std::size_t symbols     = 1000000;
        std::size_t occurrences = 4000000;
    };

    struct IndexedOccurrence
    {
        DocumentId            document = InvalidDocument;
        std::uint32_t         token    = kNone;
        std::size_t           offset   = 0;
        std::size_t           length   = 0;
        std::string           name;
        EntityId              entity     = InvalidEntity;
        OccurrenceResolution  resolution = OccurrenceResolution::Unresolved;
        OccurrenceRole        role       = OccurrenceRole::Reference;
        std::vector<EntityId> candidates;
    };

    struct IndexedEntity
    {
        // Exact, length-delimited identity, not a hash of a binder-local id.
        // Dense EntityIds are valid only in the index that issued them.
        std::string                identity;
        std::string                qualified_name;
        std::string                signature;
        std::string                type;
        std::string                configuration;
        SymbolKind                 kind      = SymbolKind::Variable;
        EntityLinkage              linkage   = EntityLinkage::Local;
        bool                       supported = false;
        std::vector<std::uint32_t> occurrences;
    };

    struct IndexedUnit
    {
        DocumentId                         document = InvalidDocument;
        std::filesystem::path              path;
        std::int64_t                       version = 0;
        std::shared_ptr<const std::string> source;
        std::string                        configuration;
        std::vector<std::string>           command;
        std::vector<DocumentId>            dependencies;
        std::vector<SymbolIndexIssue>      issues;
        std::size_t resolved = 0, unresolved = 0, ambiguous = 0, dependent = 0, unsupported = 0;
    };

    struct SymbolCoverageIssue
    {
        SymbolIndexIssue      kind;
        DocumentId            document = InvalidDocument;
        std::filesystem::path path;
    };

    // Semantic occurrences in a pinned set of registered source/header buffers.
    // Does not silently discover files or infer identity from navigation results.
    // Free function redeclarations use canonical builtin scalar parameter types
    // without names/defaults; overload calls remain ambiguous until full overload
    // resolution exists. Aliases and cv/ref adjustment are explicitly unsupported.
    class ProjectSymbolIndex
    {
      public:
        static std::expected<ProjectSymbolIndex, SymbolIndexError> Build(
            const AnalysisSnapshot& snapshot, SymbolIndexLimits limits = {},
            std::stop_token stop = {});

        std::uint64_t Revision() const noexcept { return m_revision; }

        const std::vector<IndexedEntity>& Entities() const noexcept { return m_entities; }

        const std::vector<IndexedOccurrence>& Occurrences() const noexcept { return m_occurrences; }

        const std::vector<IndexedUnit>& Units() const noexcept { return m_units; }

        const std::vector<SymbolCoverageIssue>& Issues() const noexcept { return m_issues; }

        std::span<const EntityId> Named(std::string_view qualified_name) const;

        const IndexedOccurrence* At(DocumentId document, std::size_t offset) const;

        EntityId EntityFor(DocumentId document, SymbolId symbol) const;

        // Complete only within registered documents and their recorded compile
        // configuration. Never certifies external consumers or C++ in general.
        bool Complete() const noexcept;

        // A prerequisite, not a semantic proof for a particular transformation.
        bool HasCompleteCoverageFor(EntityId entity) const noexcept;

        // Estimated owned index storage; pinned source buffers are accounted
        // separately by AnalysisSnapshot::Memory().
        std::size_t StorageBytes() const noexcept;

      private:
        std::uint64_t                                                       m_revision = 0;
        std::vector<IndexedEntity>                                          m_entities;
        std::vector<IndexedOccurrence>                                      m_occurrences;
        std::vector<IndexedUnit>                                            m_units;
        std::vector<SymbolCoverageIssue>                                    m_issues;
        std::unordered_map<std::string, std::vector<EntityId>>              m_names;
        std::unordered_map<std::uint64_t, EntityId>                         m_local;
        std::unordered_map<DocumentId, std::pair<std::size_t, std::size_t>> m_document_ranges;
    };
} // namespace heimdall
