#include <Heimdall/Navigation.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace heimdall
{

    namespace
    {
        constexpr std::size_t kNone = static_cast<std::size_t>(-1);
        constexpr std::size_t kMinAmbiguousScopes = 2;
        constexpr int kDoubleAngleCount = 2;
        constexpr std::size_t kMaxPlacementWalkDepth = 64;
        constexpr std::size_t kMinQualifiedParts = 2;
        constexpr std::size_t kMinScopeGroups = 2;
        constexpr std::size_t kMaxBaseDepth = 4;

        using Path = std::vector<std::string>;

        std::string PathKey(const Path &path)
        {
            std::string key;
            for (const auto &element: path)
            {
                key += element;
                key += '\x1f';
            }

            return key;
        }

        std::string Joined(const Path &path, std::string_view name)
        {
            std::string out;
            for (const auto &element: path)
            {
                out += element;
                out += "::";
            }

            out += name;
            return out;
        }

        bool IsTrivia(TokenKind kind)
        {
            return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
                kind == TokenKind::BlockComment;
        }

        // Declared symbol of the buffer being analyzed.
        struct Decl
        {
            std::string name;
            Path scope;
            std::size_t offset = 0;
            std::size_t length = 0;
            CompletionKind kind = CompletionKind::Variable;
            bool is_definition = false;
            std::uint32_t param_count = 0;
            // Block-local names are only visible in [local_begin, local_end).
            bool local = false;
            std::size_t local_begin = 0;
            std::size_t local_end = 0;
            // Introduced by `using a::b;` (points at the used entity's name).
            bool via_using = false;
        };

        struct RecordInfo
        {
            Path path;
            std::vector<std::string> bases;
            std::size_t begin = 0;
            std::size_t end = 0;
        };

        struct ScopeRange
        {
            std::size_t begin = 0;
            std::size_t end = 0;
            Path path;
        };

        struct UsingDecl
        {
            Path scope;
            Path target; // `a::b` for `using a::b;` (last element is the name)
            std::size_t offset = 0;
            std::size_t active_begin = 0;
            std::size_t active_end = 0;
        };

        // One external (header index) member.
        struct ExternalRef
        {
            std::size_t scope = 0;
            std::size_t member = 0;
        };

        struct Candidate
        {
            Path scope;
            std::string name;
            CompletionKind kind = CompletionKind::Variable;
            // Decl index (>= 0) or external reference (decl == kNone).
            std::size_t decl = kNone;
            ExternalRef external;
            bool is_definition = false;
            std::uint32_t param_count = 0;
        };

        class Model
        {
        public:
            Model(const ParseTree &tree, const ScopeIndex *external): m_tree(tree), m_external(external)
            {
                BuildPaths();
                CollectScopes();
                CollectDecls();
                BuildExternal();
            }

            const ParseTree &Tree() const noexcept { return m_tree; }
            const std::vector<Decl> &Decls() const noexcept { return m_decls; }
            const std::vector<UsingDirective> &Directives() const noexcept { return m_directives; }

            // ---- token helpers -------------------------------------------------
            std::string_view Text(std::size_t token) const
            {
                return m_tree.Text(m_tree.Tokens()[token]);
            }

            std::size_t PrevSig(std::size_t token) const
            {
                while (token > 0)
                {
                    --token;
                    if (!IsTrivia(m_tree.Tokens()[token].kind))
                    {
                        return token;
                    }
                }

                return kNone;
            }

            std::size_t NextSig(std::size_t token) const
            {
                for (++token; token < m_tree.Tokens().size(); ++token)
                {
                    if (!IsTrivia(m_tree.Tokens()[token].kind))
                    {
                        return token;
                    }
                }

                return kNone;
            }

            // Identifier token under (or immediately before) `offset`.
            std::size_t IdentifierAt(std::size_t offset) const
            {
                const auto &tokens = m_tree.Tokens();
                auto it = std::upper_bound(tokens.begin(), tokens.end(), offset,
                    [](std::size_t value, const Token &token) { return value < token.offset; });
                if (it == tokens.begin())
                {
                    return kNone;
                }

                std::size_t index = static_cast<std::size_t>(it - tokens.begin()) - 1;
                if (tokens[index].kind == TokenKind::Identifier &&
                    offset <= tokens[index].offset + tokens[index].length)
                {
                    return index;
                }

                // Cursor right after a previous token (`foo|(`): handled above; a
                // cursor on whitespace/punctuation resolves nothing.
                return kNone;
            }

            std::size_t NodeBegin(std::size_t node) const
            {
                const auto &grammar = m_tree.Nodes()[node];
                if (node == ParseTree::RootNode)
                {
                    return 0;
                }

                return grammar.first_token < m_tree.Tokens().size() ? m_tree.Tokens()[grammar.first_token].offset
                                                                     : m_tree.Source().size();
            }

            std::size_t NodeEnd(std::size_t node) const
            {
                const auto &grammar = m_tree.Nodes()[node];
                if (node == ParseTree::RootNode || grammar.token_count == 0)
                {
                    return node == ParseTree::RootNode ? m_tree.Source().size() : NodeBegin(node);
                }

                const std::size_t last =
                    std::min<std::size_t>(grammar.first_token + grammar.token_count, m_tree.Tokens().size()) - 1;
                return m_tree.Tokens()[last].offset + m_tree.Tokens()[last].length;
            }

            // ---- scope queries ---------------------------------------------------
            // Innermost named scope path (namespaces, records, qualified function
            // bodies) containing `offset`.
            Path ScopeAt(std::size_t offset) const
            {
                Path best;
                std::size_t span = kNone;
                for (const auto &range: m_ranges)
                {
                    if (range.begin <= offset && offset <= range.end && range.end - range.begin < span)
                    {
                        best = range.path;
                        span = range.end - range.begin;
                    }
                }

                return best;
            }

            bool KnownScope(const Path &path) const
            {
                return path.empty() || m_known_scopes.contains(PathKey(path));
            }

            bool IsRecordScope(const Path &path) const { return m_record_scopes.contains(PathKey(path)); }

            const std::vector<RecordInfo> &Records() const noexcept { return m_records; }

            // Members named `name` declared directly in `scope`: buffer first, then
            // the header index. Block locals are never members.
            void Members(const Path &scope, std::string_view name, std::vector<Candidate> &out) const
            {
                const std::string key = PathKey(scope);
                if (const auto found = m_by_name.find(std::string(name)); found != m_by_name.end())
                {
                    for (const std::size_t index: found->second)
                    {
                        const Decl &decl = m_decls[index];
                        if (!decl.local && PathKey(decl.scope) == key)
                        {
                            out.push_back(FromDecl(index));
                        }
                    }
                }

                if (m_external != nullptr)
                {
                    if (const auto found = m_external_by_name.find(std::string(name));
                        found != m_external_by_name.end())
                    {
                        for (const ExternalRef &ref: found->second)
                        {
                            if (m_external_keys[ref.scope] == key)
                            {
                                out.push_back(FromExternal(ref));
                            }
                        }
                    }
                }
            }

            // Scopes whose members include `name` (any depth), for `x.name`.
            void MemberCandidates(std::string_view name, std::vector<Candidate> &out) const
            {
                if (const auto found = m_by_name.find(std::string(name)); found != m_by_name.end())
                {
                    for (const std::size_t index: found->second)
                    {
                        const Decl &decl = m_decls[index];
                        if (!decl.local && !decl.scope.empty() && IsRecordScope(decl.scope) &&
                            decl.kind != CompletionKind::Type)
                        {
                            out.push_back(FromDecl(index));
                        }
                    }
                }

                if (m_external != nullptr)
                {
                    if (const auto found = m_external_by_name.find(std::string(name));
                        found != m_external_by_name.end())
                    {
                        for (const ExternalRef &ref: found->second)
                        {
                            const IndexedScope &scope = (*m_external)[ref.scope];
                            if (scope.kind == CompletionKind::Type && !scope.path.empty())
                            {
                                out.push_back(FromExternal(ref));
                            }
                        }
                    }
                }
            }

            // Names with at least two declarations in distinct scopes: the only ones
            // that can be ambiguous.
            bool MaybeAmbiguous(std::string_view name) const
            {
                std::unordered_set<std::string> scopes;
                if (const auto found = m_by_name.find(std::string(name)); found != m_by_name.end())
                {
                    for (const std::size_t index: found->second)
                    {
                        if (!m_decls[index].local)
                        {
                            scopes.insert(PathKey(m_decls[index].scope));
                        }
                    }
                }

                if (m_external != nullptr)
                {
                    if (const auto found = m_external_by_name.find(std::string(name));
                        found != m_external_by_name.end())
                    {
                        for (const ExternalRef &ref: found->second)
                        {
                            scopes.insert(m_external_keys[ref.scope]);
                        }
                    }
                }

                return scopes.size() >= kMinAmbiguousScopes;
            }

            // Resolve a using-directive/qualifier written relative to `from`.
            std::optional<Path> ResolveNamespace(const Path &written, const Path &from) const
            {
                for (std::size_t keep = from.size() + 1; keep > 0; --keep)
                {
                    Path candidate(from.begin(), from.begin() + static_cast<std::ptrdiff_t>(keep - 1));
                    candidate.insert(candidate.end(), written.begin(), written.end());
                    if (KnownScope(candidate))
                    {
                        return candidate;
                    }
                }

                return std::nullopt;
            }

            // Names visible through using-directives at `pos`, per lookup level.
            // Returns, for level `level`, the namespaces whose members become
            // visible there (transitively through directives inside them).
            std::vector<Path> NominatedAt(const Path &level, std::size_t pos) const
            {
                std::vector<Path> result;
                std::unordered_set<std::string> seen;
                for (const UsingDirective &directive: m_directives)
                {
                    if (!(directive.active_begin <= pos && pos < directive.active_end))
                    {
                        continue;
                    }

                    const auto target = ResolveNamespace(directive.target, directive.scope);
                    if (!target)
                    {
                        continue;
                    }

                    // The nominated namespace appears in the nearest enclosing
                    // namespace that contains both the directive and the target.
                    std::size_t common = 0;
                    while (common < directive.scope.size() && common < target->size() &&
                        directive.scope[common] == (*target)[common])
                    {
                        ++common;
                    }

                    if (level.size() != common ||
                        !std::equal(level.begin(), level.end(), target->begin()))
                    {
                        continue;
                    }

                    AddClosure(*target, result, seen);
                }

                return result;
            }

            // using-declarations active at `pos` introducing `name` into `level`.
            void UsingDeclared(const Path &level, std::string_view name, std::size_t pos,
                std::vector<Candidate> &out) const
            {
                for (const UsingDecl &using_decl: m_using_decls)
                {
                    if (using_decl.target.empty() || using_decl.target.back() != name ||
                        !(using_decl.active_begin <= pos && pos < using_decl.active_end) ||
                        using_decl.scope != level)
                    {
                        continue;
                    }

                    Path qualifier(using_decl.target.begin(), using_decl.target.end() - 1);
                    const auto resolved = ResolveNamespace(qualifier, using_decl.scope);
                    if (resolved)
                    {
                        Members(*resolved, name, out);
                    }
                }
            }

            // Direct base record paths of `record`.
            std::vector<Path> BaseScopes(const Path &record) const
            {
                std::vector<Path> out;
                for (const RecordInfo &info: m_records)
                {
                    if (info.path != record)
                    {
                        continue;
                    }

                    for (const std::string &base: info.bases)
                    {
                        // Same namespace first, then any record with that name.
                        Path preferred(record.begin(), record.end() - 1);
                        preferred.push_back(base);
                        if (IsRecordScope(preferred))
                        {
                            out.push_back(std::move(preferred));
                            continue;
                        }

                        for (const RecordInfo &other: m_records)
                        {
                            if (!other.path.empty() && other.path.back() == base)
                            {
                                out.push_back(other.path);
                                break;
                            }
                        }
                    }
                }

                return out;
            }

            // Records derived (transitively) from a record named `base`.
            std::vector<const RecordInfo *> Derived(std::string_view base) const
            {
                std::vector<const RecordInfo *> out;
                std::unordered_set<std::string> frontier{std::string(base)};
                bool grew = true;
                std::unordered_set<const RecordInfo *> taken;
                while (grew)
                {
                    grew = false;
                    for (const RecordInfo &info: m_records)
                    {
                        if (taken.contains(&info) || info.path.empty())
                        {
                            continue;
                        }

                        for (const std::string &name: info.bases)
                        {
                            if (frontier.contains(name))
                            {
                                taken.insert(&info);
                                out.push_back(&info);
                                frontier.insert(info.path.back());
                                grew = true;
                                break;
                            }
                        }
                    }
                }

                return out;
            }

            Candidate FromDecl(std::size_t index) const
            {
                const Decl &decl = m_decls[index];
                Candidate candidate;
                candidate.scope = decl.scope;
                candidate.name = decl.name;
                candidate.kind = decl.kind;
                candidate.decl = index;
                candidate.is_definition = decl.is_definition;
                candidate.param_count = decl.param_count;
                return candidate;
            }

            Candidate FromExternal(ExternalRef ref) const
            {
                const IndexedScope &scope = (*m_external)[ref.scope];
                const CompletionItem &item = scope.members[ref.member];
                Candidate candidate;
                candidate.scope = scope.path;
                candidate.name = item.label;
                candidate.kind = item.kind;
                candidate.external = ref;
                candidate.is_definition = item.is_definition;
                candidate.param_count = item.param_count;
                return candidate;
            }

            NavTarget ToTarget(const Candidate &candidate) const
            {
                NavTarget target;
                target.name = candidate.name;
                target.scope = candidate.scope;
                target.kind = candidate.kind;
                target.is_definition = candidate.is_definition;
                target.param_count = candidate.param_count;
                if (candidate.decl != kNone)
                {
                    const Decl &decl = m_decls[candidate.decl];
                    target.file = -1;
                    target.offset = decl.offset;
                    target.length = decl.length;
                }
                else
                {
                    const CompletionItem &item = (*m_external)[candidate.external.scope].members[candidate.external.member];
                    target.file = item.file;
                    target.offset = item.offset;
                    target.length = item.label.size();
                }

                return target;
            }

            const std::vector<UsingDecl> &UsingDecls() const noexcept { return m_using_decls; }

        private:
            void AddClosure(const Path &ns, std::vector<Path> &out, std::unordered_set<std::string> &seen) const
            {
                if (!seen.insert(PathKey(ns)).second)
                {
                    return;
                }

                out.push_back(ns);
                // Directives written inside `ns` make their targets visible with it.
                for (const UsingDirective &inner: m_directives)
                {
                    if (inner.scope != ns)
                    {
                        continue;
                    }

                    if (const auto target = ResolveNamespace(inner.target, inner.scope))
                    {
                        AddClosure(*target, out, seen);
                    }
                }
            }

            void BuildPaths()
            {
                const auto &nodes = m_tree.Nodes();
                m_node_path.assign(nodes.size(), Path{});
                m_node_elements.assign(nodes.size(), Path{});
                for (std::size_t n = 1; n < nodes.size(); ++n)
                {
                    const GrammarKind kind = nodes[n].kind;
                    Path path = nodes[n].parent < n ? m_node_path[nodes[n].parent] : Path{};
                    if (kind == GrammarKind::NamespaceDefinition || kind == GrammarKind::RecordDefinition)
                    {
                        const ScopeName name = ParseScopeName(n);
                        m_scope_name.emplace(n, name);
                        if (!name.visible_elements.empty())
                        {
                            path.insert(path.end(), name.visible_elements.begin(), name.visible_elements.end());
                        }
                    }

                    m_node_path[n] = std::move(path);
                }
            }

            struct ScopeName
            {
                // Elements the scope adds to the qualified path (empty for
                // anonymous and inline namespaces).
                Path visible_elements;
                // All written elements (`a::b` -> {a, b}); last is the declared name.
                Path written;
                std::size_t name_token = kNone;
                bool has_body = false;
                bool is_enum = false;
                bool is_scoped_enum = false;
                bool is_inline = false;
                std::vector<std::string> bases;
            };

            ScopeName ParseScopeName(std::size_t node) const
            {
                ScopeName out;
                const auto &grammar = m_tree.Nodes()[node];
                const auto &tokens = m_tree.Tokens();
                const std::size_t end = std::min<std::size_t>(grammar.first_token + grammar.token_count, tokens.size());
                std::size_t t = grammar.first_token;
                std::size_t body = kNone;
                for (std::size_t k = t; k < end; ++k)
                {
                    if (!IsTrivia(tokens[k].kind) && tokens[k].kind == TokenKind::Punctuation && Text(k) == "{")
                    {
                        body = k;
                        break;
                    }
                }

                out.has_body = body != kNone;
                const std::size_t limit = body != kNone ? body : end;
                const bool is_namespace = m_tree.Nodes()[node].kind == GrammarKind::NamespaceDefinition;
                std::size_t k = t;
                for (; k < limit; ++k)
                {
                    if (tokens[k].kind != TokenKind::Identifier)
                    {
                        continue;
                    }

                    const std::string_view word = Text(k);
                    if (is_namespace ? word == "namespace"
                                     : (word == "class" || word == "struct" || word == "union" || word == "enum"))
                    {
                        if (word == "enum")
                        {
                            out.is_enum = true;
                            const std::size_t after = NextSig(k);
                            if (after != kNone && after < limit && (Text(after) == "class" || Text(after) == "struct"))
                            {
                                out.is_scoped_enum = true;
                                k = after;
                            }
                        }

                        break;
                    }

                    if (is_namespace && word == "inline")
                    {
                        out.is_inline = true;
                    }
                }

                // Name: skip attributes and alignas(...), then ident (:: ident)*.
                std::size_t cursor = k;
                while (true)
                {
                    cursor = NextSig(cursor);
                    if (cursor == kNone || cursor >= limit)
                    {
                        break;
                    }

                    const std::string_view text = Text(cursor);
                    if (text == "[")
                    {
                        int depth = 0;
                        for (; cursor < limit; ++cursor)
                        {
                            const std::string_view piece = Text(cursor);
                            if (piece == "[")
                            {
                                ++depth;
                            }
                            else if (piece == "]" && --depth == 0)
                            {
                                break;
                            }
                        }

                        continue;
                    }

                    if (tokens[cursor].kind == TokenKind::Identifier && (text == "alignas" || text == "inline"))
                    {
                        if (text == "alignas")
                        {
                            const std::size_t open = NextSig(cursor);
                            if (open != kNone && open < limit && Text(open) == "(")
                            {
                                int depth = 0;
                                for (cursor = open; cursor < limit; ++cursor)
                                {
                                    const std::string_view piece = Text(cursor);
                                    if (piece == "(")
                                    {
                                        ++depth;
                                    }
                                    else if (piece == ")" && --depth == 0)
                                    {
                                        break;
                                    }
                                }
                            }
                        }

                        continue;
                    }

                    if (tokens[cursor].kind != TokenKind::Identifier)
                    {
                        break;
                    }

                    // Qualified name chain.
                    while (cursor != kNone && cursor < limit && tokens[cursor].kind == TokenKind::Identifier)
                    {
                        const std::string_view word = Text(cursor);
                        if (word == "final" && out.written.empty() == false)
                        {
                            break;
                        }

                        out.written.emplace_back(word);
                        out.name_token = cursor;
                        const std::size_t sep = NextSig(cursor);
                        if (sep != kNone && sep < limit && Text(sep) == "::")
                        {
                            const std::size_t next = NextSig(sep);
                            if (next != kNone && next < limit && tokens[next].kind == TokenKind::Identifier)
                            {
                                cursor = next;
                                continue;
                            }
                        }

                        break;
                    }

                    break;
                }

                if (!is_namespace && !out.is_enum && !out.written.empty() && out.name_token != kNone)
                {
                    // Base clause: `: public A, private ns::B<int>`.
                    std::size_t colon = NextSig(out.name_token);
                    if (colon != kNone && colon < limit && Text(colon) == "final")
                    {
                        colon = NextSig(colon);
                    }

                    if (colon != kNone && colon < limit && Text(colon) == ":")
                    {
                        std::string last;
                        int angle = 0;
                        for (std::size_t b = colon + 1; b < limit; ++b)
                        {
                            if (IsTrivia(tokens[b].kind))
                            {
                                continue;
                            }

                            const std::string_view piece = Text(b);
                            if (piece == "<")
                            {
                                ++angle;
                            }
                            else if (piece == ">")
                            {
                                --angle;
                            }
                            else if (piece == ">>")
                            {
                                angle -= kDoubleAngleCount;
                            }
                            else if (angle <= 0 && piece == ",")
                            {
                                if (!last.empty())
                                {
                                    out.bases.push_back(last);
                                }

                                last.clear();
                            }
                            else if (angle <= 0 && tokens[b].kind == TokenKind::Identifier && piece != "public" &&
                                piece != "private" && piece != "protected" && piece != "virtual")
                            {
                                last = std::string(piece);
                            }
                        }

                        if (!last.empty())
                        {
                            out.bases.push_back(last);
                        }
                    }
                }

                if (!out.is_inline && !out.written.empty())
                {
                    out.visible_elements = out.written;
                }

                // `inline namespace v1`: members belong to the parent scope.
                if (is_namespace && out.is_inline)
                {
                    out.visible_elements.clear();
                }

                return out;
            }

            void CollectScopes()
            {
                m_known_scopes.insert(std::string());
                const auto &nodes = m_tree.Nodes();
                for (std::size_t n = 1; n < nodes.size(); ++n)
                {
                    const GrammarKind kind = nodes[n].kind;
                    if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
                    {
                        continue;
                    }

                    const ScopeName &name = m_scope_name.at(n);
                    if (!name.has_body)
                    {
                        continue;
                    }

                    const Path &path = m_node_path[n];
                    // Every prefix is a known scope (`a::b` declares `a` too).
                    for (std::size_t length = 1; length <= path.size(); ++length)
                    {
                        m_known_scopes.insert(PathKey(Path(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(length))));
                    }

                    m_ranges.push_back({NodeBegin(n), NodeEnd(n), path});
                    if (kind == GrammarKind::RecordDefinition && !name.written.empty())
                    {
                        m_record_scopes.insert(PathKey(path));
                        RecordInfo info;
                        info.path = path;
                        info.bases = name.bases;
                        info.begin = NodeBegin(n);
                        info.end = NodeEnd(n);
                        m_records.push_back(std::move(info));
                    }
                }
            }

            // Boundary classification for a DeclaredName.
            struct Placement
            {
                bool ignore = false;
                bool local = false;
                std::size_t boundary = kNone;
                std::size_t container = 0;
                bool param = false;
                bool enumerator = false;
            };

            Placement Place(std::size_t name_node) const
            {
                const auto &nodes = m_tree.Nodes();
                Placement placement;
                std::size_t previous = name_node;
                std::size_t current = nodes[name_node].parent;
                for (std::size_t depth = 0; depth < kMaxPlacementWalkDepth && current < nodes.size(); ++depth)
                {
                    const GrammarKind kind = nodes[current].kind;
                    switch (kind)
                    {
                        case GrammarKind::ParameterDeclaration:
                            placement.param = true;
                            break;
                        case GrammarKind::Enumerator:
                            placement.enumerator = true;
                            break;
                        case GrammarKind::FunctionDefinition:
                            if (placement.param)
                            {
                                placement.local = true;
                                placement.boundary = current;
                                return placement;
                            }

                            break;
                        case GrammarKind::FunctionDeclaration:
                            if (placement.param)
                            {
                                placement.ignore = true;
                                return placement;
                            }

                            break;
                        case GrammarKind::LambdaExpression:
                            if (placement.param)
                            {
                                placement.local = true;
                                placement.boundary = current;
                                return placement;
                            }

                            break;
                        case GrammarKind::CompoundStatement:
                        case GrammarKind::IfStatement:
                        case GrammarKind::LoopStatement:
                        case GrammarKind::SwitchStatement:
                        case GrammarKind::TryStatement:
                            placement.local = true;
                            placement.boundary = current;
                            return placement;
                        case GrammarKind::NamespaceDefinition:
                        case GrammarKind::RecordDefinition:
                        case GrammarKind::TranslationUnit:
                            placement.container = current;
                            return placement;
                        default:
                            break;
                    }

                    previous = current;
                    current = nodes[current].parent;
                }

                (void)previous;
                return placement;
            }

            void AddDecl(Decl decl) { m_decls.push_back(std::move(decl)); }

            void CollectDecls()
            {
                const auto &nodes = m_tree.Nodes();
                const auto &tokens = m_tree.Tokens();
                for (std::size_t n = 1; n < nodes.size(); ++n)
                {
                    const GrammarNode &grammar = nodes[n];
                    if (grammar.kind == GrammarKind::DeclaredName)
                    {
                        CollectDeclaredName(n);
                    }
                    else if (grammar.kind == GrammarKind::NamespaceDefinition)
                    {
                        const ScopeName &name = m_scope_name.at(n);
                        if (name.written.empty() || name.name_token == kNone)
                        {
                            continue;
                        }

                        Decl decl;
                        decl.name = name.written.back();
                        decl.scope = nodes[n].parent < n ? m_node_path[nodes[n].parent] : Path{};
                        decl.scope.insert(decl.scope.end(), name.written.begin(), name.written.end() - 1);
                        decl.offset = tokens[name.name_token].offset;
                        decl.length = tokens[name.name_token].length;
                        decl.kind = CompletionKind::Namespace;
                        decl.is_definition = true;
                        AddDecl(std::move(decl));
                    }
                    else if (grammar.kind == GrammarKind::RecordDefinition)
                    {
                        const ScopeName &name = m_scope_name.at(n);
                        if (name.written.empty() || name.name_token == kNone || !name.has_body)
                        {
                            continue;
                        }

                        Decl decl;
                        decl.name = name.written.back();
                        decl.scope = nodes[n].parent < n ? m_node_path[nodes[n].parent] : Path{};
                        decl.scope.insert(decl.scope.end(), name.written.begin(), name.written.end() - 1);
                        decl.offset = tokens[name.name_token].offset;
                        decl.length = tokens[name.name_token].length;
                        decl.kind = CompletionKind::Type;
                        decl.is_definition = true;
                        AddDecl(std::move(decl));
                    }
                    else if (grammar.kind == GrammarKind::UsingDeclaration)
                    {
                        CollectUsing(n);
                    }
                }

                for (std::size_t i = 0; i < m_decls.size(); ++i)
                {
                    m_by_name[m_decls[i].name].push_back(i);
                }
            }

            void CollectDeclaredName(std::size_t n)
            {
                const auto &nodes = m_tree.Nodes();
                const auto &tokens = m_tree.Tokens();
                const std::size_t token = nodes[n].first_token;
                if (token >= tokens.size() || tokens[token].kind != TokenKind::Identifier)
                {
                    return;
                }

                const Placement placement = Place(n);
                if (placement.ignore)
                {
                    return;
                }

                Decl decl;
                decl.name = std::string(Text(token));
                decl.offset = tokens[token].offset;
                decl.length = tokens[token].length;
                decl.is_definition = true;

                // Function-ness and qualifiers come from the owning Declarator.
                const std::size_t declarator = nodes[n].parent;
                Path qualifiers;
                if (declarator < nodes.size() && nodes[declarator].kind == GrammarKind::Declarator)
                {
                    bool suffix = false;
                    std::size_t suffix_node = kNone;
                    for (const std::size_t child: m_tree.Children(declarator))
                    {
                        const GrammarKind kind = nodes[child].kind;
                        if (kind == GrammarKind::NestedNameSpecifier)
                        {
                            const std::size_t first = nodes[child].first_token;
                            if (first < tokens.size() && tokens[first].kind == TokenKind::Identifier)
                            {
                                qualifiers.emplace_back(Text(first));
                            }
                        }
                        else if (kind == GrammarKind::FunctionSuffix && !suffix)
                        {
                            suffix = true;
                            suffix_node = child;
                        }
                    }

                    if (suffix && !placement.param)
                    {
                        decl.kind = CompletionKind::Function;
                        const std::size_t owner = nodes[declarator].parent;
                        decl.is_definition =
                            owner < nodes.size() && nodes[owner].kind == GrammarKind::FunctionDefinition;
                        std::uint32_t count = 0;
                        std::size_t only_void = kNone;
                        for (const std::size_t param: m_tree.Children(suffix_node))
                        {
                            if (nodes[param].kind == GrammarKind::ParameterDeclaration)
                            {
                                ++count;
                                only_void = (nodes[param].token_count == 1 &&
                                                Text(nodes[param].first_token) == "void")
                                    ? param
                                    : kNone;
                            }
                        }

                        if (count == 1 && only_void != kNone)
                        {
                            count = 0;
                        }

                        decl.param_count = count;
                    }
                }

                if (placement.local)
                {
                    decl.local = true;
                    decl.local_begin = placement.param ? NodeBegin(placement.boundary) : decl.offset;
                    decl.local_end = NodeEnd(placement.boundary);
                    AddDecl(std::move(decl));
                    return;
                }

                const std::size_t container = placement.container;
                Path scope = m_node_path[container];
                scope.insert(scope.end(), qualifiers.begin(), qualifiers.end());
                if (placement.enumerator)
                {
                    // Enumerators live in the enum's scope; unscoped enums also
                    // leak them into the enclosing scope.
                    decl.kind = CompletionKind::Variable;
                    decl.scope = scope;
                    const auto found = m_scope_name.find(container);
                    const bool scoped = found != m_scope_name.end() && found->second.is_scoped_enum;
                    if (!scoped && !scope.empty())
                    {
                        Decl outer = decl;
                        outer.scope = Path(scope.begin(), scope.end() - 1);
                        AddDecl(std::move(outer));
                    }
                }
                else
                {
                    decl.scope = scope;
                }

                // Out-of-line member bodies see the class scope.
                if (decl.kind == CompletionKind::Function && !qualifiers.empty() && decl.is_definition)
                {
                    const std::size_t owner = nodes[nodes[n].parent].parent;
                    if (owner < nodes.size() && nodes[owner].kind == GrammarKind::FunctionDefinition)
                    {
                        m_ranges.push_back({NodeBegin(owner), NodeEnd(owner), decl.scope});
                    }
                }

                AddDecl(std::move(decl));
            }

            void CollectUsing(std::size_t n)
            {
                const auto &nodes = m_tree.Nodes();
                const auto &tokens = m_tree.Tokens();
                const std::size_t end = std::min<std::size_t>(nodes[n].first_token + nodes[n].token_count, tokens.size());
                std::size_t first = nodes[n].first_token;
                while (first < end && IsTrivia(tokens[first].kind))
                {
                    ++first;
                }

                if (first >= end || Text(first) != "using")
                {
                    return;
                }

                std::size_t cursor = NextSig(first);
                if (cursor == kNone || cursor >= end)
                {
                    return;
                }

                const std::size_t parent = nodes[n].parent;
                const Path scope = m_node_path[n];
                const std::size_t active_begin = tokens[first].offset;
                const std::size_t active_end = parent < nodes.size() ? NodeEnd(parent) : m_tree.Source().size();

                auto read_path = [&](std::size_t at) -> std::pair<Path, std::size_t>
                {
                    Path path;
                    std::size_t last = at;
                    while (at != kNone && at < end)
                    {
                        if (Text(at) == "::" && path.empty())
                        {
                            at = NextSig(at);
                            continue;
                        }

                        if (tokens[at].kind != TokenKind::Identifier)
                        {
                            break;
                        }

                        path.emplace_back(Text(at));
                        last = at;
                        const std::size_t sep = NextSig(at);
                        if (sep != kNone && sep < end && Text(sep) == "::")
                        {
                            at = NextSig(sep);
                            continue;
                        }

                        break;
                    }

                    return {path, last};
                };

                if (Text(cursor) == "namespace")
                {
                    const auto [path, last] = read_path(NextSig(cursor));
                    if (!path.empty())
                    {
                        UsingDirective directive;
                        directive.scope = scope;
                        directive.target = path;
                        directive.offset = tokens[NextSig(cursor)].offset;
                        directive.length = tokens[last].offset + tokens[last].length - directive.offset;
                        directive.active_begin = active_begin;
                        directive.active_end = active_end;
                        m_directives.push_back(std::move(directive));
                    }

                    return;
                }

                if (Text(cursor) == "typename")
                {
                    cursor = NextSig(cursor);
                }

                if (cursor == kNone || cursor >= end || tokens[cursor].kind != TokenKind::Identifier)
                {
                    return;
                }

                const std::size_t after = NextSig(cursor);
                if (after != kNone && after < end && Text(after) == "=")
                {
                    // Alias declaration: a type name in this scope.
                    Decl decl;
                    decl.name = std::string(Text(cursor));
                    decl.scope = scope;
                    decl.offset = tokens[cursor].offset;
                    decl.length = tokens[cursor].length;
                    decl.kind = CompletionKind::Type;
                    decl.is_definition = true;
                    AddDecl(std::move(decl));
                    return;
                }

                const auto [path, last] = read_path(cursor);
                if (path.size() >= kMinQualifiedParts)
                {
                    UsingDecl using_decl;
                    using_decl.scope = scope;
                    using_decl.target = path;
                    using_decl.offset = tokens[last].offset;
                    using_decl.active_begin = active_begin;
                    using_decl.active_end = active_end;
                    m_using_decls.push_back(std::move(using_decl));
                }
            }

            void BuildExternal()
            {
                if (m_external == nullptr)
                {
                    return;
                }

                m_external_keys.reserve(m_external->size());
                for (std::size_t s = 0; s < m_external->size(); ++s)
                {
                    const IndexedScope &scope = (*m_external)[s];
                    m_external_keys.push_back(PathKey(scope.path));
                    for (std::size_t m = 0; m < scope.members.size(); ++m)
                    {
                        if (!scope.members[m].has_location)
                        {
                            continue;
                        }

                        // The tag scan files a type's own name inside its scope
                        // (injected-class-name); the real entry is in the parent.
                        if (scope.members[m].kind == CompletionKind::Type && !scope.path.empty() &&
                            scope.members[m].label == scope.path.back())
                        {
                            continue;
                        }

                        m_external_by_name[scope.members[m].label].push_back({s, m});
                    }
                }

                // Header scopes also make their path (and prefixes) known.
                for (const IndexedScope &scope: *m_external)
                {
                    for (std::size_t length = 1; length <= scope.path.size(); ++length)
                    {
                        m_known_scopes.insert(
                            PathKey(Path(scope.path.begin(), scope.path.begin() + static_cast<std::ptrdiff_t>(length))));
                    }

                    if (scope.kind == CompletionKind::Type && !scope.path.empty())
                    {
                        m_record_scopes.insert(PathKey(scope.path));
                    }
                }
            }

            const ParseTree &m_tree;
            const ScopeIndex *m_external;
            std::vector<Path> m_node_path;
            std::vector<Path> m_node_elements;
            std::unordered_map<std::size_t, ScopeName> m_scope_name;
            std::vector<Decl> m_decls;
            std::unordered_map<std::string, std::vector<std::size_t>> m_by_name;
            std::vector<RecordInfo> m_records;
            std::vector<ScopeRange> m_ranges;
            std::unordered_set<std::string> m_known_scopes;
            std::unordered_set<std::string> m_record_scopes;
            std::vector<UsingDirective> m_directives;
            std::vector<UsingDecl> m_using_decls;
            std::vector<std::string> m_external_keys;
            std::unordered_map<std::string, std::vector<ExternalRef>> m_external_by_name;
        };

        // ---- lookup ---------------------------------------------------------------

        struct LookupResult
        {
            std::vector<Candidate> found;
            bool ambiguous = false;
        };

        bool IsFunctionKind(CompletionKind kind) { return kind == CompletionKind::Function; }

        // Group candidates by entity (scope + name); declaration/definition pairs
        // collapse. Several groups are ambiguous unless every group is a function.
        bool Ambiguous(const std::vector<Candidate> &found)
        {
            std::unordered_map<std::string, bool> groups; // key -> all functions
            for (const Candidate &candidate: found)
            {
                const std::string key = Joined(candidate.scope, candidate.name);
                const auto [it, inserted] = groups.emplace(key, IsFunctionKind(candidate.kind));
                if (!inserted)
                {
                    it->second = it->second && IsFunctionKind(candidate.kind);
                }
            }

            if (groups.size() < kMinScopeGroups)
            {
                return false;
            }

            for (const auto &[key, all_functions]: groups)
            {
                if (!all_functions)
                {
                    return true;
                }
            }

            return false;
        }

        void CollectLevel(const Model &model, const Path &level, std::string_view name, std::size_t pos,
            std::vector<Candidate> &out, int base_depth)
        {
            model.Members(level, name, out);
            model.UsingDeclared(level, name, pos, out);
            for (const Path &nominated: model.NominatedAt(level, pos))
            {
                model.Members(nominated, name, out);
            }

            if (out.empty() && base_depth < kMaxBaseDepth && !level.empty() && model.IsRecordScope(level))
            {
                for (const Path &base: model.BaseScopes(level))
                {
                    CollectLevel(model, base, name, pos, out, base_depth + 1);
                    if (!out.empty())
                    {
                        break;
                    }
                }
            }
        }

        LookupResult LookupUnqualified(const Model &model, std::string_view name, std::size_t pos)
        {
            LookupResult result;
            // Block locals: innermost range wins, latest declaration in it.
            const Decl *best = nullptr;
            std::size_t best_index = kNone;
            const auto &decls = model.Decls();
            for (std::size_t i = 0; i < decls.size(); ++i)
            {
                const Decl &decl = decls[i];
                if (!decl.local || decl.name != name || !(decl.local_begin <= pos && pos < decl.local_end) ||
                    decl.offset > pos)
                {
                    continue;
                }

                const std::size_t span = decl.local_end - decl.local_begin;
                if (best == nullptr || span < best->local_end - best->local_begin ||
                    (span == best->local_end - best->local_begin && decl.offset > best->offset))
                {
                    best = &decl;
                    best_index = i;
                }
            }

            if (best != nullptr)
            {
                result.found.push_back(model.FromDecl(best_index));
                return result;
            }

            Path level = model.ScopeAt(pos);
            while (true)
            {
                std::vector<Candidate> found;
                CollectLevel(model, level, name, pos, found, 0);
                if (!found.empty())
                {
                    result.ambiguous = Ambiguous(found);
                    result.found = std::move(found);
                    return result;
                }

                if (level.empty())
                {
                    break;
                }

                level.pop_back();
            }

            return result;
        }

        // `a::b::` qualifier chain ending at identifier token `token`.
        struct Qualification
        {
            Path elements;
            bool absolute = false;
            bool member_access = false;
        };

        Qualification QualificationOf(const Model &model, std::size_t token)
        {
            Qualification out;
            std::size_t current = token;
            while (true)
            {
                const std::size_t before = model.PrevSig(current);
                if (before == kNone)
                {
                    break;
                }

                const std::string_view text = model.Text(before);
                if (text == "." || text == "->" || text == ".*" || text == "->*")
                {
                    out.member_access = out.elements.empty();
                    break;
                }

                if (text != "::")
                {
                    break;
                }

                std::size_t qualifier = model.PrevSig(before);
                if (qualifier == kNone)
                {
                    out.absolute = true;
                    break;
                }

                // Skip a balanced template argument list: `vector<int>::iterator`.
                if (model.Text(qualifier) == ">" || model.Text(qualifier) == ">>")
                {
                    int depth = model.Text(qualifier) == ">" ? 1 : kDoubleAngleCount;
                    while (depth > 0 && qualifier != kNone)
                    {
                        qualifier = model.PrevSig(qualifier);
                        if (qualifier == kNone)
                        {
                            break;
                        }

                        const std::string_view piece = model.Text(qualifier);
                        if (piece == ">")
                        {
                            ++depth;
                        }
                        else if (piece == ">>")
                        {
                            depth += kDoubleAngleCount;
                        }
                        else if (piece == "<")
                        {
                            --depth;
                        }
                    }

                    if (qualifier != kNone)
                    {
                        qualifier = model.PrevSig(qualifier);
                    }
                }

                if (qualifier == kNone || model.Tree().Tokens()[qualifier].kind != TokenKind::Identifier)
                {
                    out.absolute = true;
                    break;
                }

                out.elements.insert(out.elements.begin(), std::string(model.Text(qualifier)));
                current = qualifier;
            }

            return out;
        }

        // First qualifier element resolved through enclosing scopes and active
        // directives; remaining elements must exist below it.
        std::optional<Path> ResolveQualifier(const Model &model, const Qualification &qualification,
            std::size_t pos, const Path &elements)
        {
            if (elements.empty())
            {
                return Path{};
            }

            if (qualification.absolute)
            {
                return model.KnownScope(elements) ? std::optional<Path>(elements) : std::nullopt;
            }

            Path level = model.ScopeAt(pos);
            while (true)
            {
                Path candidate = level;
                candidate.insert(candidate.end(), elements.begin(), elements.end());
                if (model.KnownScope(candidate))
                {
                    return candidate;
                }

                for (const Path &nominated: model.NominatedAt(level, pos))
                {
                    Path through = nominated;
                    through.insert(through.end(), elements.begin(), elements.end());
                    if (model.KnownScope(through))
                    {
                        return through;
                    }
                }

                if (level.empty())
                {
                    break;
                }

                level.pop_back();
            }

            return std::nullopt;
        }

        std::vector<Candidate> ResolveAt(const Model &model, std::size_t offset, bool &ambiguous)
        {
            ambiguous = false;
            std::vector<Candidate> out;
            const std::size_t token = model.IdentifierAt(offset);
            if (token == kNone)
            {
                return out;
            }

            const std::string name(model.Text(token));
            const std::size_t pos = model.Tree().Tokens()[token].offset;

            // Cursor on a declaration site.
            const auto &decls = model.Decls();
            for (std::size_t i = 0; i < decls.size(); ++i)
            {
                if (decls[i].offset == pos && decls[i].name == name)
                {
                    out.push_back(model.FromDecl(i));
                    return out;
                }
            }

            const Qualification qualification = QualificationOf(model, token);
            if (qualification.member_access)
            {
                model.MemberCandidates(name, out);
                return out;
            }

            const std::size_t next = model.NextSig(token);
            const bool is_qualifier = next != kNone && model.Text(next) == "::";
            if (is_qualifier)
            {
                // A namespace/type used as qualifier: resolve the path ending here.
                Path chain = qualification.elements;
                chain.push_back(name);
                if (const auto path = ResolveQualifier(model, qualification, pos, chain))
                {
                    Path scope(path->begin(), path->end() - 1);
                    model.Members(scope, name, out);
                    std::erase_if(out, [](const Candidate &candidate)
                        { return candidate.kind != CompletionKind::Namespace && candidate.kind != CompletionKind::Type; });
                }

                return out;
            }

            if (!qualification.elements.empty() || qualification.absolute)
            {
                if (const auto scope = ResolveQualifier(model, qualification, pos, qualification.elements))
                {
                    model.Members(*scope, name, out);
                    if (out.empty() && model.IsRecordScope(*scope))
                    {
                        for (const Path &base : model.BaseScopes(*scope))
                        {
                            model.Members(base, name, out);
                            if (!out.empty())
                            {
                                break;
                            }
                        }
                    }
                }

                return out;
            }

            LookupResult result = LookupUnqualified(model, name, pos);
            ambiguous = result.ambiguous;
            return std::move(result.found);
        }

        // Keep definitions when the group has any; drop duplicate declarations.
        std::vector<Candidate> PreferDefinitions(const Model &model, std::vector<Candidate> found)
        {
            (void)model;
            std::unordered_map<std::string, bool> has_definition;
            auto key_of = [](const Candidate &candidate)
            {
                return Joined(candidate.scope, candidate.name) + '#' +
                    (IsFunctionKind(candidate.kind) ? std::to_string(candidate.param_count) : std::string());
            };
            for (const Candidate &candidate: found)
            {
                if (candidate.is_definition)
                {
                    has_definition[key_of(candidate)] = true;
                }
            }

            std::vector<Candidate> out;
            for (Candidate &candidate: found)
            {
                if (!candidate.is_definition && has_definition.contains(key_of(candidate)))
                {
                    continue;
                }

                out.push_back(std::move(candidate));
            }

            return out;
        }

        void AppendUnique(std::vector<NavTarget> &out, NavTarget target)
        {
            for (const NavTarget &existing: out)
            {
                if (existing.file == target.file && existing.offset == target.offset)
                {
                    return;
                }
            }

            out.push_back(std::move(target));
        }

        std::vector<NavTarget> ToTargets(const Model &model, const std::vector<Candidate> &candidates)
        {
            std::vector<NavTarget> out;
            for (const Candidate &candidate: candidates)
            {
                AppendUnique(out, model.ToTarget(candidate));
            }

            return out;
        }
    } // namespace

    std::vector<UsingDirective> Navigation::UsingDirectives(const ParseTree &tree)
    {
        return Model(tree, nullptr).Directives();
    }

    std::vector<AmbiguousReference> Navigation::FindAmbiguities(const ParseTree &tree, const ScopeIndex *external)
    {
        const Model model(tree, external);
        std::vector<AmbiguousReference> out;
        if (model.Directives().empty() && model.UsingDecls().empty())
        {
            return out;
        }

        std::unordered_set<std::size_t> declaration_sites;
        for (const Decl &decl: model.Decls())
        {
            declaration_sites.insert(decl.offset);
        }

        // Offsets covered by using-declarations/directives and preprocessor lines.
        std::vector<std::pair<std::size_t, std::size_t>> skip;
        for (std::size_t n = 1; n < tree.Nodes().size(); ++n)
        {
            if (tree.Nodes()[n].kind == GrammarKind::UsingDeclaration)
            {
                skip.emplace_back(model.NodeBegin(n), model.NodeEnd(n));
            }
        }

        for (const auto &directive: tree.Directives())
        {
            skip.emplace_back(directive.offset, directive.offset + directive.length);
        }

        std::unordered_map<std::string, bool> maybe_cache;
        const auto &tokens = tree.Tokens();
        for (std::size_t t = 0; t < tokens.size(); ++t)
        {
            if (tokens[t].kind != TokenKind::Identifier)
            {
                continue;
            }

            const std::size_t offset = tokens[t].offset;
            if (declaration_sites.contains(offset))
            {
                continue;
            }

            bool skipped = false;
            for (const auto &[begin, end]: skip)
            {
                if (begin <= offset && offset < end)
                {
                    skipped = true;
                    break;
                }
            }

            if (skipped)
            {
                continue;
            }

            const std::string name(model.Text(t));
            auto cached = maybe_cache.find(name);
            if (cached == maybe_cache.end())
            {
                cached = maybe_cache.emplace(name, model.MaybeAmbiguous(name)).first;
            }

            if (!cached->second)
            {
                continue;
            }

            const std::size_t before = model.PrevSig(t);
            if (before != kNone)
            {
                const std::string_view text = model.Text(before);
                if (text == "." || text == "->" || text == "::" || text == ".*" || text == "->*" ||
                    text == "struct" || text == "class" || text == "union" || text == "enum" ||
                    text == "namespace")
                {
                    continue;
                }
            }

            const std::size_t next = model.NextSig(t);
            if (next != kNone && model.Text(next) == "::")
            {
                continue;
            }

            const LookupResult result = LookupUnqualified(model, name, offset);
            if (!result.ambiguous)
            {
                continue;
            }

            AmbiguousReference reference;
            reference.offset = offset;
            reference.length = tokens[t].length;
            reference.name = name;
            std::unordered_set<std::string> seen;
            for (const Candidate &candidate: result.found)
            {
                std::string qualified = Joined(candidate.scope, candidate.name);
                if (seen.insert(qualified).second)
                {
                    reference.candidates.push_back(std::move(qualified));
                }
            }

            std::sort(reference.candidates.begin(), reference.candidates.end());
            out.push_back(std::move(reference));
        }

        return out;
    }

    std::vector<NavTarget> Navigation::Definition(const ParseTree &tree, std::size_t offset, const ScopeIndex *external)
    {
        const Model model(tree, external);
        bool ambiguous = false;
        std::vector<Candidate> found = ResolveAt(model, offset, ambiguous);
        found = PreferDefinitions(model, std::move(found));
        // A declaration site with a definition elsewhere in the buffer jumps to it;
        // a definition site (`void C::m() {}`) jumps back to its declaration.
        std::vector<Candidate> refined;
        bool jumped_back = false;
        const std::size_t cursor_token = model.IdentifierAt(offset);
        const std::size_t cursor_offset =
            cursor_token == kNone ? kNone : model.Tree().Tokens()[cursor_token].offset;
        for (const Candidate &candidate: found)
        {
            if (candidate.decl != kNone && candidate.is_definition && candidate.kind == CompletionKind::Function &&
                model.Decls()[candidate.decl].offset == cursor_offset)
            {
                std::vector<Candidate> same;
                model.Members(candidate.scope, candidate.name, same);
                bool declared = false;
                for (const Candidate &other: same)
                {
                    if (!other.is_definition && other.kind == CompletionKind::Function &&
                        other.param_count == candidate.param_count)
                    {
                        refined.push_back(other);
                        declared = true;
                    }
                }

                if (declared)
                {
                    jumped_back = true;
                    continue;
                }
            }

            if (candidate.decl != kNone && !candidate.is_definition && candidate.kind == CompletionKind::Function)
            {
                std::vector<Candidate> same;
                model.Members(candidate.scope, candidate.name, same);
                bool replaced = false;
                for (const Candidate &other: same)
                {
                    if (other.is_definition && other.kind == CompletionKind::Function &&
                        other.param_count == candidate.param_count)
                    {
                        refined.push_back(other);
                        replaced = true;
                    }
                }

                if (replaced)
                {
                    continue;
                }
            }

            refined.push_back(candidate);
        }

        std::vector<NavTarget> targets = ToTargets(model, refined);
        for (NavTarget &target: targets)
        {
            target.back_reference = jumped_back && !target.is_definition;
        }

        return targets;
    }

    std::vector<NavTarget> Navigation::Implementation(const ParseTree &tree, std::size_t offset,
        const ScopeIndex *external)
    {
        const Model model(tree, external);
        bool ambiguous = false;
        const std::vector<Candidate> found = ResolveAt(model, offset, ambiguous);
        std::vector<NavTarget> out;
        for (const Candidate &candidate: found)
        {
            bool added = false;
            if (candidate.kind == CompletionKind::Function)
            {
                // Overriders in derived records.
                if (!candidate.scope.empty() && model.IsRecordScope(candidate.scope))
                {
                    for (const RecordInfo *derived: model.Derived(candidate.scope.back()))
                    {
                        std::vector<Candidate> members;
                        model.Members(derived->path, candidate.name, members);
                        for (const Candidate &member: members)
                        {
                            if (member.kind == CompletionKind::Function && member.param_count == candidate.param_count)
                            {
                                AppendUnique(out, model.ToTarget(member));
                                added = true;
                            }
                        }
                    }
                }

                if (!added)
                {
                    // Out-of-line definition(s) of the same overload.
                    std::vector<Candidate> same;
                    model.Members(candidate.scope, candidate.name, same);
                    for (const Candidate &other: same)
                    {
                        if (other.kind == CompletionKind::Function && other.is_definition &&
                            other.param_count == candidate.param_count)
                        {
                            AppendUnique(out, model.ToTarget(other));
                            added = true;
                        }
                    }
                }
            }

            if (candidate.kind == CompletionKind::Type)
            {
                for (const RecordInfo *derived: model.Derived(candidate.name))
                {
                    std::vector<Candidate> self;
                    Path scope(derived->path.begin(), derived->path.end() - 1);
                    model.Members(scope, derived->path.back(), self);
                    for (const Candidate &item: self)
                    {
                        if (item.kind == CompletionKind::Type)
                        {
                            AppendUnique(out, model.ToTarget(item));
                            added = true;
                        }
                    }
                }
            }

            if (!added)
            {
                AppendUnique(out, model.ToTarget(candidate));
            }
        }

        return out;
    }

    std::vector<NavTarget> Navigation::FindDefinitions(const ParseTree &tree, const std::vector<std::string> &scope,
        std::string_view name, bool function, std::uint32_t param_count)
    {
        const Model model(tree, nullptr);
        std::vector<Candidate> members;
        model.Members(scope, name, members);
        std::vector<NavTarget> out;
        for (const Candidate &candidate: members)
        {
            if (!candidate.is_definition)
            {
                continue;
            }

            if (function ? (candidate.kind == CompletionKind::Function && candidate.param_count == param_count)
                         : candidate.kind == CompletionKind::Type)
            {
                out.push_back(model.ToTarget(candidate));
            }
        }

        return out;
    }

    std::vector<NavTarget> Navigation::FindOverriders(const ParseTree &tree, std::string_view base,
        std::string_view name, std::uint32_t param_count)
    {
        const Model model(tree, nullptr);
        std::vector<NavTarget> out;
        for (const RecordInfo *derived: model.Derived(base))
        {
            std::vector<Candidate> members;
            model.Members(derived->path, name, members);
            for (const Candidate &member: members)
            {
                if (member.kind == CompletionKind::Function && member.param_count == param_count)
                {
                    AppendUnique(out, model.ToTarget(member));
                }
            }
        }

        return out;
    }

} // namespace heimdall
