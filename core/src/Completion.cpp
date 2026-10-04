#include <Heimdall/Completion.hpp>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/Navigation.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/Preprocessor.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace heimdall
{

    namespace
    {

        constexpr bool IsIdentChar(char c)
        {
            return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') ||(c >= '0' && c <= '9') || c == '_' ||
                static_cast<unsigned char>(c) >= 0x80;
        }

        constexpr bool IsIdentStart(char c)
        {
            return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') || c == '_' ||
                static_cast<unsigned char>(c) >= 0x80;
        }

        constexpr std::string_view kKeywords[] = {
            "alignas", "alignof", "and", "and_eq", "asm", "auto",
            "bitand", "bitor", "bool", "break", "case", "catch",
            "char", "char8_t", "char16_t", "char32_t", "class", "compl",
            "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
            "continue", "co_await", "co_return", "co_yield", "decltype", "default",
            "delete", "do", "double", "dynamic_cast", "else", "enum",
            "explicit", "export", "extern", "false", "float", "for",
            "friend", "goto", "if", "inline", "int", "long",
            "mutable", "namespace", "new", "noexcept", "not", "not_eq",
            "nullptr", "operator", "or", "or_eq", "private", "protected",
            "public", "reinterpret_cast", "requires", "return", "short", "signed",
            "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
            "template", "this", "thread_local", "throw", "true", "try",
            "typedef", "typeid", "typename", "union", "unsigned", "using",
            "virtual", "void", "volatile", "wchar_t", "while", "xor",
            "xor_eq", "compl", "import", "module", "co_await", "co_return",
            "char8_t", "char16_t", "consteval", "constinit",
        };

        constexpr std::string_view kBuiltinTypes[] = {
            "void", "bool", "char", "char8_t", "char16_t", "char32_t",
            "wchar_t", "short", "int", "long", "signed", "unsigned",
            "float", "double", "auto", "void", "typename", "decltype",
        };

        constexpr std::string_view kDirectives[] = {
            "include", "define", "undef", "if", "ifdef", "ifndef",
            "elif", "else", "endif", "pragma", "error", "line",
        };

        bool IsBuiltinType(std::string_view word)
        {
            for (const auto type: kBuiltinTypes)
            {
                if (word == type)
                {
                    return true;
                }
            }

            return false;
        }

        bool IsKeyword(std::string_view word)
        {
            for (const auto keyword: kKeywords)
            {
                if (word == keyword)
                {
                    return true;
                }
            }

            return false;
        }

        int KindPriority(CompletionKind kind)
        {
            switch (kind)
            {
            case CompletionKind::Macro:
                return 5;
            case CompletionKind::Type:
            case CompletionKind::Namespace:
                return 4;
            case CompletionKind::Function:
                return 3;
            case CompletionKind::Variable:
                return 2;
            case CompletionKind::Directive:
                return 2;
            case CompletionKind::Keyword:
                return 1;
            }

            return 0;
        }

        std::string KindDetail(CompletionKind kind)
        {
            switch (kind)
            {
            case CompletionKind::Keyword:
                return "keyword";
            case CompletionKind::Type:
                return "type";
            case CompletionKind::Namespace:
                return "namespace";
            case CompletionKind::Function:
                return "function";
            case CompletionKind::Variable:
                return "variable";
            case CompletionKind::Macro:
                return "macro";
            case CompletionKind::Directive:
                return "directive";
            }

            return "";
        }

        bool StartsWith(std::string_view text, std::string_view prefix)
        {
            return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
        }

        enum class CursorContext
        {
            Expression,
            MemberAccess,
            ScopeAccess,
            Preprocessor,
            Suppressed,
        };

        // Finds the token holding offset-1 (the character just typed). Returns
        // tokens.size() when offset == 0 or source is empty.
        std::size_t TokenBefore(const std::vector<Token> & tokens, std::size_t offset)
        {
            if (offset == 0)
            {
                return tokens.size();
            }

            const std::size_t pos = offset - 1;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].offset <= pos && pos < tokens[i].offset + tokens[i].length)
                {
                    return i;
                }
            }

            return tokens.size();
        }

        std::size_t PreviousSignificant(const std::vector<Token> & tokens, std::size_t index,
            std::string_view source)
        {
            std::size_t i = index;
            while (i > 0)
            {
                --i;
                const TokenKind kind = tokens[i].kind;
                if (kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
                    kind == TokenKind::BlockComment)
                {
                    continue;
                }

                (void) source;
                return i;
            }

            return tokens.size();
        }

        std::string_view TokenText(std::string_view source, const Token &token)
        {
            return source.substr(token.offset, token.length);
        }

        // Index of the `.`, `->`, `.*` or `::` punctuation token directly preceding
        // the completion prefix (whitespace allowed, e.g. `ns :: name`), or
        // tokens.size() when there is none. Shared by context classification and
        // qualifier extraction so the two can never disagree.
        std::size_t AccessOperatorBefore(std::string_view source, const std::vector<Token> & tokens,
            std::size_t offset, std::string_view prefix)
        {
            const std::size_t before = TokenBefore(tokens, offset);
            // The prefix itself is either the identifier token under the cursor or
            // empty (cursor after punctuation/whitespace).
            bool prefix_is_identifier = false;
            if (before < tokens.size() && tokens[before].kind == TokenKind::Identifier)
            {
                const std::string_view text = TokenText(source, tokens[before]);
                if (!prefix.empty() && text.size() >= prefix.size() &&
                    text.substr(text.size() - prefix.size()) == prefix &&
                    tokens[before].offset + tokens[before].length >= offset &&
                    tokens[before].offset < offset)
                {
                    prefix_is_identifier = true;
                }
            }

            std::size_t prev = tokens.size();
            if (prefix_is_identifier)
            {
                prev = PreviousSignificant(tokens, before, source);
            }
            else if (before < tokens.size())
            {
                // Cursor is right after punctuation/whitespace: the token before the
                // cursor is significant unless it is whitespace.
                if (tokens[before].kind == TokenKind::Whitespace)
                {
                    prev = PreviousSignificant(tokens, before, source);
                }
                else
                {
                    prev = before;
                }
            }
            else
            {
                // Offset past the end: scan back to the last significant token.
                prev = PreviousSignificant(tokens, tokens.size(), source);
            }

            if (prev < tokens.size() && tokens[prev].kind == TokenKind::Punctuation)
            {
                return prev;
            }

            return tokens.size();
        }

        CursorContext ClassifyContext(std::string_view source, const std::vector<Token> & tokens,
            std::size_t offset, std::string_view prefix)
        {
            if (offset > source.size())
            {
                offset = source.size();
            }

            const std::size_t before = TokenBefore(tokens, offset);
            if (before < tokens.size())
            {
                const TokenKind kind = tokens[before].kind;
                if (kind == TokenKind::LineComment || kind == TokenKind::BlockComment ||
                    kind == TokenKind::StringLiteral || kind == TokenKind::CharacterLiteral ||
                    kind == TokenKind::RawStringLiteral)
                {
                    return CursorContext::Suppressed;
                }

                if (kind == TokenKind::Number)
                {
                    return CursorContext::Suppressed;
                }
            }

            // Preprocessor directive line: `^\s*#\s*\w*$` before the cursor.
            std::size_t line_start = source.rfind('\n', offset > 0 ? offset - 1 : 0);
            line_start = line_start == std::string_view::npos ? 0 : line_start + 1;
            // If offset is at the start of a line, rfind finds the previous newline;
            // recompute when offset points just after '\n'.
            if (offset > 0 && offset <= source.size() && source[offset - 1] == '\n')
            {
                line_start = offset;
            }

            std::size_t first = line_start;
            while (first < offset && (source[first] == ' ' || source[first] == '\t'))
            {
                ++first;
            }

            if (first < offset && source[first] == '#')
            {
                return CursorContext::Preprocessor;
            }

            // Member / scope access: look at the significant token before the prefix.
            const std::size_t access = AccessOperatorBefore(source, tokens, offset, prefix);
            if (access < tokens.size())
            {
                const std::string_view text = TokenText(source, tokens[access]);
                if (text == "." || text == "->" || text == ".*")
                {
                    return CursorContext::MemberAccess;
                }

                if (text == "::")
                {
                    return CursorContext::ScopeAccess;
                }
            }

            return CursorContext::Expression;
        }

        CompletionKind ClassifyDeclaredName(const ParseTree &tree, std::size_t node_index)
        {
            std::size_t current = tree.Nodes()[node_index].parent;
            for (std::size_t depth = 0; depth < 8 && current < tree.Nodes().size(); ++depth)
            {
                const GrammarKind kind = tree.Nodes()[current].kind;
                if (kind == GrammarKind::ParameterDeclaration || kind == GrammarKind::CompoundStatement ||
                    kind == GrammarKind::DeclarationStatement || kind == GrammarKind::ExpressionStatement ||
                    kind == GrammarKind::ReturnStatement || kind == GrammarKind::IfStatement ||
                    kind == GrammarKind::LoopStatement || kind == GrammarKind::SwitchStatement ||
                    kind == GrammarKind::CaseLabel || kind == GrammarKind::TryStatement ||
                    kind == GrammarKind::DoStatement || kind == GrammarKind::LambdaExpression)
                {
                    return CompletionKind::Variable;
                }

                if (kind == GrammarKind::FunctionDefinition || kind == GrammarKind::FunctionDeclaration)
                {
                    return CompletionKind::Function;
                }

                if (kind == GrammarKind::RecordDefinition || kind == GrammarKind::ConceptDefinition ||
                    kind == GrammarKind::TemplateDeclaration)
                {
                    if (tree.Nodes()[node_index].parent == current ||
                        tree.Nodes()[tree.Nodes()[node_index].parent].parent == current)
                    {
                        return CompletionKind::Type;
                    }

                    return CompletionKind::Variable;
                }

                if (kind == GrammarKind::Enumerator)
                {
                    return CompletionKind::Variable;
                }

                if (kind == GrammarKind::UsingDeclaration)
                {
                    return CompletionKind::Type;
                }

                if (kind == GrammarKind::Declaration)
                {
                    const GrammarNode &declaration = tree.Nodes()[current];
                    const std::size_t last = std::min<std::size_t>(declaration.first_token + declaration.token_count,
                        tree.Tokens().size());
                    std::size_t seen = 0;
                    for (std::size_t t = declaration.first_token; t < last && seen < 4; ++t)
                    {
                        const TokenKind token_kind = tree.Tokens()[t].kind;
                        if (token_kind == TokenKind::Whitespace || token_kind == TokenKind::LineComment ||
                            token_kind == TokenKind::BlockComment)
                        {
                            continue;
                        }

                        if (tree.Text(tree.Tokens()[t]) == "typedef")
                        {
                            return CompletionKind::Type;
                        }

                        ++seen;
                    }
                }

                current = tree.Nodes()[current].parent;
            }

            return CompletionKind::Variable;
        }

        void InsertItem(std::unordered_map<std::string, CompletionItem> & best, CompletionItem item)
        {
            if (item.label.empty() ||!IsIdentStart(item.label.front()))
            {
                return;
            }

            const auto found = best.find(item.label);
            if (found == best.end())
            {
                best.emplace(item.label, std::move(item));
                return;
            }

            CompletionItem &old = found->second;
            if (KindPriority(item.kind) > KindPriority(old.kind))
            {
                // The same symbol seen as a record scope (with its doc comment) and
                // as a bare tag/forward declaration (without one): keep the comment.
                if (item.documentation.empty())
                {
                    item.documentation = std::move(old.documentation);
                }

                if (item.type_text.empty())
                {
                    item.type_text = std::move(old.type_text);
                }

                old = std::move(item);
                return;
            }

            if (item.kind == old.kind)
            {
                // Same symbol seen twice (reopened namespace, declaration + definition):
                // fill in details the first sighting lacked.
                if (old.documentation.empty() && !item.documentation.empty())
                {
                    old.documentation = std::move(item.documentation);
                }

                if (old.type_text.empty() && !item.type_text.empty())
                {
                    old.type_text = std::move(item.type_text);
                }

                if (old.detail == KindDetail(old.kind) && item.detail != KindDetail(item.kind))
                {
                    old.detail = std::move(item.detail);
                }
            }
            else if (old.documentation.empty() && !item.documentation.empty())
            {
                // Lower-priority sighting (record scope) that carries the doc comment.
                old.documentation = std::move(item.documentation);
            }
        }

        // --- Scope-aware name resolution -------------------------------------------
        // Unqualified lookup walks the scope chain (block, function, namespace,
        // global) honoring the point of declaration; qualified lookup (`ns::name`)
        // resolves the qualifier to a scope node and lists its direct members.

        constexpr std::size_t NoIndex = static_cast<std::size_t>(-1);

        bool IsScopeKind(GrammarKind kind)
        {
            switch (kind)
            {
            case GrammarKind::TranslationUnit:
            case GrammarKind::NamespaceDefinition:
            case GrammarKind::RecordDefinition:
            case GrammarKind::FunctionDefinition:
            case GrammarKind::CompoundStatement:
            case GrammarKind::LambdaExpression:
                return true;
            default:
                return false;
            }
        }

        std::pair<std::size_t, std::size_t> NodeRange(const ParseTree &tree, std::size_t node)
        {
            if (node >= tree.Nodes().size()) return {0, 0};
            const GrammarNode &grammar = tree.Nodes()[node];
            if (grammar.token_count == 0 || grammar.first_token >= tree.Tokens().size()) return {0, 0};
            const std::size_t last = grammar.first_token + grammar.token_count <= tree.Tokens().size()
            ? grammar.first_token + grammar.token_count - 1
            : tree.Tokens().size() - 1;
            const Token &first = tree.Tokens()[grammar.first_token];
            const Token &last_token = tree.Tokens()[last];
            return {first.offset, last_token.offset + last_token.length};
        }

        bool IsLocalBoundary(GrammarKind kind)
        {
            switch (kind)
            {
            case GrammarKind::ParameterDeclaration:
            case GrammarKind::CompoundStatement:
            case GrammarKind::DeclarationStatement:
            case GrammarKind::ExpressionStatement:
            case GrammarKind::ReturnStatement:
            case GrammarKind::IfStatement:
            case GrammarKind::LoopStatement:
            case GrammarKind::SwitchStatement:
            case GrammarKind::CaseLabel:
            case GrammarKind::TryStatement:
            case GrammarKind::DoStatement:
            case GrammarKind::LambdaExpression:
                return true;
            default:
                return false;
            }
        }

        // Block-likes owning a local's visibility: the innermost compound (or the
        // loop/statement for initializers), not the declaration statement itself —
        // `long total = 0;` is usable for the rest of the block, not just its line.
        bool IsBlockBoundary(GrammarKind kind)
        {
            switch (kind)
            {
            case GrammarKind::CompoundStatement:
            case GrammarKind::LoopStatement:
            case GrammarKind::SwitchStatement:
            case GrammarKind::IfStatement:
            case GrammarKind::TryStatement:
            case GrammarKind::DoStatement:
            case GrammarKind::LambdaExpression:
                return true;
            default:
                return false;
            }
        }

        struct LocalInfo
        {
            bool is_local = false;
            // Enclosing function/lambda for parameters and body locals; NoIndex when
            // the name belongs to an outer scope (or to no function at all, such as
            // prototype parameters, which are never usable).
            std::size_t owner = NoIndex;
            // Innermost block owning a body local; the function itself for parameters.
            std::size_t boundary = NoIndex;
        };

        // Splits DeclaredName nodes into function parameters/body locals versus names
        // owned by an outer scope (the function's own name, globals, namespace and
        // record members). Lambda bodies count as function boundaries so their
        // parameters are modeled as locals of the lambda.
        LocalInfo AnalyzeLocal(const ParseTree &tree, std::size_t node)
        {
            LocalInfo info;
            if (node >= tree.Nodes().size())
            {
                return info;
            }

            std::size_t current = tree.Nodes()[node].parent;
            for (std::size_t depth = 0; depth < 32 && current < tree.Nodes().size(); ++depth)
            {
                const GrammarKind kind = tree.Nodes()[current].kind;
                if (kind == GrammarKind::FunctionDefinition || kind == GrammarKind::LambdaExpression)
                {
                    if (!info.is_local)
                    {
                        return info;
                    } // the function's own name

                    info.owner = current;
                    if (info.boundary == NoIndex)
                    {
                        info.boundary = current;
                    } // parameters: whole body

                    return info;
                }

                if (IsLocalBoundary(kind))
                {
                    info.is_local = true;
                    // The owning block (`for` initializers own their loop, `if`
                    // initializers their statement) — not the declaration line itself,
                    // which would hide later uses in the same block.
                    if (IsBlockBoundary(kind) && info.boundary == NoIndex)
                    {
                        info.boundary = current;
                    }
                }

                if (kind == GrammarKind::TranslationUnit || kind == GrammarKind::NamespaceDefinition ||
                    kind == GrammarKind::RecordDefinition)
                {
                    return info;
                }

                current = tree.Nodes()[current].parent;
            }

            return info;
        }

        bool IsTransparentForMembership(GrammarKind kind)
        {
            // Declarator plumbing between a name and the scope that owns it: the name
            // of a function defined in a namespace belongs to the namespace, while a
            // parameter or body local stops at ParameterDeclaration/CompoundStatement.
            switch (kind)
            {
            case GrammarKind::Declarator:
            case GrammarKind::InitDeclarator:
            case GrammarKind::Declaration:
            case GrammarKind::TypeSpecifier:
            case GrammarKind::FunctionSuffix:
            case GrammarKind::PointerOperator:
            case GrammarKind::NestedNameSpecifier:
            case GrammarKind::ArraySuffix:
            case GrammarKind::TrailingReturnType:
            case GrammarKind::NoexceptSpecifier:
            case GrammarKind::AttributeSpecifier:
            case GrammarKind::BitfieldSuffix:
            case GrammarKind::TemplateDeclaration:
            case GrammarKind::TemplateArgument:
            case GrammarKind::FunctionDefinition:
            case GrammarKind::FunctionDeclaration:
            case GrammarKind::Enumerator:
            case GrammarKind::RequiresClause:
            case GrammarKind::DeclaredName:
                return true;
            default:
                return false;
            }
        }

        // Scope node a name belongs to for qualified lookup: the namespace/record
        // that directly owns it (a function defined in a namespace belongs to the
        // namespace; its locals belong to body blocks instead).
        std::size_t MemberScope(const ParseTree &tree, std::size_t node)
        {
            if (node >= tree.Nodes().size())
            {
                return NoIndex;
            }

            std::size_t current = tree.Nodes()[node].parent;
            for (std::size_t depth = 0; depth < 32 && current < tree.Nodes().size(); ++depth)
            {
                if (IsTransparentForMembership(tree.Nodes()[current].kind))
                {
                    current = tree.Nodes()[current].parent;
                    continue;
                }

                return current;
            }

            return NoIndex;
        }

        // Name elements introducing a namespace/record scope node. Compound
        // definitions (`namespace a::b`) contribute several elements; anonymous scopes
        // contribute one empty element so they can never match a typed qualifier.
        std::vector<std::string> ScopeNameElements(const ParseTree &tree, std::size_t node)
        {
            const auto &nodes = tree.Nodes();
            if (node >= nodes.size()) return {
                {}};
            const GrammarKind kind = nodes[node].kind;
            if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
            {
                return {
                    {}};
            }

            const GrammarNode &grammar = nodes[node];
            std::vector<std::size_t> significant;
            const std::size_t end = std::min<std::size_t>(grammar.first_token + grammar.token_count,
                tree.Tokens().size());
            for (std::size_t i = grammar.first_token; i < end; ++i)
            {
                const TokenKind token_kind = tree.Tokens()[i].kind;
                if (token_kind == TokenKind::Whitespace || token_kind == TokenKind::LineComment ||
                    token_kind == TokenKind::BlockComment || tree.IsDecorationToken(i))
                {
                    continue;
                }

                significant.push_back(i);
            }

            auto text =[&](std::size_t token)->std::string_view
            {
                return tree.Text(tree.Tokens()[token]);
            };
            auto is_identifier =[&](std::size_t token)
            {
                return tree.Tokens()[token].kind == TokenKind::Identifier;
            };
            std::size_t pos = 0;
            if (kind == GrammarKind::NamespaceDefinition)
            {
                while (pos < significant.size() &&
                    (text(significant[pos]) == "export" || text(significant[pos]) == "inline"))
                {
                    ++pos;
                }

                if (pos >= significant.size() || text(significant[pos]) != "namespace") return {
                    {}};
                ++pos;
                // Anonymous (`{`), alias (`name = ...`), or a dotted name.
                std::vector<std::string> names;
                while (pos < significant.size() && is_identifier(significant[pos]))
                {
                    names.emplace_back(text(significant[pos]));
                    ++pos;
                    if (pos + 1 < significant.size() && text(significant[pos]) == "::" &&
                        is_identifier(significant[pos + 1]))
                    {
                        ++pos;
                        continue;
                    }

                    break;
                }

                return names.empty() ? std::vector<std::string>{
                    {}}: names;
            }

            for (; pos < significant.size(); ++pos)
            {
                const std::string_view word = text(significant[pos]);
                if (word != "struct" && word != "class" && word != "union" && word != "enum")
                {
                    continue;
                }

                ++pos;
                if (word == "enum" && pos < significant.size() &&
                    (text(significant[pos]) == "class" || text(significant[pos]) == "struct"))
                {
                    ++pos;
                }

                if (pos < significant.size() && is_identifier(significant[pos]))
                {
                    return {std::string(text(significant[pos]))};
                }

                return {
                    {}};
            }

            return {
                {}};
        }

        // Whether a namespace definition is `inline namespace` (members visible as
        // direct members of the enclosing scope).
        bool IsInlineNamespace(const ParseTree &tree, std::size_t node)
        {
            const auto &nodes = tree.Nodes();
            if (node >= nodes.size() || nodes[node].kind != GrammarKind::NamespaceDefinition)
            {
                return false;
            }

            const GrammarNode &grammar = nodes[node];
            const std::size_t end = std::min<std::size_t>(grammar.first_token + grammar.token_count,
                tree.Tokens().size());
            bool seen_inline = false;
            for (std::size_t i = grammar.first_token; i < end; ++i)
            {
                const Token &token = tree.Tokens()[i];
                if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                    token.kind == TokenKind::BlockComment)
                {
                    continue;
                }

                const std::string_view word = tree.Text(token);
                if (word == "inline")
                {
                    seen_inline = true;
                    continue;
                }

                if (word == "export")
                {
                    continue;
                }

                return seen_inline && word == "namespace";
            }

            return false;
        }

        // Qualified path of a scope node from the translation unit down, e.g.
        // `outer::inner`. Function-like levels cannot be named from the outside and
        // are skipped, so a function-local struct still resolves by its tag. Inline
        // namespaces are transparent (their members are visible as direct members of
        // the enclosing scope), which is what makes e.g. libc++ `std::__1::vector`
        // resolve as `std::vector`.
        std::vector<std::string> ScopePath(const ParseTree &tree, std::size_t node)
        {
            std::vector<std::string> path;
            std::size_t current = node;
            for (std::size_t depth = 0; depth < 32 && current < tree.Nodes().size(); ++depth)
            {
                const GrammarKind kind = tree.Nodes()[current].kind;
                if (kind == GrammarKind::TranslationUnit)
                {
                    break;
                }

                if (kind == GrammarKind::NamespaceDefinition || kind == GrammarKind::RecordDefinition)
                {
                    if (!IsInlineNamespace(tree, current))
                    {
                        const auto elements = ScopeNameElements(tree, current);
                        path.insert(path.begin(), elements.begin(), elements.end());
                    }
                }

                current = tree.Nodes()[current].parent;
            }

            return path;
        }

        struct Qualifier
        {
            std::vector<std::string> path;
            bool global = false;     // leading `::` with no names (`::name`)
            bool unresolved = false; // modeled as nothing (`A<int>::x`, `call()::y`)
        };

        // Names in `a::b::` before the final `::` at scope_op.
        Qualifier QualifierBefore(std::string_view source, const std::vector<Token> & tokens,
            std::size_t scope_op)
        {
            Qualifier result;
            std::size_t current = scope_op;
            while (true)
            {
                const std::size_t name = PreviousSignificant(tokens, current, source);
                if (name >= tokens.size() || tokens[name].kind != TokenKind::Identifier)
                {
                    break;
                }

                result.path.insert(result.path.begin(), std::string(TokenText(source, tokens[name])));
                const std::size_t separator = PreviousSignificant(tokens, name, source);
                if (separator >= tokens.size() || tokens[separator].kind != TokenKind::Punctuation ||
                    TokenText(source, tokens[separator]) != "::")
                {
                    break;
                }

                current = separator;
            }

            if (!result.path.empty())
            {
                return result;
            }

            // No identifiers: either a global qualifier (`::name`) or something this
            // engine cannot model. A closing bracket hints at the latter
            // (`A<int>::x`); anything else is the global scope.
            const std::size_t prev = PreviousSignificant(tokens, scope_op, source);
            if (prev < tokens.size() && tokens[prev].kind == TokenKind::Punctuation)
            {
                const std::string_view text = TokenText(source, tokens[prev]);
                if (text == ">" || text == ">>" || text == ")" || text == "]")
                {
                    result.unresolved = true;
                }
                else
                {
                    result.global = true;
                }
            }
            else
            {
                result.global = true;
            }

            return result;
        }

        // Scope nodes whose qualified path equals the qualifier. Namespaces reopened
        // in several blocks naturally yield several targets whose members unite.
        std::vector<std::size_t> ResolveScope(const std::vector<std::vector<std::string>> & scope_paths,
            const std::vector<std::string> & path)
        {
            std::vector<std::size_t> targets;
            for (std::size_t n = 0; n < scope_paths.size(); ++n)
            {
                if (scope_paths[n].empty() && path.empty())
                {
                    // The translation unit itself has no entry in scope_paths (only
                    // namespace/record nodes are cached); the root is handled by
                    // callers via MemberScope() == RootNode checks, so skip empties.
                    continue;
                }

                if (scope_paths[n] == path)
                {
                    targets.push_back(n);
                }
            }

            return targets;
        }

        // Qualified path of every namespace/record node, indexed by node. Built once
        // per completion request: ResolveScope, CollectChildScopes and the namespace
        // loops previously recomputed ScopePath per node on each pass.
        std::vector<std::vector<std::string>> BuildScopePaths(const ParseTree &tree)
        {
            std::vector<std::vector<std::string>> paths(tree.Nodes().size());
            for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
            {
                const GrammarKind kind = tree.Nodes()[n].kind;
                if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
                {
                    continue;
                }

                paths[n] = ScopePath(tree, n);
            }

            return paths;
        }

        // Contiguous function/lambda ranges, sorted by start offset. Built once per
        // completion request so occurrence visibility is a binary search per token
        // instead of the previous O(TxN) InnermostCallable scan per identifier.
        struct CallableInterval
        {
            std::size_t start = 0;
            std::size_t end = 0;
            std::size_t node = NoIndex;
        };

        std::vector<CallableInterval> BuildCallableIntervals(const ParseTree &tree)
        {
            std::vector<CallableInterval> intervals;
            intervals.reserve(tree.Nodes().size() / 8);
            for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
            {
                const GrammarKind kind = tree.Nodes()[n].kind;
                if (kind != GrammarKind::FunctionDefinition && kind != GrammarKind::LambdaExpression)
                {
                    continue;
                }

                const auto[start, end] = NodeRange(tree, n);
                if (end <= start && start != 0)
                {
                    continue;
                }

                if (start == 0 && end == 0)
                {
                    continue;
                }

                intervals.push_back({start, end, n});
            }

            std::sort(intervals.begin(), intervals.end(),[](const CallableInterval &left,
                const CallableInterval &right)
                {
                    if (left.start != right.start)
                    {
                        return left.start < right.start;
                }

                    return left.end > right.end;
            });
            return intervals;
        }

        // Innermost function/lambda range containing pos (inclusive end, matching the
        // old linear scan). Intervals nest, so walking back from the last start<=pos
        // finds the innermost container with the first end>=pos hit.
        std::size_t FindCallable(const std::vector<CallableInterval> & intervals, std::size_t pos)
        {
            std::size_t lo = 0;
            std::size_t hi = intervals.size();
            while (lo < hi)
            {
                const std::size_t mid = lo +(hi - lo) / 2;
                if (intervals[mid].start <= pos)
                {
                    lo = mid + 1;
                }
                else
                {
                    hi = mid;
                }
            }

            for (std::size_t k = lo; k > 0; --k)
            {
                const auto &interval = intervals[k - 1];
                if (pos <= interval.end)
                {
                    return interval.node;
                }
            }

            return NoIndex;
        }

        // Named namespace/record ranges for the `!InNamedScope` global-qualifier
        // filter, built once per request instead of one O(N) scan per token.
        struct ScopeInterval
        {
            std::size_t start = 0;
            std::size_t end = 0;
        };

        std::vector<ScopeInterval> BuildNamedScopeIntervals(const ParseTree &tree)
        {
            std::vector<ScopeInterval> intervals;
            for (std::size_t n = 1; n < tree.Nodes().size(); ++n)
            {
                const GrammarKind kind = tree.Nodes()[n].kind;
                if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
                {
                    continue;
                }

                const auto[start, end] = NodeRange(tree, n);
                if (end <= start)
                {
                    continue;
                }

                intervals.push_back({start, end});
            }

            return intervals;
        }

        bool InNamedScopeIntervals(const std::vector<ScopeInterval> & intervals, std::size_t offset)
        {
            for (const auto & interval: intervals)
            {
                if (interval.start <= offset && offset < interval.end)
                {
                    return true;
                }
            }

            return false;
        }

        // The grammar does not always materialize the tag name itself as a
        // DeclaredName (e.g. `struct Widget { ... };`), so collect tag names
        // lexically as a fallback: `class/struct/union/enum [class/struct] Name`
        // plus `using Name` and `concept Name`. Mirrors SemanticAnalyzer::
        // CollectTypeNames. The range restriction serves qualified lookup, which
        // only wants tags nested directly in the resolved scope.
        struct TagName
        {
            std::string_view text;
            std::size_t offset = 0;
            // Introducing keyword (`struct`, `enum class`, `using`, `concept`) and
            // its token index, for `struct Widget`-style details and doc anchoring.
            std::string_view intro;
            std::size_t intro_token = 0;
            std::size_t name_token = 0;
        };

        struct Define
        {
            std::string_view name;
            std::string value; // trimmed rest of the line (may hold `(args) replacement`)
            std::size_t hash_token = 0;
        };

        void ScanTagNames(std::string_view source, const std::vector<Token> & tokens,
            std::vector<TagName> & out, const ParseTree *tree = nullptr)
        {
            struct Word
            {
                std::string_view text;
                std::size_t offset = 0;
                std::size_t token = 0;
            };

            std::vector<Word> words;
            words.reserve(tokens.size());
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                const Token &token = tokens[i];
                if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                    token.kind == TokenKind::BlockComment ||(tree != nullptr && tree->IsDecorationToken(i)))
                {
                    continue;
                }

                words.push_back({TokenText(source, token), token.offset, i});
            }

            auto is_word =[](std::string_view value)
            {
                return!value.empty() && IsIdentStart(value.front());
            };
            for (std::size_t i = 0; i < words.size(); ++i)
            {
                auto emit =[&](std::size_t index, std::string_view intro, std::size_t intro_token)
                {
                    if (index < words.size() && is_word(words[index].text))
                    {
                        out.push_back({words[index].text, words[index].offset, intro, intro_token,
                                words[index].token});
                    }
                };
                if (words[i].text == "using" || words[i].text == "concept")
                {
                    emit(i + 1, words[i].text, words[i].token);
                }
                else if (words[i].text == "class" || words[i].text == "struct" || words[i].text == "union" ||
                    words[i].text == "enum")
                {
                    std::size_t name = i + 1;
                    std::string_view intro = words[i].text;
                    std::size_t intro_token = words[i].token;
                    if (words[i].text == "enum" && name < words.size() &&
                        (words[name].text == "class" || words[name].text == "struct"))
                    {
                        intro = source.substr(words[i].offset, words[name].offset + words[name].text.size() -
                            words[i].offset);
                        intro_token = words[i].token;
                        ++name;
                    }

                    emit(name, intro, intro_token);
                }
            }
        }

        // `#define` scan with replacement text and `#` anchor for documentation.
        // Function-like macros keep their `(params)` in the value.
        void ScanDefines(std::string_view source, const std::vector<Token> & tokens,
            std::vector<Define> & out)
        {
            for (std::size_t i = 0; i + 2 < tokens.size(); ++i)
            {
                if (tokens[i].kind != TokenKind::Punctuation)
                {
                    continue;
                }

                if (TokenText(source, tokens[i]) != "#")
                {
                    continue;
                }

                std::size_t j = i + 1;
                while (j < tokens.size() && tokens[j].kind == TokenKind::Whitespace)
                {
                    ++j;
                }

                if (j >= tokens.size() || tokens[j].kind != TokenKind::Identifier)
                {
                    continue;
                }

                if (TokenText(source, tokens[j]) != "define")
                {
                    continue;
                }

                ++j;
                while (j < tokens.size() && tokens[j].kind == TokenKind::Whitespace)
                {
                    ++j;
                }

                if (j >= tokens.size() || tokens[j].kind != TokenKind::Identifier)
                {
                    continue;
                }

                const Token &name_token = tokens[j];
                const std::size_t value_start = name_token.offset + name_token.length;
                std::size_t line_end = value_start;
                while (line_end < source.size() && source[line_end] != '\n')
                {
                    ++line_end;
                }

                std::string_view value = source.substr(value_start, line_end - value_start);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                {
                    value.remove_prefix(1);
                }

                while (!value.empty() &&
                    (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
                {
                    value.remove_suffix(1);
                }

                out.push_back({TokenText(source, name_token), std::string(value), i});
            }
        }

        // --- Symbol description (detail + documentation) ---------------------------

        // First direct child of `parent` with kind, or NoIndex. Uses the tree's
        // pre-order subtree range instead of a per-request children table.
        std::size_t FindChild(const ParseTree &tree, std::size_t parent, GrammarKind kind)
        {
            if (parent >= tree.Nodes().size())
            {
                return NoIndex;
            }

            const std::size_t end = tree.Nodes()[parent].subtree_end < tree.Nodes().size()
            ? tree.Nodes()[parent].subtree_end
            : tree.Nodes().size();
            for (std::size_t i = parent + 1; i < end; ++i)
            {
                if (tree.Nodes()[i].parent == parent && tree.Nodes()[i].kind == kind)
                {
                    return i;
                }
            }

            return NoIndex;
        }

        // First pre-order descendant of `node` with kind, or NoIndex. Subtrees occupy
        // contiguous index ranges, so a single linear scan needs no auxiliary index
        // and allocates nothing.
        std::size_t FindInSubtree(const ParseTree &tree, std::size_t node, GrammarKind kind)
        {
            if (node >= tree.Nodes().size())
            {
                return NoIndex;
            }

            const std::size_t end = tree.Nodes()[node].subtree_end < tree.Nodes().size()
            ? tree.Nodes()[node].subtree_end
            : tree.Nodes().size();
            for (std::size_t i = node + 1; i < end; ++i)
            {
                if (tree.Nodes()[i].kind == kind)
                {
                    return i;
                }
            }

            return NoIndex;
        }

        // Exact source slice of a token range (ranges retain trivia).
        std::string SliceRange(const ParseTree &tree, std::size_t first_token, std::size_t token_count)
        {
            if (token_count == 0 || first_token >= tree.Tokens().size()) return {};
            const std::size_t last =
                std::min<std::size_t>(static_cast<std::size_t>(first_token) + token_count,
                tree.Tokens().size()) - 1;
            const Token &first = tree.Tokens()[first_token];
            const Token &last_token = tree.Tokens()[last];
            return std::string(
                tree.Source().substr(first.offset, last_token.offset + last_token.length - first.offset));
        }

        // Single-line, capped copy for `detail` fields.
        std::string CompactWs(std::string text, std::size_t cap)
        {
            std::string out;
            out.reserve(std::min(text.size(), cap));
            bool space = true; // trim leading whitespace
            for (const char c: text)
            {
                const bool blank = c == ' ' || c == '\t' || c == '\n' || c == '\r';
                if (blank)
                {
                    if (!space && out.size() < cap)
                    {
                        out += ' ';
                    }

                    space = true;
                }
                else
                {
                    if (out.size() >= cap)
                    {
                        break;
                    }

                    out += c;
                    space = false;
                }
            }

            while (!out.empty() && out.back() == ' ')
            {
                out.pop_back();
            }

            return out;
        }

        // Nearest Declarator ancestor of a DeclaredName, or NoIndex.
        std::size_t DeclaratorOf(const ParseTree &tree, std::size_t node)
        {
            if (node >= tree.Nodes().size())
            {
                return NoIndex;
            }

            std::size_t current = tree.Nodes()[node].parent;
            for (std::size_t depth = 0; depth < 8 && current < tree.Nodes().size(); ++depth)
            {
                const GrammarKind kind = tree.Nodes()[current].kind;
                if (kind == GrammarKind::Declarator)
                {
                    return current;
                }

                if (kind == GrammarKind::FunctionDefinition || kind == GrammarKind::FunctionDeclaration ||
                    kind == GrammarKind::TranslationUnit || kind == GrammarKind::NamespaceDefinition ||
                    kind == GrammarKind::RecordDefinition || kind == GrammarKind::CompoundStatement)
                {
                    return NoIndex;
                }

                current = tree.Nodes()[current].parent;
            }

            return NoIndex;
        }

        // `int add(int left, int right)` for a function name, or empty when the shape
        // is unexpected (callers fall back to the plain kind detail).
        std::string FunctionSignature(const ParseTree &tree, std::size_t node, std::string_view name)
        {
            const std::size_t declarator = DeclaratorOf(tree, node);
            if (declarator == NoIndex) return {};
            std::string params;
            const std::size_t suffix = FindInSubtree(tree, declarator, GrammarKind::FunctionSuffix);
            if (suffix != NoIndex)
            {
                const auto &suffix_node = tree.Nodes()[suffix];
                params = CompactWs(SliceRange(tree, suffix_node.first_token, suffix_node.token_count), 200);
            }

            std::string trailing;
            const std::size_t trailing_node =
                FindInSubtree(tree, declarator, GrammarKind::TrailingReturnType);
            if (trailing_node != NoIndex)
            {
                const auto &trailing_ref = tree.Nodes()[trailing_node];
                trailing = CompactWs(SliceRange(tree, trailing_ref.first_token, trailing_ref.token_count), 64);
            }

            std::string returns;
            std::size_t function = tree.Nodes()[declarator].parent;
            for (std::size_t depth = 0; depth < 8 && function < tree.Nodes().size(); ++depth)
            {
                const GrammarKind kind = tree.Nodes()[function].kind;
                if (kind == GrammarKind::FunctionDefinition || kind == GrammarKind::FunctionDeclaration)
                {
                    break;
                }

                if (kind == GrammarKind::TranslationUnit || kind == GrammarKind::NamespaceDefinition ||
                    kind == GrammarKind::RecordDefinition || kind == GrammarKind::CompoundStatement)
                {
                    function = NoIndex;
                    break;
                }

                function = tree.Nodes()[function].parent;
            }

            if (function != NoIndex)
            {
                const std::size_t type = FindChild(tree, function, GrammarKind::TypeSpecifier);
                if (type != NoIndex)
                {
                    const auto &type_node = tree.Nodes()[type];
                    returns = CompactWs(SliceRange(tree, type_node.first_token, type_node.token_count), 96);
                }
            }

            std::string signature;
            if (!returns.empty())
            {
                signature += returns;
                signature += ' ';
            }

            signature += name;
            signature += params.empty() ? "()" : params;
            if (!trailing.empty())
            {
                signature += ' ';
                signature += trailing;
            }

            if (signature.size() > 256)
            {
                signature.resize(256);
            }

            return signature;
        }

        // Declared type of a variable (`int`, `Widget`, ...), or empty.
        std::string VariableTypeDetail(const ParseTree &tree, std::size_t node)
        {
            std::size_t current = tree.Nodes()[node].parent;
            for (std::size_t depth = 0; depth < 8 && current < tree.Nodes().size(); ++depth)
            {
                const GrammarKind kind = tree.Nodes()[current].kind;
                if (kind == GrammarKind::Declarator || kind == GrammarKind::PointerOperator ||
                    kind == GrammarKind::ArraySuffix || kind == GrammarKind::TypeSpecifier ||
                    kind == GrammarKind::InitDeclarator)
                {
                    // Declarator plumbing (shared specifiers live further up).
                    current = tree.Nodes()[current].parent;
                    continue;
                }

                if (kind == GrammarKind::ParameterDeclaration || kind == GrammarKind::Declaration ||
                    kind == GrammarKind::DeclarationStatement)
                {
                    const std::size_t type = FindChild(tree, current, GrammarKind::TypeSpecifier);
                    if (type == NoIndex) return {};
                    const auto &type_node = tree.Nodes()[type];
                    return CompactWs(SliceRange(tree, type_node.first_token, type_node.token_count), 128);
                }

                return {};
            }

            return {};
        }

        // `Color::Red`-style detail for enumerators, or empty when the name has no
        // Enumerator ancestor.
        std::string EnumeratorDetail(const ParseTree &tree, std::size_t node, std::string_view name)
        {
            if (node >= tree.Nodes().size()) return {};
            std::size_t current = tree.Nodes()[node].parent;
            for (std::size_t depth = 0; depth < 6 && current < tree.Nodes().size(); ++depth)
            {
                const GrammarKind kind = tree.Nodes()[current].kind;
                if (kind == GrammarKind::Enumerator)
                {
                    const std::size_t record = tree.Nodes()[current].parent;
                    std::string out;
                    if (record < tree.Nodes().size() &&
                        tree.Nodes()[record].kind == GrammarKind::RecordDefinition)
                    {
                        for (const auto & element: ScopePath(tree, record))
                        {
                            if (element.empty())
                            {
                                continue;
                            }

                            if (!out.empty())
                            {
                                out += "::";
                            }

                            out += element;
                        }
                    }

                    if (!out.empty())
                    {
                        out += "::";
                    }

                    out += name;
                    return out;
                }

                if (!IsTransparentForMembership(kind)) return {};
                current = tree.Nodes()[current].parent;
            }

            return {};
        }

        bool IsDocLineComment(std::string_view text)
        {
            return text.starts_with("///") || text.starts_with("//!");
        }

        bool IsDocBlockComment(std::string_view text)
        {
            return text.size() > 5 && (text.starts_with("/**") || text.starts_with("/*!"));
        }

        std::string CleanBlockComment(std::string_view text)
        {
            // Strip the opening `/**` / `/*!` and closing `*/`, then one leading `*`
            // per line (doxygen style).
            std::string_view inner = text.substr(2, text.size() > 4 ? text.size() - 4 : 0);
            std::string out;
            std::size_t pos = 0;
            while (pos <= inner.size())
            {
                std::size_t end = inner.find('\n', pos);
                if (end == std::string_view::npos)
                {
                    end = inner.size();
                }

                std::string_view line = inner.substr(pos, end - pos);
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                {
                    line.remove_prefix(1);
                }

                if (!line.empty() && line.front() == '*')
                {
                    line.remove_prefix(1);
                }

                if (!line.empty() && line.front() == ' ')
                {
                    line.remove_prefix(1);
                }

                while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                {
                    line.remove_suffix(1);
                }

                if (!out.empty())
                {
                    out += '\n';
                }

                out += line;
                pos = end + 1;
            }

            while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
            {
                out.pop_back();
            }

            return out;
        }

        // True when the comment token starts its own line (nothing but whitespace
        // before it): a trailing `int a; // note` belongs to `a`, not to the
        // declaration below it.
        bool CommentStartsLine(std::string_view source, const std::vector<Token> & tokens,
            std::size_t index)
        {
            if (index == 0)
            {
                return true;
            }

            const Token &previous = tokens[index - 1];
            if (previous.kind != TokenKind::Whitespace)
            {
                return false;
            }

            const std::string_view text = TokenText(source, previous);
            return text.find('\n') != std::string_view::npos || index == 1;
        }

        // Documentation attached to the token at anchor_token: the contiguous
        // comments directly above it, broken by a blank line or any code. Doxygen
        // forms (`///`, `//!`, `/**`, `/*!`) always count; ordinary `//` and `/* */`
        // comments count when they sit on their own line(s) right above, which is
        // how most code documents declarations.
        std::string DocCommentFor(std::string_view source, const std::vector<Token> & tokens,
            std::size_t anchor_token)
        {
            if (anchor_token == 0 || anchor_token > tokens.size()) return {};
            constexpr std::size_t kCap = 1000;
            std::vector<std::string> lines;
            std::size_t total = 0;
            for (std::size_t j = anchor_token; j > 0;)
            {
                --j;
                const Token &token = tokens[j];
                const std::string_view text = TokenText(source, token);
                if (token.kind == TokenKind::Whitespace)
                {
                    if (std::count(text.begin(), text.end(), '\n') >= 2)
                    {
                        break;
                    } // blank line

                    continue;
                }

                if (token.kind == TokenKind::LineComment && !IsDocLineComment(text))
                {
                    if (!CommentStartsLine(source, tokens, j))
                    {
                        break;
                    }

                    std::string_view content = text.substr(2);
                    if (!content.empty() && content.front() == ' ')
                    {
                        content.remove_prefix(1);
                    }

                    while (!content.empty() && (content.back() == ' ' || content.back() == '\r'))
                    {
                        content.remove_suffix(1);
                    }

                    if (total + content.size() > kCap)
                    {
                        break;
                    }

                    lines.push_back(std::string(content));
                    total += content.size() + 1;
                    continue;
                }

                if (token.kind == TokenKind::LineComment && IsDocLineComment(text))
                {
                    std::string_view content = text.substr(3);
                    if (!content.empty() && content.front() == ' ')
                    {
                        content.remove_prefix(1);
                    }

                    if (total + content.size() > kCap)
                    {
                        break;
                    }

                    lines.push_back(std::string(content));
                    total += content.size() + 1;
                    continue;
                }

                if (token.kind == TokenKind::BlockComment && text.size() > 4 &&
                    (IsDocBlockComment(text) || CommentStartsLine(source, tokens, j)))
                {
                    const std::string block = CleanBlockComment(text);
                    if (block.empty())
                    {
                        break;
                    }

                    if (total + block.size() > kCap)
                    {
                        break;
                    }

                    lines.push_back(block);
                    total += block.size() + 1;
                    continue;
                }

                break;
            }

            std::string out;
            for (auto it = lines.rbegin(); it != lines.rend(); ++it)
            {
                if (!out.empty())
                {
                    out += '\n';
                }

                out += *it;
            }

            while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
            {
                out.pop_back();
            }

            return out;
        }

        // First token of the outermost declaration owning a DeclaredName (through
        // `template<...>`, specifiers and declarators, stopping at scope owners), so
        // documentation sitting above the whole declaration attaches.
        std::size_t DocAnchorToken(const ParseTree &tree, std::size_t node)
        {
            if (node >= tree.Nodes().size())
            {
                return 0;
            }

            std::size_t current = node;
            for (std::size_t depth = 0; depth < 16; ++depth)
            {
                const std::size_t parent = tree.Nodes()[current].parent;
                if (parent >= tree.Nodes().size() || parent == current)
                {
                    break;
                }

                switch (tree.Nodes()[parent].kind)
                {
                case GrammarKind::Declarator:
                case GrammarKind::InitDeclarator:
                case GrammarKind::Declaration:
                case GrammarKind::ParameterDeclaration:
                case GrammarKind::FunctionDefinition:
                case GrammarKind::FunctionDeclaration:
                case GrammarKind::TemplateDeclaration:
                case GrammarKind::TypeSpecifier:
                case GrammarKind::UsingDeclaration:
                case GrammarKind::ConceptDefinition:
                case GrammarKind::Enumerator:
                    current = parent;
                    continue;
                default:
                    break;
                }

                break;
            }

            return tree.Nodes()[current].first_token;
        }

        // Return type of the function a declared name belongs to (leading type or
        // trailing `-> T`), or empty.
        std::string ReturnTypeText(const ParseTree &tree, std::size_t node)
        {
            const std::size_t declarator = DeclaratorOf(tree, node);
            if (declarator == NoIndex)
            {
                return {};
            }

            const std::size_t trailing = FindInSubtree(tree, declarator, GrammarKind::TrailingReturnType);
            if (trailing != NoIndex)
            {
                std::string text = CompactWs(SliceRange(tree, tree.Nodes()[trailing].first_token,
                    tree.Nodes()[trailing].token_count), 128);
                if (text.starts_with("->"))
                {
                    text.erase(0, 2);
                }

                while (!text.empty() && text.front() == ' ')
                {
                    text.erase(0, 1);
                }

                if (!text.empty())
                {
                    return text;
                }
            }

            std::size_t function = tree.Nodes()[declarator].parent;
            for (std::size_t depth = 0; depth < 8 && function < tree.Nodes().size(); ++depth)
            {
                const GrammarKind kind = tree.Nodes()[function].kind;
                if (kind == GrammarKind::FunctionDefinition || kind == GrammarKind::FunctionDeclaration)
                {
                    const std::size_t type = FindChild(tree, function, GrammarKind::TypeSpecifier);
                    if (type == NoIndex)
                    {
                        return {};
                    }

                    return CompactWs(SliceRange(tree, tree.Nodes()[type].first_token,
                        tree.Nodes()[type].token_count), 128);
                }

                if (kind == GrammarKind::TranslationUnit || kind == GrammarKind::NamespaceDefinition ||
                    kind == GrammarKind::RecordDefinition || kind == GrammarKind::CompoundStatement)
                {
                    return {};
                }

                function = tree.Nodes()[function].parent;
            }

            return {};
        }

        // Aliased type of `using Name = T;` / `typedef T Name;`, or empty when the
        // name is not an alias (records, `using ns::name;`).
        std::string AliasTargetText(const ParseTree &tree, std::size_t node)
        {
            std::size_t current = tree.Nodes()[node].parent;
            for (std::size_t depth = 0; depth < 8 && current < tree.Nodes().size(); ++depth)
            {
                const GrammarKind kind = tree.Nodes()[current].kind;
                if (kind == GrammarKind::UsingDeclaration)
                {
                    const auto &grammar = tree.Nodes()[current];
                    const std::size_t last = std::min<std::size_t>(grammar.first_token + grammar.token_count,
                        tree.Tokens().size());
                    for (std::size_t i = grammar.first_token; i < last; ++i)
                    {
                        if (tree.Tokens()[i].kind == TokenKind::Punctuation && tree.Text(tree.Tokens()[i]) == "=")
                        {
                            std::string text = CompactWs(SliceRange(tree, static_cast<std::uint32_t>(i + 1),
                                last -(i + 1)), 160);
                            while (!text.empty() && (text.back() == ';' || text.back() == ' '))
                            {
                                text.pop_back();
                            }

                            return text;
                        }
                    }

                    return {};
                }

                if (kind == GrammarKind::Declaration)
                {
                    const std::size_t type = FindChild(tree, current, GrammarKind::TypeSpecifier);
                    if (type == NoIndex)
                    {
                        return {};
                    }

                    return CompactWs(SliceRange(tree, tree.Nodes()[type].first_token,
                        tree.Nodes()[type].token_count), 160);
                }

                if (kind == GrammarKind::RecordDefinition || kind == GrammarKind::CompoundStatement ||
                    kind == GrammarKind::TranslationUnit || kind == GrammarKind::NamespaceDefinition)
                {
                    return {};
                }

                current = tree.Nodes()[current].parent;
            }

            return {};
        }

        // Base classes of a record node as written (`ns::Base`, args dropped).
        std::vector<std::string> RecordBases(const ParseTree &tree, std::size_t node)
        {
            std::vector<std::string> bases;
            const auto &grammar = tree.Nodes()[node];
            const std::size_t last = std::min<std::size_t>(grammar.first_token + grammar.token_count,
                tree.Tokens().size());
            std::size_t i = grammar.first_token;
            for (; i < last; ++i)
            {
                const Token &token = tree.Tokens()[i];
                if (token.kind != TokenKind::Punctuation)
                {
                    continue;
                }

                const std::string_view text = tree.Text(token);
                if (text == "{" || text == ";")
                {
                    return bases;
                }

                if (text == ":")
                {
                    break;
                }
            }

            std::string current;
            int angle = 0;
            for (++i; i < last; ++i)
            {
                const Token &token = tree.Tokens()[i];
                if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                    token.kind == TokenKind::BlockComment)
                {
                    continue;
                }

                const std::string_view text = tree.Text(token);
                if (token.kind == TokenKind::Punctuation)
                {
                    if (text == "{")
                    {
                        break;
                    }

                    if (text == "<")
                    {
                        ++angle;
                    }
                    else if (text == ">")
                    {
                        angle = std::max(0, angle - 1);
                    }
                    else if (text == ">>")
                    {
                        angle = std::max(0, angle - 2);
                    }
                    else if (text == "," && angle == 0)
                    {
                        if (!current.empty())
                        {
                            bases.push_back(std::move(current));
                            current.clear();
                        }
                    }
                    else if (text == "::" && angle == 0)
                    {
                        current += "::";
                    }

                    continue;
                }

                if (angle > 0 || token.kind != TokenKind::Identifier)
                {
                    continue;
                }

                if (text == "public" || text == "private" || text == "protected" || text == "virtual" ||
                    text == "typename")
                {
                    continue;
                }

                if (!current.empty() && !current.ends_with("::"))
                {
                    continue; // `final`, attributes
                }

                current += text;
            }

            if (!current.empty())
            {
                bases.push_back(std::move(current));
            }

            return bases;
        }

        CompletionItem DescribeDeclared(const ParseTree &tree, std::string_view source,
            const std::vector<Token> & tokens, std::size_t node,
            std::string_view name, CompletionKind kind)
        {
            std::string detail = KindDetail(kind);
            if (kind == CompletionKind::Function)
            {
                const std::string signature = FunctionSignature(tree, node, name);
                if (!signature.empty())
                {
                    detail = signature;
                }
            }
            else if (kind == CompletionKind::Variable)
            {
                const std::string scoped = EnumeratorDetail(tree, node, name);
                if (!scoped.empty())
                {
                    detail = scoped;
                }
                else
                {
                    const std::string type = VariableTypeDetail(tree, node);
                    if (!type.empty())
                    {
                        detail = type;
                    }
                }
            }

            if (detail.size() > 256)
            {
                detail.resize(256);
            }

            std::string documentation = DocCommentFor(source, tokens, DocAnchorToken(tree, node));
            if (documentation.size() > 1000)
            {
                documentation.resize(1000);
            }

            CompletionItem item{std::string(name), kind, std::move(detail), std::move(documentation)};
            if (kind == CompletionKind::Variable)
            {
                item.type_text = VariableTypeDetail(tree, node);
            }
            else if (kind == CompletionKind::Function)
            {
                item.type_text = ReturnTypeText(tree, node);
            }
            else if (kind == CompletionKind::Type)
            {
                item.type_text = AliasTargetText(tree, node);
            }

            const std::size_t token = tree.Nodes()[node].first_token;
            if (token < tokens.size())
            {
                item.has_location = true;
                item.offset = tokens[token].offset;
            }

            item.is_definition = kind != CompletionKind::Function;
            if (kind == CompletionKind::Function)
            {
                const std::size_t declarator = DeclaratorOf(tree, node);
                if (declarator != NoIndex)
                {
                    const std::size_t owner = tree.Nodes()[declarator].parent;
                    item.is_definition = owner < tree.Nodes().size() &&
                        tree.Nodes()[owner].kind == GrammarKind::FunctionDefinition;
                    const std::size_t suffix = FindInSubtree(tree, declarator, GrammarKind::FunctionSuffix);
                    if (suffix != NoIndex)
                    {
                        for (const std::size_t child: tree.Children(suffix))
                        {
                            if (tree.Nodes()[child].kind == GrammarKind::ParameterDeclaration)
                            {
                                ++item.param_count;
                            }
                        }
                    }
                }
            }

            return item;
        }

        // For `template<...> struct S`, the documentation sits above `template`, not
        // above `struct`. Walks back over one balanced `<>` group to the `template`
        // keyword; returns intro_token unchanged when the shape differs.
        std::size_t TemplateHeadBefore(std::string_view source, const std::vector<Token> & tokens,
            std::size_t intro_token)
        {
            std::size_t prev = PreviousSignificant(tokens, intro_token, source);
            if (prev >= tokens.size() || tokens[prev].kind != TokenKind::Punctuation)
            {
                return intro_token;
            }

            const std::string_view closer = TokenText(source, tokens[prev]);
            int depth = 0;
            if (closer == ">")
            {
                depth = 1;
            }
            else if (closer == ">>")
            {
                depth = 2;
            }
            else
            {
                return intro_token;
            }

            std::size_t j = prev;
            while (j > 0)
            {
                --j;
                if (tokens[j].kind == TokenKind::Whitespace || tokens[j].kind == TokenKind::LineComment ||
                    tokens[j].kind == TokenKind::BlockComment)
                {
                    continue;
                }

                if (tokens[j].kind != TokenKind::Punctuation)
                {
                    continue;
                }

                const std::string_view text = TokenText(source, tokens[j]);
                if (text == ">" || text == ">=")
                {
                    ++depth;
                }
                else if (text == ">>")
                {
                    depth += 2;
                }
                else if (text == "<" || text == "<=" || text == "<=>")
                {
                    if (--depth == 0)
                    {
                        const std::size_t keyword = PreviousSignificant(tokens, j, source);
                        if (keyword < tokens.size() && tokens[keyword].kind == TokenKind::Identifier &&
                            TokenText(source, tokens[keyword]) == "template")
                        {
                            return keyword;
                        }

                        return intro_token;
                    }
                }
                else
                {
                    return intro_token;
                } // `;`, `{`, `(`, ... : not a template head
            }

            return intro_token;
        }

        CompletionItem MakeTagItem(std::string_view source, const std::vector<Token> & tokens,
            const TagName &tag)
        {
            std::string detail(tag.intro);
            detail += ' ';
            detail += tag.text;
            CompletionItem item{std::string(tag.text), CompletionKind::Type, std::move(detail),
                DocCommentFor(source, tokens, TemplateHeadBefore(source, tokens, tag.intro_token))};
            item.has_location = true;
            item.offset = static_cast<std::uint32_t>(tag.offset);
            // `struct S {` / `struct S : B {` / `using S = ...` define; `struct S;`
            // and elaborated uses (`struct S x;`) do not.
            bool defines = tag.intro == "using" || tag.intro == "concept";
            for (std::size_t t = tag.name_token + 1; t < tokens.size() && !defines; ++t)
            {
                if (tokens[t].kind == TokenKind::Whitespace || tokens[t].kind == TokenKind::LineComment ||
                    tokens[t].kind == TokenKind::BlockComment)
                {
                    continue;
                }

                const std::string_view next = TokenText(source, tokens[t]);
                defines = next == "{" || next == ":" || next == "final";
                break;
            }

            item.is_definition = defines;
            return item;
        }

        CompletionItem MakeNamespaceItem(const ParseTree &tree, std::string_view source,
            const std::vector<Token> & tokens, std::size_t node,
            const std::vector<std::string> & full_path)
        {
            std::string joined;
            for (const auto & element: full_path)
            {
                if (element.empty())
                {
                    continue;
                }

                if (!joined.empty())
                {
                    joined += "::";
                }

                joined += element;
            }

            std::string detail = "namespace";
            if (!joined.empty())
            {
                detail += ' ';
                detail += joined;
            }

            // A record/enum scope reads as `enum class n::Mode`, not `namespace n::Mode`.
            if (tree.Nodes()[node].kind == GrammarKind::RecordDefinition && !joined.empty())
            {
                std::string intro;
                const auto &record = tree.Nodes()[node];
                const std::size_t record_end = std::min<std::size_t>(record.first_token + record.token_count,
                    tokens.size());
                for (std::size_t t = record.first_token; t < record_end; ++t)
                {
                    if (tokens[t].kind != TokenKind::Identifier || tree.IsDecorationToken(t))
                    {
                        continue;
                    }

                    const std::string_view word = TokenText(source, tokens[t]);
                    if (word != "enum" && word != "class" && word != "struct" && word != "union")
                    {
                        break;
                    }

                    intro += word;
                    intro += ' ';
                }

                if (!intro.empty())
                {
                    detail = intro + joined;
                }
            }

            const std::string label = joined.empty() ? std::string{}: full_path.back();
            CompletionItem item{label, CompletionKind::Namespace, std::move(detail),
                DocCommentFor(source, tokens, tree.Nodes()[node].first_token)};
            // The name token follows `namespace` (and `inline`): first identifier
            // that is not a keyword inside the node.
            const auto &grammar = tree.Nodes()[node];
            for (std::size_t t = grammar.first_token; t < grammar.first_token + grammar.token_count &&
                t < tokens.size(); ++t)
            {
                if (tokens[t].kind != TokenKind::Identifier || tree.IsDecorationToken(t))
                {
                    continue;
                }

                const std::string_view word = TokenText(source, tokens[t]);
                if (word == "namespace" || word == "inline" || word == "export" || word == "class" ||
                    word == "struct" || word == "union" || word == "enum")
                {
                    continue;
                }

                item.has_location = true;
                item.offset = tokens[t].offset;
                item.is_definition = true;
                break;
            }

            return item;
        }

        void CollectDefines(std::string_view source, const std::vector<Token> & tokens,
            std::unordered_map<std::string, CompletionItem> & best, std::string_view prefix)
        {
            std::vector<Define> defines;
            ScanDefines(source, tokens, defines);
            for (const auto & define: defines)
            {
                if (!prefix.empty() && !StartsWith(define.name, prefix))
                {
                    continue;
                }

                std::string detail = define.value.empty() ? "macro" : define.value;
                if (detail.size() > 128)
                {
                    detail.resize(128);
                }

                InsertItem(best, {std::string(define.name), CompletionKind::Macro, std::move(detail),
                        DocCommentFor(source, tokens, define.hash_token)});
            }
        }

        void CollectTagNamesIn(std::string_view source, const std::vector<Token> & tokens,
            std::unordered_map<std::string, CompletionItem> & best, std::string_view prefix,
            std::size_t range_start, std::size_t range_end)
        {
            std::vector<TagName> tags;
            ScanTagNames(source, tokens, tags);
            for (const auto & tag: tags)
            {
                if (IsKeyword(tag.text))
                {
                    continue;
                }

                if (tag.offset < range_start || tag.offset >= range_end)
                {
                    continue;
                }

                if (!prefix.empty() && !StartsWith(tag.text, prefix))
                {
                    continue;
                }

                InsertItem(best, MakeTagItem(source, tokens, tag));
            }
        }

        void CollectTagNames(std::string_view source, const std::vector<Token> & tokens,
            std::unordered_map<std::string, CompletionItem> & best, std::string_view prefix)
        {
            CollectTagNamesIn(source, tokens, best, prefix, 0, source.size());
        }

        // Nested scope names below a qualifier: for `ns::`, a scope with path
        // `ns::inner` contributes `inner`. Powers both `ns::` member listing and the
        // global `::` scope, including names introduced by compound definitions
        // (`namespace a::b`) that have no intermediate node to match exactly.
        void CollectChildScopes(const ParseTree &tree, std::string_view source,
            const std::vector<Token> & tokens,
            const std::vector<std::vector<std::string>> & scope_paths,
            const std::vector<std::string> & qualifier,
            std::unordered_map<std::string, CompletionItem> & best, std::string_view prefix)
        {
            for (std::size_t n = 0; n < scope_paths.size(); ++n)
            {
                if (scope_paths[n].empty())
                {
                    continue;
                }

                const std::vector<std::string> & path = scope_paths[n];
                if (path.size() <= qualifier.size())
                {
                    continue;
                }

                bool matches = true;
                for (std::size_t i = 0; i < qualifier.size(); ++i)
                {
                    if (path[i] != qualifier[i])
                    {
                        matches = false;
                        break;
                    }
                }

                if (!matches)
                {
                    continue;
                }

                const std::string & name = path[qualifier.size()];
                if (name.empty())
                {
                    continue;
                }

                // Reserved (`_`-leading) scopes are never meant to be named.
                if (name.front() == '_')
                {
                    continue;
                }

                if (!prefix.empty() && !StartsWith(name, prefix))
                {
                    continue;
                }

                std::vector<std::string> full_path(path.begin(), path.begin() + qualifier.size() + 1);
                InsertItem(best, MakeNamespaceItem(tree, source, tokens, n, full_path));
            }
        }

    } // namespace

    static ScopeIndex BuildScopeIndex(const ParseTree &tree, bool for_header);

    ScopeIndex CompletionEngine::IndexScopes(std::string_view source, const ParserOptions &options)
    {
        const ParseTree tree = ParseTree::Parse(source, options);
        return IndexScopes(tree);
    }

    ScopeIndex CompletionEngine::IndexScopes(const ParseTree &tree)
    {
        return BuildScopeIndex(tree, true);
    }

    // `for_header` drops `_`-leading variables (implementation detail of a
    // header); the buffer's own index keeps them so `obj.` offers `_field`.
    static ScopeIndex BuildScopeIndex(const ParseTree &tree, bool for_header)
    {
        const std::string_view source = tree.Source();
        const std::vector<Token> & tokens = tree.Tokens();
        ScopeIndex index;

        std::unordered_map<std::string, std::size_t> entry_pos;
        auto path_key =[](const std::vector<std::string> & path)
        {
            std::string key;
            for (const auto & element: path)
            {
                key += element;
                key += '\0';
            }

            return key;
        };
        auto entry_for =[&](const std::vector<std::string> & path, CompletionKind kind) -> IndexedScope &
        {
            const std::string key = path_key(path);
            if (const auto found = entry_pos.find(key); found != entry_pos.end())
            {
                return index[found->second];
            }

            const std::size_t pos = index.size();
            index.push_back({path, kind, {}});
            entry_pos.emplace(key, pos);
            return index.back();
        };
        entry_for({}, CompletionKind::Keyword);

        const std::vector<std::vector<std::string>> scope_paths = BuildScopePaths(tree);

        // Declared names bucketed by owning scope; function locals land on body
        // blocks (no entry) and are skipped: they are not qualifier-addressable.
        for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
        {
            if (tree.Nodes()[n].kind != GrammarKind::DeclaredName)
            {
                continue;
            }

            const std::size_t scope = MemberScope(tree, n);
            if (scope >= tree.Nodes().size())
            {
                continue;
            }

            const GrammarKind scope_kind = tree.Nodes()[scope].kind;
            if (scope_kind != GrammarKind::NamespaceDefinition && scope_kind != GrammarKind::RecordDefinition &&
                scope != ParseTree::RootNode)
            {
                continue;
            }

            const std::size_t token_index = tree.Nodes()[n].first_token;
            if (token_index >= tree.Tokens().size())
            {
                continue;
            }

            const std::string_view name = tree.Text(tree.Tokens()[token_index]);
            if (name.empty() || IsKeyword(name))
            {
                continue;
            }

            const CompletionKind kind = ClassifyDeclaredName(tree, n);
            if (for_header && kind == CompletionKind::Variable && name.front() == '_')
            {
                continue;
            }

            IndexedScope &entry = entry_for(scope < scope_paths.size() ? scope_paths[scope]
                : ScopePath(tree, scope),
                CompletionKind::Type);
            entry.members.push_back(DescribeDeclared(tree, source, tokens, n, name, kind));
        }

        // `using Name = Type;` carries no DeclaredName node: index the alias as a
        // Type member that remembers its target for member-access resolution.
        for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
        {
            if (tree.Nodes()[n].kind != GrammarKind::UsingDeclaration)
            {
                continue;
            }

            const auto &grammar = tree.Nodes()[n];
            const std::size_t last = std::min<std::size_t>(grammar.first_token + grammar.token_count,
                tokens.size());
            std::size_t name_token = tokens.size();
            std::size_t equals = tokens.size();
            for (std::size_t k = grammar.first_token; k < last; ++k)
            {
                if (tokens[k].kind == TokenKind::Whitespace || tokens[k].kind == TokenKind::LineComment ||
                    tokens[k].kind == TokenKind::BlockComment)
                {
                    continue;
                }

                const std::string_view word = tree.Text(tokens[k]);
                if (word == "using")
                {
                    continue;
                }

                if (tokens[k].kind == TokenKind::Identifier && name_token == tokens.size())
                {
                    name_token = k;
                    continue;
                }

                if (name_token != tokens.size() && word == "=")
                {
                    equals = k;
                }

                break;
            }

            if (name_token == tokens.size() || equals == tokens.size())
            {
                continue;
            }

            const std::size_t scope = MemberScope(tree, n);
            if (scope >= tree.Nodes().size())
            {
                continue;
            }

            const GrammarKind scope_kind = tree.Nodes()[scope].kind;
            if (scope_kind != GrammarKind::NamespaceDefinition && scope_kind != GrammarKind::RecordDefinition &&
                scope != ParseTree::RootNode)
            {
                continue;
            }

            const std::string_view name = tree.Text(tokens[name_token]);
            if (IsKeyword(name) ||(for_header && name.front() == '_'))
            {
                continue;
            }

            std::string target = CompactWs(SliceRange(tree, static_cast<std::uint32_t>(equals + 1),
                last -(equals + 1)), 160);
            while (!target.empty() && (target.back() == ';' || target.back() == ' '))
            {
                target.pop_back();
            }

            CompletionItem item{std::string(name), CompletionKind::Type, KindDetail(CompletionKind::Type),
                DocCommentFor(source, tokens, grammar.first_token)};
            item.has_location = true;
            item.offset = tokens[name_token].offset;
            item.is_definition = true;
            item.type_text = std::move(target);
            IndexedScope &entry = entry_for(scope < scope_paths.size() ? scope_paths[scope]
                : ScopePath(tree, scope),
                CompletionKind::Type);
            entry.members.push_back(std::move(item));
        }

        // Base classes, attached to the record's own entry (created on demand:
        // a record that only inherits has no members of its own).
        for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
        {
            if (tree.Nodes()[n].kind != GrammarKind::RecordDefinition || n >= scope_paths.size() ||
                scope_paths[n].empty() || scope_paths[n].back().empty())
            {
                continue;
            }

            auto bases = RecordBases(tree, n);
            if (bases.empty())
            {
                continue;
            }

            IndexedScope &entry = entry_for(scope_paths[n], CompletionKind::Type);
            for (auto & base: bases)
            {
                if (std::find(entry.bases.begin(), entry.bases.end(), base) == entry.bases.end())
                {
                    entry.bases.push_back(std::move(base));
                }
            }
        }

        // Nested scope names owned by each entry's scope.
        for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
        {
            const GrammarKind kind = tree.Nodes()[n].kind;
            if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
            {
                continue;
            }

            const std::size_t owner = MemberScope(tree, n);
            if (owner >= tree.Nodes().size())
            {
                continue;
            }

            const GrammarKind owner_kind = tree.Nodes()[owner].kind;
            if (owner_kind != GrammarKind::NamespaceDefinition && owner_kind != GrammarKind::RecordDefinition &&
                owner != ParseTree::RootNode)
            {
                continue;
            }

            const auto elements = ScopeNameElements(tree, n);
            if (elements.empty() || elements.back().empty() || elements.back().front() == '_')
            {
                continue;
            }

            const std::vector<std::string> owner_path =
                owner < scope_paths.size() && !scope_paths[owner].empty() ? scope_paths[owner] : ScopePath(tree,
                owner);
            const std::vector<std::string> full_path =
                n < scope_paths.size() && !scope_paths[n].empty() ? scope_paths[n] : ScopePath(tree, n);
            IndexedScope &entry = entry_for(owner_path, CompletionKind::Type);
            entry.members.push_back(MakeNamespaceItem(tree, source, tokens, n, full_path));
        }

        // Tag names bucketed by innermost enclosing named scope.
        std::vector<TagName> tags;
        ScanTagNames(source, tokens, tags, &tree);
        for (const auto & tag: tags)
        {
            if (IsKeyword(tag.text) || tag.text.front() == '_')
            {
                continue;
            }

            std::size_t bucket = ParseTree::RootNode;
            std::size_t span = static_cast<std::size_t>(-1);
            for (std::size_t n = 1; n < tree.Nodes().size(); ++n)
            {
                const GrammarKind kind = tree.Nodes()[n].kind;
                if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
                {
                    continue;
                }

                const auto[start, end] = NodeRange(tree, n);
                if (start <= tag.offset && tag.offset < end && end - start < span)
                {
                    bucket = n;
                    span = end - start;
                }
            }

            IndexedScope &entry = entry_for(bucket < scope_paths.size() && !scope_paths[bucket].empty()
                ? scope_paths[bucket]
                : ScopePath(tree, bucket),
                CompletionKind::Type);
            entry.members.push_back(MakeTagItem(source, tokens, tag));
        }

        // Macros are always global.
        {
            std::vector<Define> defines;
            ScanDefines(source, tokens, defines);
            IndexedScope &entry = entry_for({}, CompletionKind::Keyword);
            for (const auto & define: defines)
            {
                if (define.name.front() == '_')
                {
                    continue;
                }

                std::string detail = define.value.empty() ? "macro" : define.value;
                if (detail.size() > 128)
                {
                    detail.resize(128);
                }

                entry.members.push_back({std::string(define.name), CompletionKind::Macro, std::move(detail),
                        DocCommentFor(source, tokens, define.hash_token)});
            }
        }
        for (auto & entry: index)
        {
            std::sort(entry.members.begin(), entry.members.end(),[](const CompletionItem &left,
                const CompletionItem &right)
                {
                    if (left.label != right.label)
                    {
                        return left.label < right.label;
                }

                    return static_cast<int>(left.kind) < static_cast<int>(right.kind);
            });
            // Merge same (label, kind) duplicates, keeping documentation.
            std::vector<CompletionItem> merged;
            for (auto & member: entry.members)
            {
                if (!merged.empty() && merged.back().label == member.label &&
                    merged.back().kind == member.kind)
                {
                    if (merged.back().documentation.empty() && !member.documentation.empty())
                    {
                        merged.back().documentation = std::move(member.documentation);
                    }

                    if (merged.back().type_text.empty() && !member.type_text.empty())
                    {
                        merged.back().type_text = std::move(member.type_text);
                    }

                    if (merged.back().detail == KindDetail(merged.back().kind) &&
                        member.detail != KindDetail(member.kind))
                    {
                        merged.back().detail = std::move(member.detail);
                    }

                    continue;
                }

                merged.push_back(std::move(member));
            }

            entry.members = std::move(merged);
        }

        return index;
    }

    namespace
    {

        void CompleteExpression(const ParseTree &tree, std::string_view source,
            const std::vector<Token> & tokens, const ParserOptions &options,
            const std::vector<CallableInterval> & callables,
            const std::vector<std::vector<std::string>> & scope_paths, std::size_t offset,
            std::string_view prefix, const ScopeIndex *external,
            std::unordered_map<std::string, CompletionItem> & best)
        {
            for (const auto keyword: kKeywords)
            {
                if (!prefix.empty() && !StartsWith(keyword, prefix))
                {
                    continue;
                }

                // `compl`/`co_await` appear twice in the table; dedupe keeps one.
                const CompletionKind kind =
                    IsBuiltinType(keyword) ? CompletionKind::Type : CompletionKind::Keyword;
                InsertItem(best, {std::string(keyword), kind, KindDetail(kind), {}});
            }

            // Lexical fallback with occurrence visibility: an identifier is usable
            // when it occurs at global scope or earlier in the cursor's own function.
            // This hides locals of other functions while keeping words the grammar
            // has not modeled, and still works mid-recovery on incomplete code.
            // Callable intervals are binary-searched per token (was: one O(N) node
            // scan per token), and repeat identifiers are deduped by view before any
            // std::string allocation (was: one CompletionItem per occurrence).
            const std::size_t cursor_callable = FindCallable(callables, offset);
            std::unordered_set<std::string_view> seen_lexical;
            seen_lexical.reserve(tokens.size());
            for (const auto & token: tokens)
            {
                if (token.kind != TokenKind::Identifier)
                {
                    continue;
                }

                const std::string_view text = TokenText(source, token);
                if (text == prefix)
                {
                    continue;
                } // don't echo the word being typed

                if (!prefix.empty() && !StartsWith(text, prefix))
                {
                    continue;
                }

                if (IsKeyword(text))
                {
                    continue;
                } // keyword entry already added

                if (!seen_lexical.insert(text).second)
                {
                    continue;
                }

                const std::size_t owner = FindCallable(callables, token.offset);
                if (owner != NoIndex && (owner != cursor_callable || token.offset >= offset))
                {
                    continue;
                }

                InsertItem(best, {std::string(text), CompletionKind::Variable, "variable", {}});
            }

            // Declared names upgrade the kind (function/type vs plain variable) and
            // carry signatures plus documentation. Tag names (`struct Widget`) are
            // not always DeclaredName nodes, so collect them lexically as well.
            CollectTagNames(source, tokens, best, prefix);
            for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
            {
                if (tree.Nodes()[n].kind != GrammarKind::DeclaredName)
                {
                    continue;
                }

                const std::size_t token_index = tree.Nodes()[n].first_token;
                if (token_index >= tree.Tokens().size())
                {
                    continue;
                }

                const std::string_view name = tree.Text(tree.Tokens()[token_index]);
                if (name.empty())
                {
                    continue;
                }

                if (!prefix.empty() && !StartsWith(name, prefix))
                {
                    continue;
                }

                const LocalInfo local = AnalyzeLocal(tree, n);
                if (local.is_local)
                {
                    // Point of declaration: usable only inside the owning block and
                    // once the declaration itself is complete.
                    if (local.owner == NoIndex)
                    {
                        continue;
                    }

                    const auto[block_start, block_end] = NodeRange(tree, local.boundary);
                    if (offset < block_start || offset > block_end)
                    {
                        continue;
                    }

                    const Token &name_token = tree.Tokens()[token_index];
                    if (name_token.offset + name_token.length > offset)
                    {
                        continue;
                    }

                    InsertItem(best, DescribeDeclared(tree, source, tokens, n, name,
                        CompletionKind::Variable));
                    continue;
                }

                const CompletionKind kind = ClassifyDeclaredName(tree, n);
                if (IsKeyword(name) && kind == CompletionKind::Variable)
                {
                    continue;
                }

                InsertItem(best, DescribeDeclared(tree, source, tokens, n, name, kind));
            }

            // Top-level namespaces are visible unqualified (as qualifier heads) with
            // their documentation attached. Nested ones stay qualified-only. The
            // Namespace kind outranks the lexical fallback's plain Variable.
            for (std::size_t n = 0; n < scope_paths.size(); ++n)
            {
                if (tree.Nodes()[n].kind != GrammarKind::NamespaceDefinition)
                {
                    continue;
                }

                if (scope_paths[n].size() != 1 || scope_paths[n].front().empty())
                {
                    continue;
                }

                const std::vector<std::string> & path = scope_paths[n];
                if (IsKeyword(path.front()) || path.front().front() == '_')
                {
                    continue;
                }

                if (!prefix.empty() && !StartsWith(path.front(), prefix))
                {
                    continue;
                }

                InsertItem(best, MakeNamespaceItem(tree, source, tokens, n, path));
            }

            // Macros from `#define` and predefined compile-command defines.
            CollectDefines(source, tokens, best, prefix);
            for (const auto &[name, value]: options.Macros())
            {
                if (!prefix.empty() && !StartsWith(name, prefix))
                {
                    continue;
                }

                std::string detail = value.empty() ? "macro" : value;
                if (detail.size() > 128)
                {
                    detail.resize(128);
                }

                InsertItem(best, {name, CompletionKind::Macro, std::move(detail), {}});
            }

            // Header globals (top-level functions, macros, using-aliases) are visible
            // unqualified once included. Namespaced header members stay qualified-only.
            if (external != nullptr)
            {
                for (const auto & scope: *external)
                {
                    if (!scope.path.empty())
                    {
                        continue;
                    }

                    for (const auto & member: scope.members)
                    {
                        if (!prefix.empty() && !StartsWith(member.label, prefix))
                        {
                            continue;
                        }

                        InsertItem(best, member);
                    }
                }
            }
        }

        bool MatchesPrefix(std::string_view name, std::string_view prefix)
        {
            return!name.empty() && (prefix.empty() || StartsWith(name, prefix));
        }

        void CompleteQualified(const ParseTree &tree, std::string_view source,
            const std::vector<Token> & tokens,
            const std::vector<std::vector<std::string>> & scope_paths,
            const std::vector<CallableInterval> & callables,
            const std::vector<ScopeInterval> & named_scopes, std::size_t offset,
            std::string_view prefix, const ScopeIndex *external,
            std::unordered_map<std::string, CompletionItem> & best)
        {
            const std::size_t scope_op = AccessOperatorBefore(source, tokens, offset, prefix);
            if (scope_op >= tokens.size() || TokenText(source, tokens[scope_op]) != "::")
            {
                return;
            }

            const Qualifier qualifier = QualifierBefore(source, tokens, scope_op);
            if (qualifier.unresolved)
            {
                return;
            } // `A<int>::x`, `call()::y`: no guesses

            if (qualifier.global)
            {
                // Leading `::` names the global scope: no keywords, builtins, macros
                // or function locals, and no record members.
                for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
                {
                    if (tree.Nodes()[n].kind != GrammarKind::DeclaredName)
                    {
                        continue;
                    }

                    if (MemberScope(tree, n) != ParseTree::RootNode)
                    {
                        continue;
                    }

                    const std::size_t token_index = tree.Nodes()[n].first_token;
                    if (token_index >= tree.Tokens().size())
                    {
                        continue;
                    }

                    const std::string_view name = tree.Text(tree.Tokens()[token_index]);
                    if (!MatchesPrefix(name, prefix))
                    {
                        continue;
                    }

                    const CompletionKind kind = ClassifyDeclaredName(tree, n);
                    if (IsKeyword(name) && kind == CompletionKind::Variable)
                    {
                        continue;
                    }

                    InsertItem(best, DescribeDeclared(tree, source, tokens, n, name, kind));
                }

                std::unordered_set<std::string_view> seen_global;
                seen_global.reserve(tokens.size());
                for (const auto & token: tokens)
                {
                    if (token.kind != TokenKind::Identifier)
                    {
                        continue;
                    }

                    const std::string_view text = TokenText(source, token);
                    if (!MatchesPrefix(text, prefix) || IsKeyword(text))
                    {
                        continue;
                    }

                    if (!seen_global.insert(text).second)
                    {
                        continue;
                    }

                    if (FindCallable(callables, token.offset) != NoIndex)
                    {
                        continue;
                    }

                    if (InNamedScopeIntervals(named_scopes, token.offset))
                    {
                        continue;
                    }

                    InsertItem(best, {std::string(text), CompletionKind::Variable, "variable", {}});
                }

                CollectChildScopes(tree, source, tokens, scope_paths, {}, best, prefix);
                if (external != nullptr)
                {
                    for (const auto & scope: *external)
                    {
                        if (scope.path.empty())
                        {
                            for (const auto & member: scope.members)
                            {
                                if (!MatchesPrefix(member.label, prefix))
                                {
                                    continue;
                                }

                                InsertItem(best, member);
                            }
                        }
                        else if (scope.path.size() == 1)
                        {
                            if (!MatchesPrefix(scope.path.front(), prefix))
                            {
                                continue;
                            }

                            InsertItem(best, {scope.path.front(), scope.kind, KindDetail(scope.kind), {}});
                        }
                    }
                }

                return;
            }

            const std::vector<std::size_t> targets = ResolveScope(scope_paths, qualifier.path);
            // External (header) scopes matching the qualifier union with local ones.
            // An empty target set with no external match means an unknown qualifier.
            bool saw_external = false;
            if (external != nullptr)
            {
                for (const auto & scope: *external)
                {
                    if (scope.path != qualifier.path)
                    {
                        continue;
                    }

                    saw_external = true;
                    for (const auto & member: scope.members)
                    {
                        if (!MatchesPrefix(member.label, prefix))
                        {
                            continue;
                        }

                        InsertItem(best, member);
                    }
                }

                // External nested scopes below the qualifier (`std::` offers `chrono`
                // from a `std::chrono` header scope).
                for (const auto & scope: *external)
                {
                    if (scope.path.size() <= qualifier.path.size())
                    {
                        continue;
                    }

                    bool matches = true;
                    for (std::size_t i = 0; i < qualifier.path.size(); ++i)
                    {
                        if (scope.path[i] != qualifier.path[i])
                        {
                            matches = false;
                            break;
                        }
                    }

                    if (!matches)
                    {
                        continue;
                    }

                    saw_external = true;
                    const std::string & name = scope.path[qualifier.path.size()];
                    if (!MatchesPrefix(name, prefix))
                    {
                        continue;
                    }

                    InsertItem(best, {name, scope.kind, KindDetail(scope.kind), {}});
                }
            }

            if (targets.empty() && !saw_external)
            {
                return;
            } // unknown qualifier (`std::`): no guesses

            auto is_target =[&](std::size_t node)
            {
                for (const auto target: targets)
                {
                    if (node == target)
                    {
                        return true;
                    }
                }

                return false;
            };
            for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
            {
                if (tree.Nodes()[n].kind != GrammarKind::DeclaredName)
                {
                    continue;
                }

                if (!is_target(MemberScope(tree, n)))
                {
                    continue;
                }

                const std::size_t token_index = tree.Nodes()[n].first_token;
                if (token_index >= tree.Tokens().size())
                {
                    continue;
                }

                const std::string_view name = tree.Text(tree.Tokens()[token_index]);
                if (!MatchesPrefix(name, prefix))
                {
                    continue;
                }

                const CompletionKind kind = ClassifyDeclaredName(tree, n);
                if (IsKeyword(name) && kind == CompletionKind::Variable)
                {
                    continue;
                }

                InsertItem(best, DescribeDeclared(tree, source, tokens, n, name, kind));
            }

            // Nested scopes (`struct Inner` / `namespace inner`) owned by a target.
            for (std::size_t n = 0; n < tree.Nodes().size(); ++n)
            {
                const GrammarKind kind = tree.Nodes()[n].kind;
                if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
                {
                    continue;
                }

                if (!is_target(MemberScope(tree, n)))
                {
                    continue;
                }

                const auto elements = ScopeNameElements(tree, n);
                if (elements.empty() ||!MatchesPrefix(elements.back(), prefix))
                {
                    continue;
                }

                std::vector<std::string> full_path = ScopePath(tree, n);
                if (full_path.empty())
                {
                    continue;
                }

                InsertItem(best, MakeNamespaceItem(tree, source, tokens, n, full_path));
            }

            for (const auto target: targets)
            {
                const auto[start, end] = NodeRange(tree, target);
                CollectTagNamesIn(source, tokens, best, prefix, start, end);
            }

            // Members introduced by deeper compound definitions (`ns::a::b` makes `a`
            // visible under `ns::` even without an intermediate node).
            CollectChildScopes(tree, source, tokens, scope_paths, qualifier.path, best, prefix);
        }

        // --- Member access (`obj.` / `ptr->`) -----------------------------------
        // The receiver is parsed backwards from the operator into a chain of
        // segments (`a.b().c[0]->`), each resolved to a record path through the
        // variable/field/function declarations the tree and header index know.
        // Anything the engine cannot type (dependent types, lambdas, operators)
        // yields no items rather than a guess.

        struct TypeName
        {
            std::vector<std::string> path;
            std::vector<std::string> args; // top-level template arguments, as written
            bool ok = false;
        };

        bool IsTypeWord(std::string_view word)
        {
            static constexpr std::string_view words[] = {"const", "volatile", "static", "constexpr",
                "consteval", "constinit", "inline", "extern", "mutable", "register", "thread_local",
                "typename", "struct", "class", "union", "enum", "virtual", "friend", "explicit",
                "typedef", "signed", "unsigned"};
            return std::find(std::begin(words), std::end(words), word) != std::end(words);
        }

        bool IsTrivia(TokenKind kind)
        {
            return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
                kind == TokenKind::BlockComment;
        }

        // Splits the template argument list that opens at `open` (a `<` token) into
        // top-level arguments. Returns the index of the closing token (or tokens.size()).
        std::size_t SplitTemplateArgs(std::string_view text, const std::vector<Token> & toks,
            std::size_t open, std::vector<std::string> & args)
        {
            int angle = 0;
            int nest = 0;
            std::size_t arg_start = toks[open].offset + toks[open].length;
            for (std::size_t k = open; k < toks.size(); ++k)
            {
                if (toks[k].kind != TokenKind::Punctuation)
                {
                    continue;
                }

                const std::string_view p = text.substr(toks[k].offset, toks[k].length);
                if (p == "(" || p == "[" || p == "{")
                {
                    ++nest;
                }
                else if (p == ")" || p == "]" || p == "}")
                {
                    --nest;
                }
                else if (nest == 0 && p == "<")
                {
                    ++angle;
                }
                else if (nest == 0 && (p == ">" || p == ">>"))
                {
                    angle -= p == ">" ? 1 : 2;
                    if (angle <= 0)
                    {
                        const std::size_t end = toks[k].offset;
                        if (end > arg_start)
                        {
                            args.push_back(CompactWs(std::string(text.substr(arg_start, end - arg_start)), 160));
                        }

                        return k;
                    }
                }
                else if (nest == 0 && angle == 1 && p == ",")
                {
                    args.push_back(CompactWs(std::string(text.substr(arg_start, toks[k].offset - arg_start)),
                        160));
                    arg_start = toks[k].offset + toks[k].length;
                }
            }

            return toks.size();
        }

        // `const std::vector<int> &` -> {std, vector}, args {int}.
        TypeName ParseTypeName(std::string_view text)
        {
            TypeName result;
            const std::vector<Token> toks = Lexer(text).Lex();
            auto sig =[&](std::size_t k)
            {
                while (k < toks.size() && IsTrivia(toks[k].kind))
                {
                    ++k;
                }

                return k;
            };
            auto txt =[&](std::size_t k)
            {
                return text.substr(toks[k].offset, toks[k].length);
            };
            std::size_t i = sig(0);
            while (i < toks.size() && toks[i].kind == TokenKind::Identifier && IsTypeWord(txt(i)))
            {
                i = sig(i + 1);
            }

            if (i < toks.size() && toks[i].kind == TokenKind::Punctuation && txt(i) == "::")
            {
                i = sig(i + 1);
            }

            while (i < toks.size() && toks[i].kind == TokenKind::Identifier)
            {
                result.path.emplace_back(txt(i));
                result.args.clear();
                i = sig(i + 1);
                if (i < toks.size() && toks[i].kind == TokenKind::Punctuation && txt(i) == "<")
                {
                    const std::size_t close = SplitTemplateArgs(text, toks, i, result.args);
                    if (close >= toks.size())
                    {
                        return {};
                    }

                    i = sig(close + 1);
                }

                if (i < toks.size() && toks[i].kind == TokenKind::Punctuation && txt(i) == "::")
                {
                    i = sig(i + 1);
                    continue;
                }

                break;
            }

            result.ok =!result.path.empty();
            return result;
        }

        std::string PathKey(const std::vector<std::string> & path)
        {
            std::string key;
            for (const auto & element: path)
            {
                key += element;
                key += '\0';
            }

            return key;
        }

        std::vector<std::string> Join(const std::vector<std::string> & prefix,
            const std::vector<std::string> & rest)
        {
            std::vector<std::string> out = prefix;
            out.insert(out.end(), rest.begin(), rest.end());
            return out;
        }

        // Wrappers whose `->` reaches the first template argument.
        bool IsPointerLike(std::string_view name)
        {
            static constexpr std::string_view names[] = {"unique_ptr", "shared_ptr", "optional",
                "auto_ptr", "scoped_ptr", "intrusive_ptr", "weak_ptr_lock", "reference_wrapper"};
            return std::find(std::begin(names), std::end(names), name) != std::end(names);
        }

        // Containers whose `[]` yields the first / second template argument.
        bool SubscriptYieldsSecond(std::string_view name)
        {
            return name == "map" || name == "unordered_map" || name == "flat_map";
        }

        struct ChainSegment
        {
            std::vector<std::string> qualifier;
            std::string name;
            std::vector<std::string> targs;
            std::string post; // '(' call/construct, '[' subscript, in source order
        };

        struct Chain
        {
            std::vector<ChainSegment> segments;
            std::vector<std::string> ops; // ops[i] follows segments[i]
            bool ok = false;
            bool new_expression = false;
            std::size_t head_token = 0;
        };

        struct Resolved
        {
            std::vector<std::string> path;
            std::vector<std::string> args;
            bool ok = false;
        };

        class MemberResolver
        {
        public:
            MemberResolver(const ParseTree &tree, const ScopeIndex *external, std::size_t offset)
            : m_tree(tree), m_source(tree.Source()), m_tokens(tree.Tokens()), m_offset(offset)
            {
                m_local = BuildScopeIndex(tree, false);
                for (const auto & scope: m_local)
                {
                    m_by_path[PathKey(scope.path)].push_back(&scope);
                }

                if (external != nullptr)
                {
                    for (const auto & scope: *external)
                    {
                        m_by_path[PathKey(scope.path)].push_back(&scope);
                    }
                }

                ComputeCursorScopes();
                for (const auto & directive: Navigation::UsingDirectives(tree))
                {
                    if (directive.active_begin <= offset && offset <= directive.active_end)
                    {
                        m_using.push_back(directive.target);
                    }
                }
            }

            // Parses the receiver of the access operator at token `op`.
            Chain ParseReceiver(std::size_t op) const
            {
                Chain chain;
                std::string op_text(TokenText(m_source, m_tokens[op]));
                std::vector<ChainSegment> reversed;
                std::vector<std::string> ops_reversed{op_text};
                std::size_t pos = PreviousSignificant(m_tokens, op, m_source);
                while (true)
                {
                    ChainSegment segment;
                    std::string post_reversed;
                    while (pos < m_tokens.size() && m_tokens[pos].kind == TokenKind::Punctuation)
                    {
                        const std::string_view p = TokenText(m_source, m_tokens[pos]);
                        if (p != ")" && p != "]" && p != "}")
                        {
                            break;
                        }

                        const std::size_t open = MatchBackward(pos);
                        if (open >= m_tokens.size())
                        {
                            return chain;
                        }

                        post_reversed += p == "]" ? '[' : '(';
                        pos = PreviousSignificant(m_tokens, open, m_source);
                        // `f<T>(...)`: keep the template arguments.
                        if (post_reversed.back() == '(' && pos < m_tokens.size() &&
                            m_tokens[pos].kind == TokenKind::Punctuation &&
                            (TokenText(m_source, m_tokens[pos]) == ">" ||
                            TokenText(m_source, m_tokens[pos]) == ">>"))
                        {
                            const std::size_t lt = MatchAngleBackward(pos);
                            if (lt >= m_tokens.size())
                            {
                                return chain;
                            }

                            SplitTemplateArgsInSource(lt, segment.targs);
                            pos = PreviousSignificant(m_tokens, lt, m_source);
                        }
                    }

                    if (pos >= m_tokens.size() || m_tokens[pos].kind != TokenKind::Identifier)
                    {
                        return chain;
                    }

                    segment.name = std::string(TokenText(m_source, m_tokens[pos]));
                    segment.post.assign(post_reversed.rbegin(), post_reversed.rend());
                    chain.head_token = pos;
                    std::size_t before = PreviousSignificant(m_tokens, pos, m_source);
                    while (before < m_tokens.size() && IsPunct(before, "::"))
                    {
                        const std::size_t name = PreviousSignificant(m_tokens, before, m_source);
                        if (name >= m_tokens.size() || m_tokens[name].kind != TokenKind::Identifier)
                        {
                            break; // leading `::`
                        }

                        segment.qualifier.insert(segment.qualifier.begin(),
                            std::string(TokenText(m_source, m_tokens[name])));
                        chain.head_token = name;
                        before = PreviousSignificant(m_tokens, name, m_source);
                    }

                    reversed.push_back(std::move(segment));
                    if (before < m_tokens.size() && (IsPunct(before, ".") || IsPunct(before, "->")))
                    {
                        ops_reversed.push_back(std::string(TokenText(m_source, m_tokens[before])));
                        pos = PreviousSignificant(m_tokens, before, m_source);
                        continue;
                    }

                    if (before < m_tokens.size() && m_tokens[before].kind == TokenKind::Identifier &&
                        TokenText(m_source, m_tokens[before]) == "new")
                    {
                        chain.new_expression = true;
                        chain.head_token = before;
                    }

                    break;
                }

                chain.segments.assign(reversed.rbegin(), reversed.rend());
                chain.ops.assign(ops_reversed.rbegin(), ops_reversed.rend());
                chain.ok = true;
                return chain;
            }

            Resolved ResolveChain(const Chain &chain, int depth = 0) const
            {
                Resolved current;
                if (!chain.ok || chain.segments.empty() || depth > 6)
                {
                    return current;
                }

                for (std::size_t i = 0; i < chain.segments.size(); ++i)
                {
                    const ChainSegment &segment = chain.segments[i];
                    Resolved next = i == 0 ? ResolveHead(segment, chain.new_expression, depth)
                    : ResolveMember(current, segment, depth);
                    if (!next.ok)
                    {
                        return {};
                    }

                    current = std::move(next);
                    current = Dereference(current, chain.ops[i]);
                    if (!current.ok)
                    {
                        return {};
                    }
                }

                return current;
            }

            void CollectMembers(const std::vector<std::string> & path, std::string_view prefix,
                std::unordered_map<std::string, CompletionItem> & best) const
            {
                std::unordered_set<std::string> visited;
                CollectInto(path, prefix, best, visited, 0);
            }

            // First token of `a.b.c` style chains, for `auto` deduction.
            const std::vector<std::vector<std::string>> & EnclosingRecords() const
            {
                return m_records;
            }

        private:
            bool IsPunct(std::size_t token, std::string_view text) const
            {
                return m_tokens[token].kind == TokenKind::Punctuation &&
                    TokenText(m_source, m_tokens[token]) == text;
            }

            std::size_t MatchBackward(std::size_t close) const
            {
                int depth = 0;
                for (std::size_t k = close + 1; k > 0; --k)
                {
                    const std::size_t i = k - 1;
                    if (m_tokens[i].kind != TokenKind::Punctuation)
                    {
                        continue;
                    }

                    const std::string_view p = TokenText(m_source, m_tokens[i]);
                    if (p == ")" || p == "]" || p == "}")
                    {
                        ++depth;
                    }
                    else if (p == "(" || p == "[" || p == "{")
                    {
                        if (--depth == 0)
                        {
                            return i;
                        }
                    }
                }

                return m_tokens.size();
            }

            std::size_t MatchAngleBackward(std::size_t close) const
            {
                int depth = 0;
                for (std::size_t k = close + 1; k > 0; --k)
                {
                    const std::size_t i = k - 1;
                    if (m_tokens[i].kind != TokenKind::Punctuation)
                    {
                        continue;
                    }

                    const std::string_view p = TokenText(m_source, m_tokens[i]);
                    if (p == ">")
                    {
                        ++depth;
                    }
                    else if (p == ">>")
                    {
                        depth += 2;
                    }
                    else if (p == "<")
                    {
                        if (--depth <= 0)
                        {
                            return i;
                        }
                    }
                    else if (p == ";" || p == "{" || p == "}")
                    {
                        break;
                    }
                }

                return m_tokens.size();
            }

            void SplitTemplateArgsInSource(std::size_t open, std::vector<std::string> & args) const
            {
                SplitTemplateArgs(m_source, m_tokens, open, args);
            }

            void ComputeCursorScopes()
            {
                std::size_t best_namespace = NoIndex;
                std::size_t best_span = static_cast<std::size_t>(-1);
                std::vector<std::pair<std::size_t, std::size_t>> records; // (span, node)
                std::size_t best_function = NoIndex;
                std::size_t function_span = static_cast<std::size_t>(-1);
                for (std::size_t n = 1; n < m_tree.Nodes().size(); ++n)
                {
                    const GrammarKind kind = m_tree.Nodes()[n].kind;
                    if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition &&
                        kind != GrammarKind::FunctionDefinition)
                    {
                        continue;
                    }

                    const auto[start, end] = NodeRange(m_tree, n);
                    if (!(start <= m_offset && m_offset <= end) || end <= start)
                    {
                        continue;
                    }

                    const std::size_t span = end - start;
                    if (kind == GrammarKind::FunctionDefinition)
                    {
                        if (span < function_span)
                        {
                            function_span = span;
                            best_function = n;
                        }
                    }
                    else if (kind == GrammarKind::NamespaceDefinition)
                    {
                        if (span < best_span)
                        {
                            best_span = span;
                            best_namespace = n;
                        }
                    }
                    else
                    {
                        records.emplace_back(span, n);
                    }
                }

                std::sort(records.begin(), records.end());
                // Out-of-line member: `void A::f() { ... }` makes A the implicit class.
                if (best_function != NoIndex)
                {
                    const std::size_t declarator = FindChild(m_tree, best_function, GrammarKind::Declarator);
                    const std::size_t nested = declarator == NoIndex
                    ? NoIndex
                    : FindChild(m_tree, declarator, GrammarKind::NestedNameSpecifier);
                    if (nested != NoIndex)
                    {
                        std::vector<std::string> owner;
                        const auto &grammar = m_tree.Nodes()[nested];
                        for (std::uint32_t k = 0; k < grammar.token_count; ++k)
                        {
                            const Token &token = m_tree.Tokens()[grammar.first_token + k];
                            if (token.kind == TokenKind::Identifier)
                            {
                                owner.emplace_back(m_tree.Text(token));
                            }
                        }

                        if (!owner.empty())
                        {
                            std::vector<std::string> prefix;
                            if (best_namespace != NoIndex)
                            {
                                prefix = ScopePath(m_tree, best_namespace);
                            }

                            m_records.push_back(Join(prefix, owner));
                        }
                    }
                }

                for (const auto &[span, node]: records)
                {
                    (void) span;
                    m_records.push_back(ScopePath(m_tree, node));
                }

                if (!records.empty())
                {
                    m_hint = ScopePath(m_tree, records.front().second);
                }
                else if (!m_records.empty())
                {
                    m_hint = m_records.front();
                }
                else if (best_namespace != NoIndex)
                {
                    m_hint = ScopePath(m_tree, best_namespace);
                }
            }

            const std::vector<const IndexedScope * > & ScopesAt(const std::vector<std::string> & path) const
            {
                static const std::vector<const IndexedScope * > none;
                const auto found = m_by_path.find(PathKey(path));
                return found == m_by_path.end() ? none : found->second;
            }

            // Prefixes tried when resolving a name written inside `hint`: the hint
            // and each of its ancestors, then the using-directive namespaces.
            std::vector<std::vector<std::string>> Prefixes(const std::vector<std::string> & hint) const
            {
                std::vector<std::vector<std::string>> out;
                for (std::size_t k = hint.size() + 1; k > 0; --k)
                {
                    out.emplace_back(hint.begin(), hint.begin() + static_cast<std::ptrdiff_t>(k - 1));
                }

                for (const auto & target: m_using)
                {
                    out.push_back(target);
                }

                return out;
            }

            struct Found
            {
                const CompletionItem *item = nullptr;
                std::vector<std::string> owner; // scope that declares it
            };

            Found FindDeep(const std::vector<std::string> & path, std::string_view label,
                CompletionKind kind, std::unordered_set<std::string> & visited, int depth) const
            {
                if (depth > 16 ||!visited.insert(PathKey(path)).second)
                {
                    return {};
                }

                for (const IndexedScope * scope: ScopesAt(path))
                {
                    for (const auto & member: scope->members)
                    {
                        if (member.label == label && member.kind == kind)
                        {
                            return {&member, path};
                        }
                    }
                }

                for (const IndexedScope * scope: ScopesAt(path))
                {
                    for (const auto & base: scope->bases)
                    {
                        const Resolved resolved = ResolveType(ParseTypeName(base), Parent(path), 0);
                        if (!resolved.ok)
                        {
                            continue;
                        }

                        const Found found = FindDeep(resolved.path, label, kind, visited, depth + 1);
                        if (found.item != nullptr)
                        {
                            return found;
                        }
                    }
                }

                return {};
            }

            static std::vector<std::string> Parent(const std::vector<std::string> & path)
            {
                return path.empty() ? path : std::vector<std::string>(path.begin(), path.end() - 1);
            }

            void CollectInto(const std::vector<std::string> & path, std::string_view prefix,
                std::unordered_map<std::string, CompletionItem> & best,
                std::unordered_set<std::string> & visited, int depth) const
            {
                if (depth > 16 ||!visited.insert(PathKey(path)).second)
                {
                    return;
                }

                for (const IndexedScope * scope: ScopesAt(path))
                {
                    for (const auto & member: scope->members)
                    {
                        if (member.kind != CompletionKind::Function && member.kind != CompletionKind::Variable)
                        {
                            continue;
                        }

                        if (!path.empty() && member.label == path.back())
                        {
                            continue; // constructor
                        }

                        if (!MatchesPrefix(member.label, prefix))
                        {
                            continue;
                        }

                        InsertItem(best, member);
                    }
                }

                for (const IndexedScope * scope: ScopesAt(path))
                {
                    for (const auto & base: scope->bases)
                    {
                        const Resolved resolved = ResolveType(ParseTypeName(base), Parent(path), 0);
                        if (resolved.ok)
                        {
                            CollectInto(resolved.path, prefix, best, visited, depth + 1);
                        }
                    }
                }
            }

            // Type name -> canonical record path, following alias chains.
            Resolved ResolveType(const TypeName &type, const std::vector<std::string> & hint, int depth) const
            {
                Resolved out;
                if (!type.ok || depth > 8)
                {
                    return out;
                }

                for (const auto & prefix: Prefixes(hint))
                {
                    const std::vector<std::string> full = Join(prefix, type.path);
                    // A scope with members or bases is a record/namespace we can list.
                    if (!ScopesAt(full).empty())
                    {
                        out.path = full;
                        out.args = type.args;
                        out.ok = true;
                        return out;
                    }

                    Found alias = FindAlias(Parent(full), full.back());
                    if (alias.item != nullptr)
                    {
                        const TypeName target = ParseTypeName(alias.item->type_text);
                        Resolved via = ResolveType(target, alias.owner, depth + 1);
                        if (via.ok)
                        {
                            if (via.args.empty())
                            {
                                via.args = type.args;
                            }

                            return via;
                        }
                    }
                }

                return out;
            }

            Found FindAlias(const std::vector<std::string> & scope, std::string_view name) const
            {
                for (const IndexedScope * entry: ScopesAt(scope))
                {
                    for (const auto & member: entry->members)
                    {
                        if (member.label == name && member.kind == CompletionKind::Type &&
                            !member.type_text.empty())
                        {
                            return {&member, scope};
                        }
                    }
                }

                return {};
            }

            Resolved FromText(std::string_view text, const std::vector<std::string> & hint, int depth) const
            {
                return ResolveType(ParseTypeName(text), hint, depth);
            }

            Resolved Dereference(Resolved value, const std::string & op) const
            {
                if (op != "->" || value.path.empty() ||!IsPointerLike(value.path.back()) || value.args.empty())
                {
                    return value;
                }

                return FromText(value.args.front(), m_hint, 0);
            }

            // Declared type of the variable `name` as visible at the cursor.
            Resolved ResolveVariable(const std::string & name, int depth) const
            {
                // 1. Locals and parameters: the closest preceding visible declaration.
                std::size_t best = NoIndex;
                std::uint32_t best_offset = 0;
                for (std::size_t n = 0; n < m_tree.Nodes().size(); ++n)
                {
                    if (m_tree.Nodes()[n].kind != GrammarKind::DeclaredName)
                    {
                        continue;
                    }

                    const std::size_t token_index = m_tree.Nodes()[n].first_token;
                    if (token_index >= m_tokens.size() || m_tree.Text(m_tokens[token_index]) != name)
                    {
                        continue;
                    }

                    const LocalInfo local = AnalyzeLocal(m_tree, n);
                    if (!local.is_local || local.owner == NoIndex)
                    {
                        continue;
                    }

                    const auto[block_start, block_end] = NodeRange(m_tree, local.boundary);
                    if (m_offset < block_start || m_offset > block_end)
                    {
                        continue;
                    }

                    const Token &name_token = m_tokens[token_index];
                    if (name_token.offset + name_token.length > m_offset)
                    {
                        continue;
                    }

                    if (best == NoIndex || name_token.offset >= best_offset)
                    {
                        best = n;
                        best_offset = name_token.offset;
                    }
                }

                if (best != NoIndex)
                {
                    return ResolveLocal(best, depth);
                }

                // 2. Fields of the enclosing classes (including inherited ones).
                for (const auto & record: m_records)
                {
                    std::unordered_set<std::string> visited;
                    const Found found = FindDeep(record, name, CompletionKind::Variable, visited, 0);
                    if (found.item != nullptr)
                    {
                        return FromText(found.item->type_text, found.owner, 0);
                    }
                }

                // 3. Namespace-level variables (own, enclosing, global, `using namespace`).
                for (const auto & prefix: Prefixes(m_hint))
                {
                    std::unordered_set<std::string> visited;
                    const Found found = FindDeep(prefix, name, CompletionKind::Variable, visited, 0);
                    if (found.item != nullptr)
                    {
                        return FromText(found.item->type_text, found.owner, 0);
                    }
                }

                return {};
            }

            Resolved ResolveLocal(std::size_t node, int depth) const
            {
                const std::string text = VariableTypeDetail(m_tree, node);
                const TypeName type = ParseTypeName(text);
                if (type.ok && (type.path.size() != 1 || type.path.front() != "auto"))
                {
                    return ResolveType(type, m_hint, 0);
                }

                // `auto x = <chain>;` / `auto x{<chain>}`: type the initializer.
                std::size_t t = m_tree.Nodes()[node].first_token + 1;
                while (t < m_tokens.size() && (IsTrivia(m_tokens[t].kind)))
                {
                    ++t;
                }

                if (t >= m_tokens.size() || m_tokens[t].kind != TokenKind::Punctuation ||
                    (TokenText(m_source, m_tokens[t]) != "=" && TokenText(m_source, m_tokens[t]) != "{"))
                {
                    return {};
                }

                // Find the end of the initializer, then parse the chain that ends there.
                std::size_t end = t + 1;
                int nest = 0;
                for (; end < m_tokens.size(); ++end)
                {
                    if (m_tokens[end].kind != TokenKind::Punctuation)
                    {
                        continue;
                    }

                    const std::string_view p = TokenText(m_source, m_tokens[end]);
                    if (p == "(" || p == "[" || p == "{")
                    {
                        ++nest;
                    }
                    else if (p == ")" || p == "]" || p == "}")
                    {
                        if (--nest < 0)
                        {
                            break;
                        }
                    }
                    else if ((p == ";" || p == ",") && nest <= 0)
                    {
                        break;
                    }
                }

                if (end >= m_tokens.size() || end <= t + 1)
                {
                    return {};
                }

                Chain chain = ParseChainEndingAt(end, t);
                if (!chain.ok)
                {
                    return {};
                }

                return ResolveChain(chain, depth + 1);
            }

            // Parses the chain whose last token precedes `end`; it must start right
            // after `floor` (the `=`), or after `new`.
            Chain ParseChainEndingAt(std::size_t end, std::size_t floor) const
            {
                // Reuse the receiver parser: it reads backwards from a pseudo
                // operator token, so point it at the token after the initializer.
                Chain chain = ParseReceiverBefore(end);
                if (!chain.ok)
                {
                    return chain;
                }

                const std::size_t before = PreviousSignificant(m_tokens, chain.head_token, m_source);
                if (before != floor && !chain.new_expression)
                {
                    chain.ok = false;
                }

                return chain;
            }

            Chain ParseReceiverBefore(std::size_t end) const
            {
                Chain chain = ParseReceiver(end);
                // ParseReceiver stores the operator text as the trailing op; an
                // initializer has none, so it is harmless there.
                return chain;
            }

            Resolved ResolveHead(const ChainSegment &segment, bool new_expression, int depth) const
            {
                if (segment.name == "this" && segment.qualifier.empty() && segment.post.empty())
                {
                    if (m_records.empty())
                    {
                        return {};
                    }

                    Resolved self;
                    self.path = m_records.front();
                    self.ok = true;
                    return self;
                }

                const bool call =!segment.post.empty() && segment.post.front() == '(';
                const std::string_view rest = call ? std::string_view(segment.post).substr(1)
                : std::string_view(segment.post);
                Resolved value;
                if (segment.qualifier.empty() && !call)
                {
                    value = ResolveVariable(segment.name, depth);
                }
                else if (segment.qualifier.empty() || call)
                {
                    value = ResolveCallOrConstruct(segment, new_expression);
                }

                if (!value.ok && !segment.qualifier.empty() && !call)
                {
                    value = ResolveQualifiedVariable(segment);
                }

                return ApplyPostfix(value, rest);
            }

            Resolved ResolveQualifiedVariable(const ChainSegment &segment) const
            {
                for (const auto & prefix: Prefixes(m_hint))
                {
                    const auto scope = Join(prefix, segment.qualifier);
                    std::unordered_set<std::string> visited;
                    const Found found = FindDeep(scope, segment.name, CompletionKind::Variable, visited, 0);
                    if (found.item != nullptr)
                    {
                        return FromText(found.item->type_text, found.owner, 0);
                    }
                }

                return {};
            }

            Resolved ResolveCallOrConstruct(const ChainSegment &segment, bool new_expression) const
            {
                (void) new_expression;
                // `std::make_unique<T>(...)` and friends.
                if (segment.name == "make_unique" || segment.name == "make_shared")
                {
                    if (segment.targs.empty())
                    {
                        return {};
                    }

                    Resolved pointer;
                    pointer.path = {"std", segment.name == "make_unique" ? "unique_ptr" : "shared_ptr"};
                    pointer.args = {segment.targs.front()};
                    pointer.ok = true;
                    return pointer;
                }

                // Construction: `Type(...)`, `ns::Type{...}`.
                TypeName type;
                type.path = Join(segment.qualifier, {segment.name});
                type.args = segment.targs;
                type.ok = true;
                Resolved constructed = ResolveType(type, m_hint, 0);
                if (constructed.ok)
                {
                    return constructed;
                }

                // Free function or member of an enclosing class.
                std::vector<std::vector<std::string>> scopes;
                for (const auto & record: m_records)
                {
                    scopes.push_back(record);
                }

                for (const auto & prefix: Prefixes(m_hint))
                {
                    scopes.push_back(Join(prefix, segment.qualifier));
                }

                for (const auto & scope: scopes)
                {
                    std::unordered_set<std::string> visited;
                    const Found found = FindDeep(scope, segment.name, CompletionKind::Function, visited, 0);
                    if (found.item != nullptr && !found.item->type_text.empty())
                    {
                        return FromText(found.item->type_text, found.owner, 0);
                    }
                }

                return {};
            }

            Resolved ResolveMember(const Resolved &owner, const ChainSegment &segment, int depth) const
            {
                (void) depth;
                const bool call =!segment.post.empty() && segment.post.front() == '(';
                const std::string_view rest = call ? std::string_view(segment.post).substr(1)
                : std::string_view(segment.post);
                std::unordered_set<std::string> visited;
                const Found found = FindDeep(owner.path, segment.name,
                    call ? CompletionKind::Function : CompletionKind::Variable, visited, 0);
                if (found.item == nullptr || found.item->type_text.empty())
                {
                    return {};
                }

                return ApplyPostfix(FromText(found.item->type_text, found.owner, 0), rest);
            }

            // `[]` on a typed value: element of arrays/pointers (same type) and of
            // the standard containers (first or second template argument).
            Resolved ApplyPostfix(Resolved value, std::string_view post) const
            {
                for (const char marker: post)
                {
                    if (!value.ok || marker != '[')
                    {
                        return {};
                    }

                    if (value.args.empty())
                    {
                        continue; // raw array / pointer
                    }

                    const std::size_t which =!value.path.empty() && SubscriptYieldsSecond(value.path.back()) ? 1 : 0;
                    if (which >= value.args.size())
                    {
                        return {};
                    }

                    value = FromText(value.args[which], m_hint, 0);
                }

                return value;
            }

            const ParseTree &m_tree;
            std::string_view m_source;
            const std::vector<Token> & m_tokens;
            std::size_t m_offset;
            ScopeIndex m_local;
            std::unordered_map<std::string, std::vector<const IndexedScope * >> m_by_path;
            std::vector<std::vector<std::string>> m_using;
            std::vector<std::vector<std::string>> m_records; // enclosing classes, innermost first
            std::vector<std::string> m_hint;                 // namespace/class path at the cursor
        };

        void CompleteMember(const ParseTree &tree, std::size_t offset, std::string_view prefix,
            const ScopeIndex *external, std::unordered_map<std::string, CompletionItem> & best)
        {
            const std::string_view source = tree.Source();
            const std::vector<Token> & tokens = tree.Tokens();
            const std::size_t op = AccessOperatorBefore(source, tokens, offset, prefix);
            if (op >= tokens.size())
            {
                return;
            }

            const std::string_view op_text = TokenText(source, tokens[op]);
            if (op_text != "." && op_text != "->")
            {
                return; // `.*` takes a pointer-to-member, not a member name
            }

            const MemberResolver resolver(tree, external, offset);
            const Chain chain = resolver.ParseReceiver(op);
            const Resolved type = resolver.ResolveChain(chain);
            if (!type.ok)
            {
                return;
            }

            resolver.CollectMembers(type.path, prefix, best);
        }

    } // namespace

    std::string CompletionEngine::PrefixAt(std::string_view source, std::size_t offset)
    {
        if (offset > source.size())
        {
            offset = source.size();
        }

        std::size_t start = offset;
        while (start > 0 && IsIdentChar(source[start - 1]))
        {
            --start;
        }

        return std::string(source.substr(start, offset - start));
    }

    std::vector<CompletionItem> CompletionEngine::Complete(std::string_view source, std::size_t offset)
    {
        ParserOptions options;
        return Complete(source, options, offset);
    }

    std::vector<CompletionItem> CompletionEngine::Complete(std::string_view source,
        const ParserOptions &options, std::size_t offset)
    {
        return Complete(source, options, offset, nullptr);
    }

    std::vector<CompletionItem> CompletionEngine::Complete(std::string_view source,
        const ParserOptions &options, std::size_t offset,
        const ScopeIndex *external)
    {
        if (offset > source.size())
        {
            offset = source.size();
        }

        const std::string prefix = PrefixAt(source, offset);
        const std::vector<Token> tokens = Lexer(source).Lex();
        const CursorContext context = ClassifyContext(source, tokens, offset, prefix);

        if (context == CursorContext::Suppressed) return {};

        std::unordered_map<std::string, CompletionItem> best;

        if (context == CursorContext::Preprocessor)
        {
            for (const auto directive: kDirectives)
            {
                if (prefix.empty() || StartsWith(directive, prefix))
                {
                    InsertItem(best, {std::string(directive), CompletionKind::Directive, "directive", {}});
                }
            }

            CollectDefines(source, tokens, best, prefix);
            for (const auto &[name, value]: options.Macros())
            {
                if (!prefix.empty() && !StartsWith(name, prefix))
                {
                    continue;
                }

                std::string detail = value.empty() ? "macro" : value;
                if (detail.size() > 128)
                {
                    detail.resize(128);
                }

                InsertItem(best, {name, CompletionKind::Macro, std::move(detail), {}});
            }
        }
        else
        {
            // The grammar tree carries the scope structure for both unqualified
            // visibility and qualified (`ns::`) member resolution.
            const ParseTree tree = ParseTree::Parse(source, options);
            return Complete(tree, options, offset, external);
        }

        std::vector<CompletionItem> items;
        items.reserve(best.size());
        for (auto &[label, item]: best)
        {
            (void) label;
            items.push_back(std::move(item));
        }

        std::sort(items.begin(), items.end(),[](const CompletionItem &left, const CompletionItem &right)
            {
                if (left.label != right.label)
                {
                    return left.label < right.label;
            }

                return static_cast<int>(left.kind) < static_cast<int>(right.kind);
        });
        return items;
    }

    std::vector<CompletionItem> CompletionEngine::Complete(const ParseTree &tree,
        const ParserOptions &options,
        std::size_t offset, const ScopeIndex *external)
    {
        const std::string_view source = tree.Source();
        const std::vector<Token> & tokens = tree.Tokens();
        if (offset > source.size())
        {
            offset = source.size();
        }

        const std::string prefix = PrefixAt(source, offset);
        const CursorContext context = ClassifyContext(source, tokens, offset, prefix);

        if (context == CursorContext::Suppressed) return {};

        std::unordered_map<std::string, CompletionItem> best;

        if (context == CursorContext::Preprocessor)
        {
            for (const auto directive: kDirectives)
            {
                if (prefix.empty() || StartsWith(directive, prefix))
                {
                    InsertItem(best, {std::string(directive), CompletionKind::Directive, "directive", {}});
                }
            }

            CollectDefines(source, tokens, best, prefix);
            for (const auto &[name, value]: options.Macros())
            {
                if (!prefix.empty() && !StartsWith(name, prefix))
                {
                    continue;
                }

                std::string detail = value.empty() ? "macro" : value;
                if (detail.size() > 128)
                {
                    detail.resize(128);
                }

                InsertItem(best, {name, CompletionKind::Macro, std::move(detail), {}});
            }
        }
        else
        {
            const std::vector<CallableInterval> callables = BuildCallableIntervals(tree);
            const std::vector<std::vector<std::string>> scope_paths = BuildScopePaths(tree);
            if (context == CursorContext::MemberAccess)
            {
                CompleteMember(tree, offset, prefix, external, best);
            }
            else if (context == CursorContext::ScopeAccess)
            {
                const std::vector<ScopeInterval> named_scopes = BuildNamedScopeIntervals(tree);
                CompleteQualified(tree, source, tokens, scope_paths, callables, named_scopes,
                    offset, prefix, external, best);
            }
            else
            {
                CompleteExpression(tree, source, tokens, options, callables, scope_paths, offset,
                    prefix, external, best);
            }
        }

        std::vector<CompletionItem> items;
        items.reserve(best.size());
        for (auto &[label, item]: best)
        {
            (void) label;
            items.push_back(std::move(item));
        }

        std::sort(items.begin(), items.end(),[](const CompletionItem &left, const CompletionItem &right)
            {
                if (left.label != right.label)
                {
                    return left.label < right.label;
            }

                return static_cast<int>(left.kind) < static_cast<int>(right.kind);
        });
        return items;
    }

    std::optional<CompletionItem> CompletionEngine::Hover(std::string_view source,
        const ParserOptions &options, std::size_t offset,
        const ScopeIndex *external)
    {
        if (offset > source.size())
        {
            offset = source.size();
        }

        if (source.empty())
        {
            return std::nullopt;
        }

        std::size_t start = offset;
        while (start > 0 && IsIdentChar(source[start - 1]))
        {
            --start;
        }

        std::size_t end = offset;
        while (end < source.size() && IsIdentChar(source[end]))
        {
            ++end;
        }

        if (start == end)
        {
            return std::nullopt;
        }

        const std::string word(source.substr(start, end - start));
        if (!IsIdentStart(word.front()) || IsKeyword(word))
        {
            return std::nullopt;
        }

        const auto items = Complete(source, options, end, external);
        for (const auto & item: items)
        {
            if (item.label == word)
            {
                return item;
            }
        }

        return std::nullopt;
    }

    std::optional<CompletionItem> CompletionEngine::Hover(const ParseTree &tree,
        const ParserOptions &options,
        std::size_t offset, const ScopeIndex *external)
    {
        const std::string_view source = tree.Source();
        if (offset > source.size())
        {
            offset = source.size();
        }

        if (source.empty())
        {
            return std::nullopt;
        }

        std::size_t start = offset;
        while (start > 0 && IsIdentChar(source[start - 1]))
        {
            --start;
        }

        std::size_t end = offset;
        while (end < source.size() && IsIdentChar(source[end]))
        {
            ++end;
        }

        if (start == end)
        {
            return std::nullopt;
        }

        const std::string word(source.substr(start, end - start));
        if (!IsIdentStart(word.front()) || IsKeyword(word))
        {
            return std::nullopt;
        }

        const auto items = Complete(tree, options, end, external);
        for (const auto & item: items)
        {
            if (item.label == word)
            {
                return item;
            }
        }

        return std::nullopt;
    }

} // namespace heimdall
