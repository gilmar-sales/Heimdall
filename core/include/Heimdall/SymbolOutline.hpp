#pragma once

#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace heimdall
{

    enum class OutlineKind : std::uint8_t
    {
        Namespace,
        Class,
        Struct,
        Union,
        Enum,
        EnumMember,
        Function,
        Method,
        Constructor,
        Destructor,
        Operator,
        Field,
        Variable,
        TypeAlias,
        Concept
    };

    inline constexpr std::uint32_t kNoOutlineParent = std::numeric_limits<std::uint32_t>::max();

    /// One declaration that belongs in a file outline or a workspace symbol search.
    struct OutlineSymbol
    {
        /// Name as written, including qualifiers (`Widget::Run`, `operator==`, `~Widget`).
        std::string name;
        /// Parameters and qualifiers for callables, type for variables, aliased type for aliases.
        std::string detail;
        /// Qualified path of the enclosing namespaces and types (`app::Widget`); empty at global
        /// scope.
        std::string container;
        OutlineKind kind = OutlineKind::Variable;
        /// The name that is selected when the symbol is picked.
        std::size_t nameOffset = 0;
        std::size_t nameLength = 0;
        /// The whole declaration, including a leading `template <...>` header and any body.
        std::size_t rangeOffset = 0;
        std::size_t rangeLength = 0;
        /// Index of the enclosing symbol in the same vector, or `kNoOutlineParent`.
        std::uint32_t parent = kNoOutlineParent;
    };

    /// Syntactic declaration inventory of a `ParseTree` (core, STL only). Works on incomplete
    /// code and needs neither a compile database nor semantic analysis. Block-local names and
    /// function parameters are not listed; forward declarations and `friend` declarations are
    /// skipped because they do not introduce a member of their scope.
    class SymbolOutline
    {
    public:
        /// Symbols in source order; a parent always precedes its children.
        [[nodiscard]] static std::vector<OutlineSymbol> Extract(const ParseTree& tree);
    };

} // namespace heimdall
