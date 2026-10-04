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

    class ParseTree;
    namespace detail
    {
        void ParseWithGrammar(ParseTree &tree, const PreprocessorResult &preprocessing,
            std::stop_token stop);
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
        static ParseTree Parse(std::string_view source, const ParserOptions &options,
            std::stop_token stop);

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
        bool Cancelled() const noexcept
        {
            return m_cancelled;
        }
        std::string_view Text(const Token &token) const noexcept
        {
            return m_source.substr(token.offset, token.length);
        }
        std::vector<std::size_t> Children(std::size_t node_index) const;
        bool IsDescendant(std::size_t node_index, std::size_t candidate) const noexcept;
        void HoldSource(std::shared_ptr<const std::string> owned);

    private:
        friend class GrammarParser;
        friend void detail::ParseWithGrammar(ParseTree &, const PreprocessorResult &,
            std::stop_token);

        std::shared_ptr<const std::string> m_owned_source;
        std::string_view m_source;
        CppStandard m_standard = CppStandard::Cpp20;
        std::vector<Token> m_tokens;
        std::vector<GrammarNode> m_nodes;
        std::vector<GrammarDiagnostic> m_diagnostics;
        std::vector<PreprocessorDirective> m_directives;
        bool m_cancelled = false;
    };

} // namespace heimdall
