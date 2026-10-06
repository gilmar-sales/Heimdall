#include <Heimdall/Workspace.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace heimdall
{
    namespace detail
    {
        struct BoundStorage
        {
            std::shared_ptr<const ParseTree> tree;
            SemanticModel model;

            explicit BoundStorage(std::shared_ptr<const ParseTree> syntax)
            : tree(std::move(syntax)), model(Binder::Bind(*tree)) {}
        };

        struct TypedStorage
        {
            std::shared_ptr<const SemanticModel> model;
            TypeModel types;

            explicit TypedStorage(std::shared_ptr<const SemanticModel> semantic)
            : model(std::move(semantic)), types(Typer::Type(*model)) {}
        };

        struct SyntaxStorage
        {
            mutable std::mutex mutex;
            mutable std::shared_ptr<const ParseTree> tree;
            mutable std::shared_ptr<const ParseTree> base;
            Lexer::TextEdit edit;
            mutable AnalysisMetrics metrics;
        };

        struct DocumentAnalysis
        {
            std::filesystem::path path;
            std::shared_ptr<const std::string> source;
            std::int64_t version = 0;
            ParserOptions options;
            std::vector<std::string> include_targets;
            std::shared_ptr<SyntaxStorage> syntax = std::make_shared<SyntaxStorage>();
            mutable std::mutex mutex;
            mutable std::shared_ptr<const SemanticModel> semantic;
            mutable std::shared_ptr<const TypeModel> types;
            mutable std::shared_ptr<const HeaderSummary> summary;
            mutable AnalysisMetrics metrics;
        };

        struct ProjectStorage
        {
            std::mutex mutex;
            std::shared_ptr<const ProjectIndex> index;
            AnalysisMetrics metrics;
        };

        struct WorkspaceState
        {
            WorkspaceState() = default;

            WorkspaceState(const WorkspaceState& other)
            : revision(other.revision), documents(other.documents), paths(other.paths),
                dependencies(other.dependencies), dependents(other.dependents),
                explicit_dependencies(other.explicit_dependencies), compilation(other.compilation),
                project(other.project) {}

            std::uint64_t revision = 0;
            std::vector<std::shared_ptr<const DocumentAnalysis>> documents;
            std::unordered_map<std::string, DocumentId> paths;
            std::vector<std::vector<DocumentId>> dependencies;
            std::vector<std::vector<DocumentId>> dependents;
            std::vector<bool> explicit_dependencies;
            std::shared_ptr<const CompileDatabase> compilation;
            std::shared_ptr<const ProjectIndex> project;
            std::shared_ptr<ProjectStorage> project_cache = std::make_shared<ProjectStorage>();
        };
    }

    namespace
    {
        using Clock = std::chrono::steady_clock;
        std::uint64_t Elapsed(Clock::time_point start)
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - start).count());
        }

        std::string PathKey(const std::filesystem::path& path)
        {
            std::error_code error;
            auto absolute = std::filesystem::absolute(path, error);
            auto key = (error ? path : absolute).lexically_normal().generic_string();
#ifdef _WIN32
            // Windows paths are case-insensitive; use ASCII folding without locale.
            for (auto& c : key)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c += 'a' - 'A';
                }
            }
#endif
            return key;
        }

        std::shared_ptr<detail::DocumentAnalysis> NewDocument(const detail::DocumentAnalysis& old)
        {
            auto document = std::make_shared<detail::DocumentAnalysis>();
            document->path = old.path;
            document->source = old.source;
            document->version = old.version;
            document->options = old.options;
            document->include_targets = old.include_targets;
            return document;
        }

        bool SameOptions(const ParserOptions& a, const ParserOptions& b)
        {
            return a.standard == b.standard && a.Macros() == b.Macros() && a.type_names == b.type_names;
        }

        void InvalidateDependents(detail::WorkspaceState& state, DocumentId changed)
        {
            std::vector<bool> affected(state.documents.size(), false);
            std::vector<DocumentId> queue{changed};
            affected[changed] = true;
            for (std::size_t next = 0; next < queue.size(); ++next)
            {
                for (DocumentId id : state.dependents[queue[next]])
                {
                    if (affected[id] ||!state.documents[id])
                    {
                        continue;
                    }

                    affected[id] = true;
                    queue.push_back(id);
                    // Syntax remains valid; only the semantic dependency changed.
                    auto document = NewDocument(*state.documents[id]);
                    document->syntax = state.documents[id]->syntax;
                    state.documents[id] = std::move(document);
                }
            }
        }

        std::vector<std::string> Includes(std::string_view source, const ParserOptions& options)
        {
            std::vector<std::string> includes;
            // Reuse the syntax preprocessor's directive recognition, including
            // inactive branches and opaque raw literals, rather than a regex.
            const auto preprocessing = Preprocessor(options.Macros()).Process(source);
            for (const auto& directive : preprocessing.directives)
            {
                if (directive.kind != DirectiveKind::Include)
                {
                    continue;
                }

                auto body = source.substr(directive.offset, directive.length);
                std::vector<Token> significant;
                for (const auto& token : Lexer(body).Lex())
                {
                    if (token.kind != TokenKind::Whitespace && token.kind != TokenKind::LineComment &&
                        token.kind != TokenKind::BlockComment)
                    {
                        significant.push_back(token);
                    }
                }

                if (significant.size() < 3 || body.substr(significant[1].offset,
                    significant[1].length) != "include")
                {
                    continue;
                }

                const auto& header = significant[2];
                const auto spelling = body.substr(header.offset, header.length);
                if (header.kind == TokenKind::StringLiteral && spelling.size() >= 2 && spelling.front() == '"')
                {
                    includes.emplace_back(spelling);
                }
                else if (spelling == "<")
                {
                    std::string target = "<";
                    for (std::size_t i = 3; i < significant.size(); ++i)
                    {
                        auto text = body.substr(significant[i].offset, significant[i].length);
                        target.append(text);
                        if (text == ">")
                        {
                            includes.push_back(std::move(target));
                            break;
                        }
                    }
                }
            }

            return includes;
        }

        void ReverseEdges(detail::WorkspaceState& state)
        {
            state.dependents.assign(state.documents.size(), {});
            for (DocumentId id = 0; id < state.dependencies.size(); ++id)
            {
                for (auto dependency : state.dependencies[id])
                {
                    state.dependents[dependency].push_back(id);
                }
            }
        }

        void ResolveIncludes(detail::WorkspaceState& state)
        {
            std::vector<DocumentId> changed;
            for (DocumentId id = 0; id < state.documents.size(); ++id)
            {
                if (!state.documents[id] || state.explicit_dependencies[id])
                {
                    continue;
                }

                const auto& document = *state.documents[id];
                const auto command = state.compilation ? state.compilation->FindOrNearest(document.path) : nullptr;
                std::vector<DocumentId> edges;
                for (const auto& target : document.include_targets)
                {
                    const auto name = target.substr(1, target.size() - 2);
                    std::vector<std::filesystem::path> directories;
                    if (target.front() == '"')
                    {
                        directories.push_back(document.path.parent_path());
                        if (command)
                        {
                            directories.insert(directories.end(), command->quote_directories.begin(),
                                command->quote_directories.end());
                        }
                    }

                    if (command)
                    {
                        directories.insert(directories.end(), command->include_directories.begin(),
                            command->include_directories.end());
                    }

                    for (const auto& directory : directories)
                    {
                        const auto found = state.paths.find(PathKey(directory / name));
                        if (found != state.paths.end())
                        {
                            edges.push_back(found->second);
                            break;
                        }

                        // An earlier header on disk shadows a registered header
                        // in a later search directory, even if not tracked here.
                        std::error_code error;
                        if (std::filesystem::is_regular_file(directory / name, error))
                        {
                            break;
                        }
                    }
                }

                std::sort(edges.begin(), edges.end());
                edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
                if (edges == state.dependencies[id])
                {
                    continue;
                }

                state.dependencies[id] = std::move(edges);
                auto replacement = NewDocument(document);
                replacement->syntax = document.syntax;
                state.documents[id] = std::move(replacement);
                changed.push_back(id);
            }

            ReverseEdges(state);
            for (auto id : changed)
            {
                InvalidateDependents(state, id);
            }
        }

        ParserOptions CommandOptions(const CompileCommand& command)
        {
            ParserOptions options;
            options.standard = command.standard;
            options.predefined_macros = command.defines;
            for (const auto& name : command.undefines)
            {
                options.predefined_macros.erase(name);
            }

            return options;
        }
    }

    AnalysisSnapshot::AnalysisSnapshot(std::shared_ptr<const detail::WorkspaceState> state)
    : m_state(std::move(state)) {}

    std::shared_ptr<const detail::DocumentAnalysis> AnalysisSnapshot::Document(DocumentId document) const
    {
        return Contains(document) ? m_state->documents[document] : nullptr;
    }

    bool AnalysisSnapshot::Contains(DocumentId document) const noexcept
    {
        return m_state && document < m_state->documents.size() && m_state->documents[document];
    }

    std::uint64_t AnalysisSnapshot::Revision() const noexcept
    {
        return m_state ? m_state->revision : 0;
    }

    std::int64_t AnalysisSnapshot::Version(DocumentId document) const noexcept
    {
        return Contains(document) ? m_state->documents[document]->version : -1;
    }

    std::vector<DocumentId> AnalysisSnapshot::Documents() const
    {
        std::vector<DocumentId> ids;
        if (m_state)
        {
            for (DocumentId id = 0; id < m_state->documents.size(); ++id)
            {
                if (Contains(id))
                {
                    ids.push_back(id);
                }
            }
        }

        return ids;
    }

    DocumentId AnalysisSnapshot::Find(const std::filesystem::path& path) const
    {
        if (m_state)
        {
            const auto found = m_state->paths.find(PathKey(path));
            if (found != m_state->paths.end())
            {
                return found->second;
            }
        }

        return InvalidDocument;
    }

    std::shared_ptr<const std::string> AnalysisSnapshot::Source(DocumentId document) const
    {
        auto data = Document(document);
        return data ? data->source : nullptr;
    }

    const std::filesystem::path& AnalysisSnapshot::Path(DocumentId document) const
    {
        static const std::filesystem::path empty;
        return Contains(document) ? m_state->documents[document]->path : empty;
    }

    const ParserOptions& AnalysisSnapshot::Options(DocumentId document) const
    {
        static const ParserOptions empty;
        return Contains(document) ? m_state->documents[document]->options : empty;
    }

    const CompileCommand* AnalysisSnapshot::Command(DocumentId document) const
    {
        return Contains(document) && m_state->compilation
        ? m_state->compilation->FindOrNearest(Path(document)) : nullptr;
    }

    std::span<const DocumentId> AnalysisSnapshot::Dependencies(DocumentId document) const
    {
        return Contains(document) ? std::span<const DocumentId>(m_state->dependencies[document])
        : std::span<const DocumentId> {};
    }

    std::shared_ptr<const ParseTree> AnalysisSnapshot::Syntax(DocumentId document) const
    {
        auto data = Document(document);
        if (!data) return {};
        std::lock_guard lock(data->syntax->mutex);
        if (data->syntax->tree)
        {
            ++data->syntax->metrics.cache_hits;
            return data->syntax->tree;
        }

        auto start = Clock::now();
        const auto& edit = data->syntax->edit;
        ParseReuse reuse{data->syntax->base.get(), edit.offset, edit.old_length, edit.new_length};
        data->syntax->tree = std::make_shared<const ParseTree>(ParseTree::ParseSnapshot(data->source,
            data->options, {}, {}, data->syntax->base ? &reuse : nullptr));
        data->syntax->base.reset();
        ++data->syntax->metrics.parse_count;
        data->syntax->metrics.parse_ns += Elapsed(start);
        return data->syntax->tree;
    }

    std::shared_ptr<const SemanticModel> AnalysisSnapshot::Semantic(DocumentId document) const
    {
        auto data = Document(document);
        if (!data) return {};
        auto syntax = Syntax(document);
        std::lock_guard lock(data->mutex);
        if (data->semantic)
        {
            ++data->metrics.cache_hits;
            return data->semantic;
        }

        auto start = Clock::now();
        auto bound = std::make_shared<detail::BoundStorage>(std::move(syntax));
        data->semantic = std::shared_ptr<const SemanticModel>(bound, &bound->model);
        ++data->metrics.bind_count;
        data->metrics.bind_ns += Elapsed(start);
        return data->semantic;
    }

    std::shared_ptr<const TypeModel> AnalysisSnapshot::Types(DocumentId document) const
    {
        auto data = Document(document);
        if (!data) return {};
        auto semantic = Semantic(document);
        std::lock_guard lock(data->mutex);
        if (data->types)
        {
            ++data->metrics.cache_hits;
            return data->types;
        }

        auto start = Clock::now();
        auto typed = std::make_shared<detail::TypedStorage>(std::move(semantic));
        data->types = std::shared_ptr<const TypeModel>(typed, &typed->types);
        ++data->metrics.type_count;
        data->metrics.type_ns += Elapsed(start);
        return data->types;
    }

    std::shared_ptr<const ProjectIndex> AnalysisSnapshot::Project() const
    {
        if (!m_state)
        {
            return nullptr;
        }

        if (m_state->project)
        {
            return m_state->project;
        }

        std::lock_guard lock(m_state->project_cache->mutex);
        if (!m_state->project_cache->index)
        {
            const auto start = Clock::now();
            std::vector<std::shared_ptr<const HeaderSummary>> summaries;
            for (auto id : Documents())
            {
                auto extension = Path(id).extension().string();
                for (auto& c : extension)
                {
                    if (c >= 'A' && c <= 'Z')
                    {
                        c += 'a' - 'A';
                    }
                }

                if (extension == ".h" || extension == ".hpp" || extension == ".hh" || extension == ".hxx")
                {
                    summaries.push_back(Summary(id));
                }
            }

            m_state->project_cache->index = std::make_shared<const ProjectIndex>(ProjectIndex::FromSummaries(std::move(summaries)));
            ++m_state->project_cache->metrics.project_index_count;
            m_state->project_cache->metrics.project_index_ns += Elapsed(start);
        }

        return m_state->project_cache->index;
    }

    std::shared_ptr<const HeaderSummary> AnalysisSnapshot::Summary(DocumentId document) const
    {
        auto data = Document(document);
        if (!data) return {};
        auto semantic = Semantic(document);
        std::lock_guard lock(data->mutex);
        if (!data->summary)
        {
            data->summary = HeaderSummary::FromModel(*semantic, data->path);
        }

        return data->summary;
    }

    AnalysisMetrics AnalysisSnapshot::Metrics() const
    {
        AnalysisMetrics total;
        if (!m_state)
        {
            return total;
        }

        for (const auto& data : m_state->documents)
        {
            if (!data)
            {
                continue;
            }

            std::scoped_lock lock(data->mutex, data->syntax->mutex);
            total.parse_count += data->syntax->metrics.parse_count;
            total.parse_ns += data->syntax->metrics.parse_ns;
            total.bind_count += data->metrics.bind_count;
            total.bind_ns += data->metrics.bind_ns;
            total.type_count += data->metrics.type_count;
            total.type_ns += data->metrics.type_ns;
            total.cache_hits += data->metrics.cache_hits + data->syntax->metrics.cache_hits;
            if (data->semantic)
            {
                total.semantic_allocations += data->semantic->ArenaAllocations();
            }

            if (data->types)
            {
                total.type_allocations += data->types->ArenaAllocations();
            }
        }

        {
            std::lock_guard lock(m_state->project_cache->mutex);
            total.project_index_count = m_state->project_cache->metrics.project_index_count;
            total.project_index_ns = m_state->project_cache->metrics.project_index_ns;
        }
        return total;
    }

    MemoryBudget AnalysisSnapshot::Memory() const
    {
        MemoryBudget total;
        if (!m_state)
        {
            return total;
        }

        std::unordered_set<const HeaderSummary*> counted;
        for (const auto& data : m_state->documents)
        {
            if (!data)
            {
                continue;
            }

            std::scoped_lock lock(data->mutex, data->syntax->mutex);
            total.source_bytes += data->source ? data->source->capacity() :
            (data->syntax->tree ? data->syntax->tree->Source().size() : 0);
            if (const auto & tree = data->syntax->tree)
            {
                total.syntax_bytes += tree->StorageBytes();
            }

            if (const auto & base = data->syntax->base)
            {
                total.retained_base_bytes += base->StorageBytes();
                auto source = base->SharedSource();
                total.retained_base_bytes += source ? source->capacity() : base->Source().size();
            }

            if (data->semantic)
            {
                total.semantic_arena_bytes += data->semantic->ArenaBytes();
            }

            if (data->types)
            {
                total.type_arena_bytes += data->types->ArenaBytes();
            }

            if (data->summary && counted.insert(data->summary.get()).second)
            {
                total.project_string_bytes += data->summary->PoolBytes();
            }
        }

        std::lock_guard lock(m_state->project_cache->mutex);
        auto project = m_state->project ? m_state->project : m_state->project_cache->index;
        if (project)
        {
            for (const auto& summary : project->Summaries())
            {
                if (counted.insert(summary.get()).second)
                {
                    total.project_string_bytes += summary->PoolBytes();
                }
            }
        }

        return total;
    }

    std::expected<AnalysisSnapshot, WorkspaceError> AnalysisSnapshot::WithSyntax(DocumentId document,
        std::shared_ptr<const ParseTree> tree, ParserOptions options) const
    {
        auto data = Document(document);
        if (!data)
        {
            return std::unexpected(WorkspaceError::InvalidDocument);
        }

        const auto source = data->source ? std::string_view(*data->source) : Syntax(document)->Source();
        if (!tree || tree->Cancelled() || tree->Source() != source || tree->Standard() != options.standard)
        {
            return std::unexpected(WorkspaceError::InvalidSource);
        }

        if (!tree->SharedSource() && data->source && tree->Source().data() != data->source->data())
        {
            return std::unexpected(WorkspaceError::InvalidSource);
        }

        {
            std::lock_guard lock(data->syntax->mutex);
            if (SameOptions(data->options, options))
            {
                if (!data->syntax->tree)
                {
                    data->syntax->tree = std::move(tree);
                    data->syntax->base.reset();
                    return *this;
                }

                if (data->syntax->tree == tree)
                {
                    return *this;
                }
            }
        }
        auto state = std::make_shared<detail::WorkspaceState>(*m_state);
        auto derived = NewDocument(*data);
        derived->options = std::move(options);
        derived->syntax->tree = std::move(tree);
        state->documents[document] = std::move(derived);
        state->project.reset();
        return AnalysisSnapshot(std::move(state));
    }

    AnalysisSnapshot AnalysisSnapshot::FromSyntax(std::shared_ptr<const ParseTree> tree,
        ParserOptions options, std::filesystem::path path)
    {
        if (!tree || tree->Cancelled()) return {};
        auto state = std::make_shared<detail::WorkspaceState>();
        auto document = std::make_shared<detail::DocumentAnalysis>();
        document->path = std::move(path);
        document->source = tree->SharedSource();
        options.standard = tree->Standard();
        document->options = std::move(options);
        document->syntax->tree = std::move(tree);
        state->paths.emplace(PathKey(document->path), 0);
        state->documents.push_back(std::move(document));
        state->dependencies.emplace_back();
        state->dependents.emplace_back();
        state->explicit_dependencies.push_back(false);
        return AnalysisSnapshot(std::move(state));
    }

    Workspace::Workspace() : m_state(std::make_shared<detail::WorkspaceState>()) {}

    AnalysisSnapshot Workspace::Snapshot() const
    {
        std::lock_guard lock(m_mutex);
        return AnalysisSnapshot(m_state);
    }

    std::expected<DocumentId, WorkspaceError> Workspace::Open(
        std::filesystem::path path,
        std::shared_ptr<const std::string> source,
        std::int64_t version,
        ParserOptions options)
    {
        if (!source)
        {
            return std::unexpected(WorkspaceError::InvalidSource);
        }

        std::lock_guard lock(m_mutex);
        auto key = PathKey(path);
        if (const auto found = m_state->paths.find(key); found != m_state->paths.end())
        {
            return std::unexpected(WorkspaceError::StaleVersion);
        }

        if (m_state->documents.size() >= InvalidDocument)
        {
            return std::unexpected(WorkspaceError::IdExhausted);
        }

        auto state = std::make_shared<detail::WorkspaceState>(*m_state);
        auto document = std::make_shared<detail::DocumentAnalysis>();
        document->path = std::move(path);
        document->source = std::move(source);
        document->version = version;
        if (SameOptions(options, ParserOptions{}) && state->compilation)
        {
            if (const auto command = state->compilation->FindOrNearest(document->path))
            {
                options = CommandOptions(*command);
            }
        }

        document->options = std::move(options);
        document->include_targets = Includes(*document->source, document->options);
        const auto id = static_cast<DocumentId>(state->documents.size());
        state->documents.push_back(std::move(document));
        state->dependencies.emplace_back();
        state->dependents.emplace_back();
        state->explicit_dependencies.push_back(false);
        state->paths.emplace(std::move(key), id);
        state->project.reset();
        ResolveIncludes(*state);
        ++state->revision;
        m_state = std::move(state);
        return id;
    }

    std::expected<void, WorkspaceError> Workspace::Update(DocumentId document,
        std::shared_ptr<const std::string> source, std::int64_t version)
    {
        if (!source)
        {
            return std::unexpected(WorkspaceError::InvalidSource);
        }

        std::lock_guard lock(m_mutex);
        if (!AnalysisSnapshot(m_state).Contains(document))
        {
            return std::unexpected(WorkspaceError::InvalidDocument);
        }

        const auto& old = m_state->documents[document];
        if (version <= old->version)
        {
            return std::unexpected(WorkspaceError::StaleVersion);
        }

        auto state = std::make_shared<detail::WorkspaceState>(*m_state);
        auto replacement = NewDocument(*old);
        replacement->version = version;
        bool graph_changed = false;
        if (*source == *old->source)
        {
            // Version-only change: all cached data remains valid.
            replacement->syntax = old->syntax;
            std::lock_guard cache_lock(old->mutex);
            replacement->semantic = old->semantic;
            replacement->types = old->types;
            replacement->summary = old->summary;
            replacement->metrics = old->metrics;
            state->project_cache = m_state->project_cache;
        }
        else
        {
            {
                std::lock_guard cache_lock(old->syntax->mutex);
                replacement->syntax->base = old->syntax->tree;
            }
            if (replacement->syntax->base)
            {
                // A single edit hull is conservative for arbitrary whole-buffer
                // updates. The grammar already knows which top-level items it
                // can reuse safely (including changes in known type names).
                const auto& before = *old->source;
                const auto& after = *source;
                std::size_t prefix = 0;
                while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix])
                {
                    ++prefix;
                }

                std::size_t suffix = 0;
                while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
                    before[before.size() - suffix - 1] == after[after.size() - suffix - 1])
                {
                    ++suffix;
                }

                replacement->syntax->edit = {prefix, before.size() - prefix - suffix,
                    after.size() - prefix - suffix};
            }

            replacement->source = std::move(source);
            state->project.reset();
            replacement->include_targets = Includes(*replacement->source, replacement->options);
            graph_changed = replacement->include_targets != old->include_targets;
            InvalidateDependents(*state, document);
        }

        state->documents[document] = std::move(replacement);
        if (graph_changed)
        {
            ResolveIncludes(*state);
        }

        ++state->revision;
        m_state = std::move(state);
        return {};
    }

    std::expected<void, WorkspaceError> Workspace::Close(DocumentId document)
    {
        std::lock_guard lock(m_mutex);
        if (!AnalysisSnapshot(m_state).Contains(document))
        {
            return std::unexpected(WorkspaceError::InvalidDocument);
        }

        auto state = std::make_shared<detail::WorkspaceState>(*m_state);
        InvalidateDependents(*state, document);
        state->paths.erase(PathKey(state->documents[document]->path));
        state->documents[document].reset();
        state->project.reset();
        state->dependencies[document].clear();
        for (auto& edges : state->dependencies)
        {
            std::erase(edges, document);
        }

        ResolveIncludes(*state);
        ++state->revision;
        m_state = std::move(state);
        return {};
    }

    std::expected<void, WorkspaceError> Workspace::SetDependencies(DocumentId document,
        std::span<const DocumentId> dependencies)
    {
        std::lock_guard lock(m_mutex);
        AnalysisSnapshot snapshot(m_state);
        if (!snapshot.Contains(document))
        {
            return std::unexpected(WorkspaceError::InvalidDocument);
        }

        for (auto id : dependencies)
        {
            if (!snapshot.Contains(id))
            {
                return std::unexpected(WorkspaceError::InvalidDocument);
            }
        }

        std::vector<DocumentId> edges(dependencies.begin(), dependencies.end());
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        if (edges == m_state->dependencies[document] && m_state->explicit_dependencies[document]) return {};
        auto state = std::make_shared<detail::WorkspaceState>(*m_state);
        state->dependencies[document] = std::move(edges);
        state->explicit_dependencies[document] = true;
        ReverseEdges(*state);
        auto replacement = NewDocument(*state->documents[document]);
        replacement->syntax = state->documents[document]->syntax;
        state->documents[document] = std::move(replacement);
        InvalidateDependents(*state, document);
        ++state->revision;
        m_state = std::move(state);
        return {};
    }

    std::expected<void,
        WorkspaceError> Workspace::SetOptions(DocumentId document, ParserOptions options)
    {
        std::lock_guard lock(m_mutex);
        if (!AnalysisSnapshot(m_state).Contains(document))
        {
            return std::unexpected(WorkspaceError::InvalidDocument);
        }

        if (SameOptions(m_state->documents[document]->options, options)) return {};
        auto state = std::make_shared<detail::WorkspaceState>(*m_state);
        auto replacement = NewDocument(*state->documents[document]);
        replacement->options = std::move(options);
        replacement->include_targets = Includes(*replacement->source, replacement->options);
        state->documents[document] = std::move(replacement);
        InvalidateDependents(*state, document);
        state->project.reset();
        ResolveIncludes(*state);
        ++state->revision;
        m_state = std::move(state);
        return {};
    }

    void Workspace::SetCompilationDatabase(std::shared_ptr<const CompileDatabase> database)
    {
        std::lock_guard lock(m_mutex);
        if (m_state->compilation == database)
        {
            return;
        }

        auto state = std::make_shared<detail::WorkspaceState>(*m_state);
        state->compilation = std::move(database);
        state->project.reset();
        for (auto& document : state->documents)
        {
            if (!document)
            {
                continue;
            }

            auto replacement = NewDocument(*document);
            replacement->options = {};
            if (state->compilation)
            {
                if (auto command = state->compilation->FindOrNearest(document->path))
                {
                    replacement->options = CommandOptions(*command);
                }
            }

            replacement->include_targets = Includes(*replacement->source, replacement->options);
            document = std::move(replacement);
        }

        ResolveIncludes(*state);
        ++state->revision;
        m_state = std::move(state);
    }

    void Workspace::SetProjectIndex(std::shared_ptr<const ProjectIndex> index)
    {
        std::lock_guard lock(m_mutex);
        if (m_state->project == index)
        {
            return;
        }

        auto state = std::make_shared<detail::WorkspaceState>(*m_state);
        state->project = std::move(index);
        for (auto& document : state->documents)
        {
            if (!document)
            {
                continue;
            }

            auto replacement = NewDocument(*document);
            replacement->syntax = document->syntax;
            document = std::move(replacement);
        }

        ++state->revision;
        m_state = std::move(state);
    }
}
