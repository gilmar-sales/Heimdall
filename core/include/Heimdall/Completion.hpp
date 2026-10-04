#pragma once

#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{

    enum class CompletionKind : std::uint8_t
    {
        Keyword,
        Type,
        Namespace,
        Function,
        Variable,
        Macro,
        Directive
    };

    struct CompletionItem
    {
        std::string label;
        CompletionKind kind = CompletionKind::Keyword;
        // Short signature shown beside the label (`int add(int left, int right)`).
        std::string detail;
        // Doc comment shown in the side popup (sent as LSP markdown).
        std::string documentation;
        // Source location of the declaring name, filled for indexed declarations
        // (navigation: go to definition / implementation). `file` indexes the
        // owning IncludeIndex::Files(); -1 means the buffer the index was built
        // from.
        std::int32_t file = -1;
        std::uint32_t offset = 0;
        bool has_location = false;
        // Function with a body, record/enum with a body, variable or alias.
        bool is_definition = false;
        // Functions: number of parameters (overload matching for navigation).
        std::uint32_t param_count = 0;
        // Type as written, for member access (`obj.`): the declared type of a
        // variable/field, the return type of a function, or the aliased type of
        // a `using`/`typedef` (`basic_string<char>` for `std::string`).
        std::string type_text;
    };

    // One named scope and its direct members. `path` is the qualified path from
    // the translation unit (`{"std", "chrono"}`); an empty path denotes the
    // global scope. `kind` describes the scope itself when listed as a member
    // (Namespace for namespaces, Type for records). Backbone for header indexing:
    // headers are parsed exactly like sources and their scopes merged by path.
    struct IndexedScope
    {
        std::vector<std::string> path;
        CompletionKind kind = CompletionKind::Type;
        std::vector<CompletionItem> members;
        // Base classes of a record scope as written (`ns::Base`, template
        // arguments dropped); member access walks them for inherited members.
        std::vector<std::string> bases;
    };

    using ScopeIndex = std::vector<IndexedScope>;

    // Lexical + local ParseTree completion engine (core, STL only).
    // Works on incomplete code: ParseTree diagnostics are ignored, candidates
    // are collected from lexer identifiers, DeclaredName nodes and macros,
    // then filtered by the identifier prefix immediately before `offset`.
    class CompletionEngine
    {
    public:
        static std::string PrefixAt(std::string_view source, std::size_t offset);
        static std::vector<CompletionItem> Complete(std::string_view source, std::size_t offset);
        static std::vector<CompletionItem> Complete(std::string_view source, const ParserOptions &options,
            std::size_t offset);
        static std::vector<CompletionItem> Complete(std::string_view source, const ParserOptions &options,
            std::size_t offset, const ScopeIndex *external);
        static std::vector<CompletionItem> Complete(const ParseTree &tree, const ParserOptions &options,
            std::size_t offset, const ScopeIndex *external = nullptr);

        static ScopeIndex IndexScopes(std::string_view source, const ParserOptions &options);
        static ScopeIndex IndexScopes(const ParseTree &tree);
        static std::optional<CompletionItem> Hover(std::string_view source, const ParserOptions &options,
            std::size_t offset, const ScopeIndex *external = nullptr);
        static std::optional<CompletionItem> Hover(const ParseTree &tree, const ParserOptions &options,
            std::size_t offset, const ScopeIndex *external = nullptr);
    };

} // namespace heimdall
