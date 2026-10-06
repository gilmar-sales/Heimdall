#pragma once

#include <Heimdall/Completion.hpp>
#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{

    // A resolved declaration site. `file` is -1 for the analyzed buffer and an
    // index into IncludeIndex::Files() for names that come from the header index.
    struct NavTarget
    {
        std::int32_t file = -1;
        std::size_t offset = 0;
        std::size_t length = 0;
        std::string name;
        // Qualified scope of the entity (`{"a", "S"}` for `a::S::m`).
        std::vector<std::string> scope;
        CompletionKind kind = CompletionKind::Variable;
        // Function with a body, record/enum with a body, variable, alias.
        bool is_definition = false;
        std::uint32_t param_count = 0;
        // The cursor was on this function's definition and the target is its
        // declaration: callers must not follow it back into the body.
        bool back_reference = false;
    };

    // `using namespace target;` with the lexical range where it is in effect.
    struct UsingDirective
    {
        // Namespace/record path of the scope that contains the directive.
        std::vector<std::string> scope;
        // Nominated namespace as written (`{"std", "chrono"}`).
        std::vector<std::string> target;
        std::size_t offset = 0;
        std::size_t length = 0;
        std::size_t active_begin = 0;
        std::size_t active_end = 0;
    };

    // An unqualified name that two or more using-directives (or a directive and a
    // declaration) bring into the same lookup level.
    struct AmbiguousReference
    {
        std::size_t offset = 0;
        std::size_t length = 0;
        std::string name;
        // Qualified names of the competing entities, sorted (`a::x`, `b::x`).
        std::vector<std::string> candidates;
    };

    // Name resolution for go-to-definition / go-to-implementation and the
    // using-directive ambiguity check. Works on the buffer's ParseTree plus the
    // optional header ScopeIndex (items carry file/offset, see CompletionItem).
    // Lookup follows C++ rules approximately: block locals, then enclosing
    // class/namespace scopes (with base classes), where a using-directive makes
    // the nominated namespace visible at the nearest scope that contains both the
    // directive and the namespace; the first level with a match wins.
    class Navigation
    {
    public:
        static std::vector<UsingDirective> UsingDirectives(const ParseTree& tree);

        static std::vector<AmbiguousReference> FindAmbiguities(const ParseTree& tree,
            const ScopeIndex* external = nullptr);

        // Where the symbol under `offset` is defined (body for functions when
        // known, else the declaration). An ambiguous name yields every candidate.
        static std::vector<NavTarget> Definition(const ParseTree& tree, std::size_t offset,
            const ScopeIndex* external = nullptr);

        // Where the symbol under `offset` is implemented: overriders / derived
        // types, else out-of-line definitions, else the definition itself.
        static std::vector<NavTarget> Implementation(const ParseTree& tree, std::size_t offset,
            const ScopeIndex* external = nullptr);

        // Definitions inside `tree` of the function (`function`) or type declared
        // elsewhere as `scope::name`; used to follow a header declaration into
        // its source file. `param_count` selects the overload for functions.
        static std::vector<NavTarget> FindDefinitions(
            const ParseTree& tree,
            const std::vector<std::string>& scope,
            std::string_view name,
            bool function,
            std::uint32_t param_count);

        // Methods `name` declared in types (transitively) derived from `base`.
        static std::vector<NavTarget> FindOverriders(
            const ParseTree& tree,
            std::string_view base,
            std::string_view name,
            std::uint32_t param_count);
    };

} // namespace heimdall
