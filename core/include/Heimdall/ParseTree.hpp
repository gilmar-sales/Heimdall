#pragma once

#include <Heimdall/SyntaxTree.hpp>
#include <Heimdall/Preprocessor.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heimdall
{

    enum class GrammarKind : std::uint8_t
    {
        TranslationUnit,
        PreprocessorDirective,
        Declaration,
        ParameterDeclaration,
        InitDeclarator,
        FunctionDefinition,
        FunctionDeclaration,
        NamespaceDefinition,
        RecordDefinition,
        Enumerator,
        CompoundStatement,
        DeclarationStatement,
        ExpressionStatement,
        ReturnStatement,
        IfStatement,
        LoopStatement,
        SwitchStatement,
        JumpStatement,
        EmptyStatement,
        IdentifierExpression,
        LiteralExpression,
        ParenthesizedExpression,
        UnaryExpression,
        BinaryExpression,
        ConditionalExpression,
        CallExpression,
        SubscriptExpression,
        MemberExpression,
        LambdaExpression,
        CaseLabel,
        TryStatement,
        DoStatement,
        TemplateDeclaration,
        TemplateArgument,
        TemplateIdExpression,
        TypeSpecifier,
        Declarator,
        DeclaredName,
        PointerOperator,
        NestedNameSpecifier,
        ArraySuffix,
        FunctionSuffix,
        TrailingReturnType,
        NoexceptSpecifier,
        AttributeSpecifier,
        BitfieldSuffix,
        ModuleDeclaration,
        ImportDeclaration,
        UsingDeclaration,
        ConceptDefinition,
        RequiresClause,
        RequiresExpression,
        Requirement,
        ErrorExpression,
        Error,
        AccessSpecifier
    };

    struct GrammarNode
    {
        GrammarKind kind;
        std::uint32_t first_token;
        std::uint32_t token_count;
        std::uint32_t parent;
        std::uint32_t subtree_end;
    };

    struct GrammarDiagnostic
    {
        std::size_t offset;
        std::string message;
    };

    struct ParserOptions
    {
        CppStandard standard = CppStandard::Cpp20;
        Preprocessor::MacroMap predefined_macros;
        std::shared_ptr<const Preprocessor::MacroMap> shared_macros;
        const Preprocessor::MacroMap & Macros() const noexcept
        {
            return shared_macros ? *shared_macros : predefined_macros;
        }
    };

    // A top-level item of the translation unit as the grammar parsed it: a
    // contiguous slice of tokens, nodes and diagnostics that depends on nothing
    // outside its own tokens, which is what lets a later parse reuse it.
    struct TopLevelItem
    {
        std::uint32_t first_token;
        std::uint32_t token_end;
        std::uint32_t sig_count;
        std::uint32_t node_begin;
        std::uint32_t node_end;
        std::uint32_t diag_begin;
        std::uint32_t diag_end;
        bool reusable;
    };

    class ParseTree;

    // Describes how a new parse relates to an earlier one: `previous` was parsed
    // from the text in which `old_length` bytes at `offset` were replaced by
    // `new_length` bytes to give the text being parsed. Items of `previous` that
    // lie outside that edit are copied instead of re-parsed.
    struct ParseReuse
    {
        const ParseTree *previous = nullptr;
        std::size_t offset = 0;
        std::size_t old_length = 0;
        std::size_t new_length = 0;
    };

    namespace detail
    {
        void ParseWithGrammar(ParseTree &tree, const PreprocessorResult &preprocessing,
            std::stop_token stop, const Preprocessor::MacroMap * macros, const ParseReuse *reuse);
    } // namespace detail

    // Initial recursive-descent grammar layer over the lossless lexer. It parses
    // translation-unit items and compound statements while retaining token ranges
    // for declaration/expression forms not yet covered by dedicated productions.
    class ParseTree
    {
    public:
        static constexpr std::size_t RootNode = 0;

        static ParseTree Parse(std::string_view source, CppStandard standard = CppStandard::Cpp20);
        static ParseTree Parse(std::string_view source, const ParserOptions &options);
        // Cooperative cancellation: the grammar pass polls `stop` between
        // top-level items. A cancelled tree is partial; check Cancelled() and
        // discard it.
        // `lexed`, when given, must be Lexer(source).Lex(); it skips the lexing
        // pass (the LSP keeps tokens current via Lexer::Relex).
        static ParseTree Parse(std::string_view source, const ParserOptions &options,
            std::stop_token stop, const std::vector<Token> * lexed = nullptr,
            const ParseReuse *reuse = nullptr);

        std::string_view Source() const noexcept
        {
            return m_source;
        }
        CppStandard Standard() const noexcept
        {
            return m_standard;
        }
        const std::vector<Token> & Tokens() const noexcept
        {
            return m_tokens;
        }
        const std::vector<GrammarNode> & Nodes() const noexcept
        {
            return m_nodes;
        }
        const std::vector<GrammarDiagnostic> & Diagnostics() const noexcept
        {
            return m_diagnostics;
        }
        const std::vector<PreprocessorDirective> & Directives() const noexcept
        {
            return m_directives;
        }
        const std::vector<TopLevelItem> & Items() const noexcept
        {
            return m_items;
        }
        // Top-level items copied from the previous tree instead of parsed.
        std::size_t ReusedItems() const noexcept
        {
            return m_reused_items;
        }
        bool Cancelled() const noexcept
        {
            return m_cancelled;
        }
        // Identifier tokens the grammar treated as invisible because a predefined
        // macro expands to mere decoration (`class EXPORT Name`, `_GLIBCXX_NOEXCEPT`).
        // Name extraction that reads raw tokens must skip them too.
        bool IsDecorationToken(std::size_t token) const noexcept
        {
            return token < m_decoration.size() && m_decoration[token];
        }
        std::string_view Text(const Token &token) const noexcept
        {
            return m_source.substr(token.offset, token.length);
        }
        std::vector<std::size_t> Children(std::size_t node_index) const;
        bool IsDescendant(std::size_t node_index, std::size_t candidate) const noexcept;
        void HoldSource(std::shared_ptr<const std::string> owned);
        // Drops the tree's claim on the source text and the view of it. Afterwards
        // only the tokens, nodes, items and diagnostics may be used (that is all a
        // parse reusing this tree reads); call it only on a tree nobody else holds.
        void ReleaseSource() noexcept
        {
            m_owned_source.reset();
            m_source = {};
        }

    private:
        friend class GrammarParser;
        friend void detail::ParseWithGrammar(ParseTree &, const PreprocessorResult &,
            std::stop_token, const Preprocessor::MacroMap *, const ParseReuse *);

        std::shared_ptr<const std::string> m_owned_source;
        std::string_view m_source;
        CppStandard m_standard = CppStandard::Cpp20;
        std::vector<Token> m_tokens;
        std::vector<GrammarNode> m_nodes;
        std::vector<GrammarDiagnostic> m_diagnostics;
        std::vector<PreprocessorDirective> m_directives;
        bool m_cancelled = false;
        std::vector<bool> m_decoration;
        std::vector<TopLevelItem> m_items;
        std::size_t m_reused_items = 0;
    };

} // namespace heimdall
