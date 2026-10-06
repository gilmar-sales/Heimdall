#pragma once

#include <Heimdall/Arena.hpp>
#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heimdall
{

    // Entities are addressed by 32-bit indices into flat tables (see
    // docs/semantic-engine-architecture.md): no per-entity allocation, no
    // pointers. `kNone` means "absent" and, for resolutions, "Unknown".
    using NameId = std::uint32_t;
    using SymbolId = std::uint32_t;
    using ScopeId = std::uint32_t;
    inline constexpr std::uint32_t kNone = ~0u;

    // Maps identifier text to dense ids so name comparison is integer comparison.
    // Views passed to Intern() must outlive the pool (they point into the source
    // buffer); InternCopy() stores synthesized text in the arena.
    class InternPool
    {
    public:
        explicit InternPool(std::pmr::memory_resource* resource);

        NameId Intern(std::string_view text);
        NameId InternCopy(std::string_view text);
        NameId Find(std::string_view text) const;
        std::string_view Text(NameId id) const noexcept
        {
            return id < m_text.size() ? m_text[id] : std::string_view {};
        }

        std::size_t Size() const noexcept
        {
            return m_text.size();
        }

    private:
        std::pmr::memory_resource* m_resource;
        std::pmr::vector<std::string_view> m_text;
        std::pmr::unordered_map<std::string_view, NameId> m_ids;
    };

    enum class SymbolKind : std::uint8_t
    {
        Namespace,
        Class,
        Enum,
        Enumerator,
        Function,
        Variable,
        Parameter,
        TypeAlias
    };

    // Bit set stored in SymbolTable::flags.
    namespace SymbolFlag
    {
        inline constexpr std::uint32_t Virtual = 1u << 0;  // written `virtual`
        inline constexpr std::uint32_t Override = 1u << 1; // written `override`
        inline constexpr std::uint32_t Final = 1u << 2;    // written `final`
        inline constexpr std::uint32_t Const = 1u << 3;    // const-qualified member function
        inline constexpr std::uint32_t Static = 1u << 4;
        inline constexpr std::uint32_t Pure = 1u << 5; // `= 0`
        inline constexpr std::uint32_t Constructor = 1u << 6;
        inline constexpr std::uint32_t Destructor = 1u << 7;
        inline constexpr std::uint32_t Qualified = 1u << 8; // declared as `A::name`
        inline constexpr std::uint32_t Friend = 1u << 9;
        inline constexpr std::uint32_t Definition = 1u << 10; // function has a body
        inline constexpr std::uint32_t Defaulted = 1u << 11;  // `= default` / `= delete`
        inline constexpr std::uint32_t Operator = 1u << 12;
        inline constexpr std::uint32_t Template = 1u << 13; // declared under `template<...>`
        inline constexpr std::uint32_t RefQualified = 1u << 14;
        // Declared with `*` operators only (`T* p`, `T** p`): known to be a pointer.
        // Typedef'd and deduced pointer types are not flagged.
        inline constexpr std::uint32_t Pointer = 1u << 15;
        // Functions: the written return type is a pointer (`T* f()`, `auto f() -> T*`).
        inline constexpr std::uint32_t ReturnsPointer = 1u << 16;
    } // namespace SymbolFlag

    // Structure of arrays: a rule that filters on one property touches only
    // that column.
    struct SymbolTable
    {
        explicit SymbolTable(std::pmr::memory_resource* resource);

        std::pmr::vector<NameId> name;
        std::pmr::vector<ScopeId> scope; // scope the symbol is declared in
        std::pmr::vector<SymbolKind> kind;
        std::pmr::vector<std::uint32_t> flags;
        std::pmr::vector<std::uint32_t> decl_token; // index into ParseTree::Tokens()
        std::pmr::vector<std::uint32_t> decl_node;  // index into ParseTree::Nodes()
        std::pmr::vector<ScopeId> member_scope;     // namespaces and classes: where members live
        std::pmr::vector<std::uint64_t> signature;  // functions: parameter types + cv/ref qualifiers
        std::pmr::vector<std::uint32_t> first_base; // classes: range in BaseTable
        std::pmr::vector<std::uint32_t> base_count;
        std::pmr::vector<SymbolId> next_same_name; // chain of symbols sharing (scope, name)

        std::size_t Size() const noexcept
        {
            return name.size();
        }
    };

    enum class ScopeKind : std::uint8_t
    {
        TranslationUnit,
        Namespace,
        Class,
        Function,
        Block
    };

    struct ScopeTable
    {
        explicit ScopeTable(std::pmr::memory_resource* resource);

        std::pmr::vector<ScopeId> parent;
        std::pmr::vector<ScopeKind> kind;
        std::pmr::vector<SymbolId> owner;     // namespace/class symbol owning the scope, or kNone
        std::pmr::vector<std::uint32_t> node; // defining grammar node

        std::size_t Size() const noexcept
        {
            return parent.size();
        }
    };

    // One entry per base-specifier. `target` is kNone (Unknown) when the base
    // does not resolve to a class declared in this translation unit or names a
    // template-id (`Base<T>`): its members cannot be trusted.
    struct BaseTable
    {
        explicit BaseTable(std::pmr::memory_resource* resource);

        std::pmr::vector<SymbolId> derived;
        std::pmr::vector<NameId> name;
        std::pmr::vector<std::uint32_t> token; // first token of the written name
        std::pmr::vector<SymbolId> target;
    };

    // Identifier uses. `target` is kNone when the use is not (yet) resolvable:
    // member access through an object, template parameters, names from headers.
    struct RefTable
    {
        explicit RefTable(std::pmr::memory_resource* resource);

        std::pmr::vector<std::uint32_t> token;
        std::pmr::vector<SymbolId> target;
    };

    // Result of binding one ParseTree. Immutable once built, so it can be shared
    // across threads without locks. The tree (and its source buffer) must
    // outlive the model: names are views into that buffer.
    class SemanticModel
    {
    public:
        static constexpr ScopeId TranslationUnitScope = 0;

        explicit SemanticModel(const ParseTree& tree, std::size_t arena_hint = 64 * 1024);
        SemanticModel(SemanticModel&&) noexcept = default;

        const ParseTree& Tree() const noexcept
        {
            return *m_tree;
        }

        const InternPool& Names() const noexcept
        {
            return m_names;
        }

        const SymbolTable& Symbols() const noexcept
        {
            return m_symbols;
        }

        const ScopeTable& Scopes() const noexcept
        {
            return m_scopes;
        }

        const BaseTable& Bases() const noexcept
        {
            return m_bases;
        }

        const RefTable& Refs() const noexcept
        {
            return m_refs;
        }

        std::size_t ArenaBytes() const noexcept
        {
            return m_arena->Used();
        }

        std::size_t ArenaAllocations() const noexcept
        {
            return m_arena->AllocationCount();
        }

        // First symbol named `name` declared directly in `scope`; further
        // overloads follow through Symbols().next_same_name.
        SymbolId LookupLocal(ScopeId scope, NameId name) const;
        // Unqualified lookup from `scope` outwards. In function and block scopes
        // only declarations before `before_token` are visible.
        SymbolId Lookup(ScopeId scope, NameId name, std::uint32_t before_token = kNone) const;
        // Member of a class (searching its resolved bases) or namespace.
        SymbolId LookupMember(SymbolId owner, NameId name) const;
        // Resolution recorded for the identifier at `token`, or kNone.
        SymbolId ResolveToken(std::uint32_t token) const;
        // Scope that contains `node`'s declarations (its own scope for namespaces,
        // classes, functions and blocks).
        ScopeId ScopeOfNode(std::uint32_t node) const
        {
            return node < m_node_scope.size() ? m_node_scope[node] : TranslationUnitScope;
        }

        // Child nodes of `node`, in node-table order.
        std::span<const std::uint32_t> ChildrenOf(std::uint32_t node) const
        {
            if (node + 1 >= m_child_begin.size())
            {
                return {};
            }

            return {m_child_list.data() + m_child_begin[node], m_child_begin[node + 1] - m_child_begin[node]};
        }

        // Token is code the grammar attached to a node: false for trivia between
        // top-level items, preprocessor directives and inactive `#if` branches.
        bool IsCode(std::uint32_t token) const
        {
            return token < m_code.size() && m_code[token] != 0;
        }

        // Indices into Tree().Tokens() of the significant tokens, ascending:
        // code only, without trivia, directives or decoration macros.
        const std::pmr::vector<std::uint32_t>& Significant() const noexcept
        {
            return m_sig;
        }

    private:
        friend class Binder;
        friend class BinderImpl;

        std::unique_ptr<Arena> m_arena;
        const ParseTree* m_tree;
        InternPool m_names;
        SymbolTable m_symbols;
        ScopeTable m_scopes;
        BaseTable m_bases;
        RefTable m_refs;
        // (scope << 32 | name) -> first symbol of the chain.
        std::pmr::unordered_map<std::uint64_t, SymbolId> m_declared;
        std::pmr::vector<std::uint32_t> m_sig;
        std::pmr::vector<ScopeId> m_node_scope;
        std::pmr::vector<std::uint8_t> m_code;
        // Children of every node in compressed-row form.
        std::pmr::vector<std::uint32_t> m_child_begin;
        std::pmr::vector<std::uint32_t> m_child_list;
    };

    // Builds the model in one pass over the tree's node table.
    class Binder
    {
    public:
        static SemanticModel Bind(const ParseTree& tree);
    };

} // namespace heimdall
