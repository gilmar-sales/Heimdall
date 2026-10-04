#include <Heimdall/SemanticModel.hpp>

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace heimdall
{

    InternPool::InternPool(std::pmr::memory_resource * resource)
    : m_resource(resource), m_text(resource), m_ids(resource) {}

    NameId InternPool::Intern(std::string_view text)
    {
        if (const auto found = m_ids.find(text); found != m_ids.end())
        {
            return found->second;
        }

        const auto id = static_cast<NameId>(m_text.size());
        m_text.push_back(text);
        m_ids.emplace(text, id);
        return id;
    }

    NameId InternPool::InternCopy(std::string_view text)
    {
        if (const auto found = m_ids.find(text); found != m_ids.end())
        {
            return found->second;
        }

        auto *storage = static_cast<char * >(m_resource->allocate(text.size() == 0 ? 1 : text.size(), 1));
        if (!text.empty())
        {
            std::memcpy(storage, text.data(), text.size());
        }

        return Intern(std::string_view(storage, text.size()));
    }

    NameId InternPool::Find(std::string_view text) const
    {
        const auto found = m_ids.find(text);
        return found == m_ids.end() ? kNone : found->second;
    }

    SymbolTable::SymbolTable(std::pmr::memory_resource * resource)
    : name(resource), scope(resource), kind(resource), flags(resource), decl_token(resource),
        decl_node(resource), member_scope(resource), signature(resource), first_base(resource),
        base_count(resource), next_same_name(resource) {}

    ScopeTable::ScopeTable(std::pmr::memory_resource * resource)
    : parent(resource), kind(resource), owner(resource), node(resource) {}

    BaseTable::BaseTable(std::pmr::memory_resource * resource)
    : derived(resource), name(resource), token(resource), target(resource) {}

    RefTable::RefTable(std::pmr::memory_resource * resource) : token(resource), target(resource) {}

    SemanticModel::SemanticModel(const ParseTree &tree, std::size_t arena_hint)
    : m_arena(std::make_unique<Arena>(arena_hint)), m_tree(&tree), m_names(m_arena->Resource()),
        m_symbols(m_arena->Resource()), m_scopes(m_arena->Resource()), m_bases(m_arena->Resource()),
        m_refs(m_arena->Resource()), m_declared(m_arena->Resource()), m_sig(m_arena->Resource()),
      m_node_scope(m_arena->Resource()), m_code(m_arena->Resource()), m_child_begin(m_arena->Resource()),
      m_child_list(m_arena->Resource()) {}

    SymbolId SemanticModel::LookupLocal(ScopeId scope, NameId name) const
    {
        if (scope == kNone || name == kNone)
        {
            return kNone;
        }

        const auto found = m_declared.find((static_cast<std::uint64_t>(scope) << 32) | name);
        return found == m_declared.end() ? kNone : found->second;
    }

    SymbolId SemanticModel::LookupMember(SymbolId owner, NameId name) const
    {
        // Iterative walk over the owner and its resolved bases. Each class is
        // visited once: cycles (malformed code) and diamonds cost nothing extra.
        std::vector<SymbolId> pending{owner};
        std::unordered_set<SymbolId> visited;
        while (!pending.empty())
        {
            const SymbolId current = pending.back();
            pending.pop_back();
            if (current >= m_symbols.Size() ||!visited.insert(current).second)
            {
                continue;
            }

            if (const auto found = LookupLocal(m_symbols.member_scope[current], name); found != kNone)
            {
                return found;
            }

            const auto first = m_symbols.first_base[current];
            const auto count = m_symbols.base_count[current];
            for (std::uint32_t i = count; i > 0; --i)
            {
                if (const auto base = m_bases.target[first + i - 1]; base != kNone)
                {
                    pending.push_back(base);
                }
            }
        }

        return kNone;
    }

    SymbolId SemanticModel::Lookup(ScopeId scope, NameId name, std::uint32_t before_token) const
    {
        for (std::size_t steps = 0; scope != kNone && scope < m_scopes.Size() && steps < m_scopes.Size() + 1; ++steps)
        {
            const bool local = m_scopes.kind[scope] == ScopeKind::Function || m_scopes.kind[scope] == ScopeKind::Block;
            for (auto symbol = LookupLocal(scope,
                name); symbol != kNone; symbol = m_symbols.next_same_name[symbol])
            {
                if (!local || before_token == kNone || m_symbols.decl_token[symbol] < before_token)
                {
                    return symbol;
                }
            }

            if (m_scopes.kind[scope] == ScopeKind::Class && m_scopes.owner[scope] != kNone)
            {
                if (const auto inherited = LookupMember(m_scopes.owner[scope], name); inherited != kNone)
                {
                    return inherited;
                }
            }

            if (scope == TranslationUnitScope)
            {
                break;
            }

            scope = m_scopes.parent[scope];
        }

        return kNone;
    }

    SymbolId SemanticModel::ResolveToken(std::uint32_t token) const
    {
        const auto &tokens = m_refs.token;
        const auto it = std::lower_bound(tokens.begin(), tokens.end(), token);
        if (it == tokens.end() || *it != token)
        {
            return kNone;
        }

        return m_refs.target[static_cast<std::size_t>(it - tokens.begin())];
    }

} // namespace heimdall
