#pragma once

#include <Heimdall/FlowModel.hpp>

#include "TokenView.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace heimdall::detail
{

    // Decl-specifier keywords found between the start of a declaration and the
    // name it declares.
    namespace Spec
    {
        inline constexpr std::uint32_t Static = 1u << 0;
        inline constexpr std::uint32_t Constexpr = 1u << 1;
        inline constexpr std::uint32_t Constinit = 1u << 2;
        inline constexpr std::uint32_t Consteval = 1u << 3;
        inline constexpr std::uint32_t Extern = 1u << 4;
        inline constexpr std::uint32_t ThreadLocal = 1u << 5;
        inline constexpr std::uint32_t Mutable = 1u << 6;
        inline constexpr std::uint32_t Volatile = 1u << 7;
        inline constexpr std::uint32_t Register = 1u << 8;
        inline constexpr std::uint32_t Inline = 1u << 9;
        inline constexpr std::uint32_t Virtual = 1u << 10;
        inline constexpr std::uint32_t Friend = 1u << 11;
        inline constexpr std::uint32_t Typedef = 1u << 12;
        inline constexpr std::uint32_t Explicit = 1u << 13;
        inline constexpr std::uint32_t Const = 1u << 14;
        // Declared things the const/constexpr rules never touch.
        inline constexpr std::uint32_t Opaque = Constinit | Extern | ThreadLocal | Mutable | Volatile | Register |
            Typedef | Friend;
    } // namespace Spec

    struct Specifiers
    {
        std::uint32_t mask = 0;
        std::size_t const_count = 0;
        std::size_t const_position = 0; // position of the last `const`, when const_count > 0
    };

    // What the F4 rules need to know about constant expressions: whether the
    // initializer of a variable is one, and whether a function could be
    // `constexpr`. Everything the engine cannot see stays "not constant", so a
    // rule built on it only fires on what is certain.
    class ConstantAnalysis
    {
    public:
        explicit ConstantAnalysis(const FlowModel &flow);

        Specifiers SpecifiersOf(std::uint32_t decl_node, std::uint32_t name_token) const;

        // `T x = <constant expression>;` for a variable of arithmetic or enum
        // type, whatever its qualifiers.
        bool ConstantInitializer(SymbolId variable);

        enum class Candidate : std::uint8_t
        {
            None,
            ReplaceConst,   // `const T x = c;`: write `constexpr` instead of `const`
            InsertConstexpr // `T x = c;` that nothing modifies: add `constexpr`
        };
        // What the constexpr rule would do to this variable.
        Candidate ConstexprVariable(SymbolId variable);

        // A function that is not declared `constexpr` but could be.
        bool EligibleFunction(SymbolId function);
        bool DeclaredConstexpr(SymbolId function) const;

        // The declaration names exactly one variable.
        bool DeclaresSingleName(SymbolId variable) const;

    private:
        // Result of evaluating a constant expression. `ok` is false when the
        // expression is not constant (or the engine cannot tell); `known` when the
        // value itself is available; `wide` when the type is wider than `int` or of
        // a width that depends on the platform (values are then not tracked).
        struct Value
        {
            bool ok = false;
            bool known = false;
            bool is_float = false;
            bool wide = false;
            bool is_unsigned = false;
            std::int64_t i = 0;
            double f = 0;
        };
        class Parser;

        enum class State : std::uint8_t
        {
            Unvisited,
            Visiting,
            Yes,
            No
        };

        bool Literal(TypeId type, bool allow_void) const;
        bool UsableInConstantExpression(SymbolId variable);
        bool CalleeUsable(SymbolId callee, SymbolId current);
        bool CouldBeConstexpr(SymbolId function);
        bool BodyIsConstexprSafe(SymbolId function, FunctionId id);
        const std::vector<SymbolId> &LocalsOf(FunctionId function);
        bool InitializerRange(SymbolId variable, std::size_t &begin, std::size_t &end) const;
        bool InAnonymousNamespace(ScopeId scope) const;

        const FlowModel &m_flow;
        const TypeModel &m_types;
        const TypeTable &m_table;
        const SemanticModel &m_model;
        const SymbolTable &m_symbols;
        TokenView m_view;
        std::unordered_map<SymbolId, State> m_initializer;
        std::unordered_map<SymbolId, State> m_function;
        std::unordered_map<SymbolId, Value> m_values;
        std::vector<std::vector<SymbolId>> m_locals; // variables and parameters per FunctionId
        bool m_locals_built = false;
        std::unordered_map<std::uint32_t, SymbolId> m_symbol_at;
    };

} // namespace heimdall::detail
