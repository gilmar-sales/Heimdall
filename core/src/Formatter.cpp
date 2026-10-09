#include <Heimdall/Formatter.hpp>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/Preprocessor.hpp>

#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

namespace heimdall
{

    namespace
    {

        bool IsIndent(char c)
        {
            return c == ' ' || c == '\t';
        }

        bool IsTrivia(TokenKind kind)
        {
            return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
                   kind == TokenKind::BlockComment;
        }

        bool IsWordish(TokenKind kind)
        {
            return kind == TokenKind::Identifier || kind == TokenKind::Number ||
                   kind == TokenKind::StringLiteral || kind == TokenKind::CharacterLiteral ||
                   kind == TokenKind::RawStringLiteral;
        }

        bool IsComment(TokenKind kind)
        {
            return kind == TokenKind::LineComment || kind == TokenKind::BlockComment;
        }

        bool IsBrace(std::string_view source, const Token& token, char brace)
        {
            return token.kind == TokenKind::Punctuation && token.length == 1 &&
                   source[token.offset] == brace;
        }

        bool IsDirectiveLine(std::size_t                               line_start,
                             const std::vector<PreprocessorDirective>& directives,
                             std::size_t&                              directive_cursor)
        {
            while (directive_cursor < directives.size() &&
                   directives[directive_cursor].offset + directives[directive_cursor].length <=
                       line_start)
            {
                ++directive_cursor;
            }

            // The line is a directive line only when it starts inside the directive
            // span (was: `directive.offset < line_start + directive.length`, which
            // also matched plain code lines sitting just before the next directive
            // and copied whole regions verbatim, freezing brace depth).
            return directive_cursor < directives.size() &&
                   directives[directive_cursor].offset <= line_start &&
                   line_start <
                       directives[directive_cursor].offset + directives[directive_cursor].length;
        }

        std::string_view TokenText(std::string_view source, const Token& token)
        {
            return source.substr(token.offset, token.length);
        }

        // Significant (non-trivia) token enriched with the context the spacing and
        // chunking passes need. Directive-line tokens are excluded upstream: they are
        // copied verbatim and never count as structure.
        struct Sig
        {
            std::size_t      token = 0; // index into the lexer token vector
            std::string_view text;
            TokenKind        kind          = TokenKind::Unknown;
            std::size_t      line          = 0;
            std::size_t      paren_depth   = 0;
            std::size_t      bracket_depth = 0;
            bool             in_condition  = false; // innermost paren follows if/for/while/switch
            std::size_t match = static_cast<std::size_t>(-1); // matching (/)/[/] index into sigs
        };

        bool IsTypeKeyword(std::string_view text)
        {
            // Enough to recognize `T *p` / `T &r` declarators (user types are covered
            // by the qualifier/position rules in IsDeclaratorStar).
            static constexpr std::string_view kTypes[] = {
                "void",  "bool",     "char",     "char8_t", "char16_t", "char32_t", "wchar_t",
                "short", "int",      "long",     "signed",  "unsigned", "float",    "double",
                "auto",  "decltype", "typename", "const",   "volatile", "unsigned",
            };
            for (const auto type : kTypes)
            {
                if (text == type)
                {
                    return true;
                }
            }

            return false;
        }

        bool IsQualifierKeyword(std::string_view text)
        {
            static constexpr std::string_view kQualifiers[] = {
                "const", "volatile", "static", "extern", "mutable", "unsigned", "long",
                "short", "signed",   "struct", "class",  "union",   "enum",
            };
            for (const auto qualifier : kQualifiers)
            {
                if (text == qualifier)
                {
                    return true;
                }
            }

            return false;
        }

        bool IsControlKeyword(std::string_view text)
        {
            // Keywords whose `(...)` is a condition, not a declarator: `&`/`*`/`&&`
            // inside stay binary operators.
            return text == "if" || text == "for" || text == "while" || text == "switch";
        }

        // Defined after the spacing rules (they operate on whole files); declared
        // here because SpacingGap consults them per token pair.
        bool IsTrailingReturnArrow(const std::vector<Sig>& sigs, std::size_t k);

        bool IsBlockOpenBrace(const std::vector<Sig>& sigs, std::size_t brace);

        bool IsConstevalIfHead(const std::vector<Sig>& sigs, std::size_t consteval_index);
        constexpr std::size_t kNoSig = static_cast<std::size_t>(-1);

        // File-local named constants for the `cpp/no-magic-numbers` rule
        // (single-literal initializers, which the rule exempts).
        constexpr std::size_t kTwoTokenOffset   = 2; // neighbor-token lookbehind/lookahead
        constexpr std::size_t kThreeTokenOffset = 3; // three-token lookahead
        constexpr std::size_t kMidpointDivisor =
            2; // binary-search halve (`(lo + hi) / kMidpointDivisor`)
        constexpr std::size_t kMinLabelTokens      = 2; // minimum head tokens for a scope label
        constexpr std::size_t kMaxLabelHeadTokens  = 4; // head tokens inspected for a scope label
        constexpr std::size_t kCrlfLength          = 2; // bytes in "\r\n"
        constexpr std::size_t kCommentMarkerLength = 2; // bytes in the `//` marker
        constexpr std::size_t kMinCommentRunLength =
            2; // trailing-comment runs align from two lines up
        constexpr std::size_t kIncludeKeywordLength = 7;  // bytes in "include"
        constexpr int         kInheritanceScanLimit = 32; // backward scan for a record/enum keyword
        constexpr int         kSingleLineMaxPasses  = 32; // single-line-style fixpoint iterations
        constexpr int         kEntryCommaScanLimit  = 8;  // backward scan for an `enum` keyword
        constexpr int         kColumnSplitMaxAttempts = 8; // column-limit splits per line
        constexpr int kTypeDefinitionScanLimit = 64;  // backward scan for a type-definition opener
        constexpr int kBaseListScanLimit       = 64;  // backward scan for a base-list colon
        constexpr int kBlockBraceScanLimit     = 256; // backward scan for a block-introducing token
        constexpr int kMaxTemplateLookback     = 64;  // backward scan for a template-id `<`

        bool IsSpaceBeforeParenKeyword(std::string_view text)
        {
            // `if (` but `sizeof(` / `decltype(` / `noexcept(`.
            if (text == "sizeof" || text == "alignof" || text == "noexcept" || text == "decltype")
            {
                return false;
            }

            return text == "if" || text == "for" || text == "while" || text == "switch" ||
                   text == "catch" || text == "return" || text == "new" || text == "delete" ||
                   text == "constexpr" || text == "consteval"; // `if constexpr (`
        }

        bool IsBinaryOperator(std::string_view text)
        {
            // `<`, `>`, `<=`, `>=`, `<<`, `>>` are deliberately absent: without name
            // resolution they are indistinguishable from template brackets, so their
            // spacing is preserved as typed (see SpacingGap).
            static constexpr std::string_view kBinary[] = {
                "=", "==", "!=", "+",  "-",  "*",  "/",  "%",  "^",  "|",  "||",
                "&", "&&", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "?",
            };
            for (const auto op : kBinary)
            {
                if (text == op)
                {
                    return true;
                }
            }

            return false;
        }

        bool IsOpenForUnary(const Sig& prev)
        {
            // Positions after which `*`/`&`/`-`/etc. are unary, not binary.
            if (prev.kind != TokenKind::Punctuation && prev.kind != TokenKind::Identifier)
            {
                return false;
            }

            const std::string_view text = prev.text;
            if (text == "(" || text == "," || text == ";" || text == "=" || text == "{" ||
                text == "[" || text == "return" || text == ":" || text == "?" || text == "!")
            {
                return true;
            }

            return IsBinaryOperator(text);
        }

        // True when the `>`/`>>` at `gt` closes a template-id
        // (`vector<Token> &tokens`): a `<` is reachable by scanning back over
        // template arguments only. Bails on anything that cannot appear inside
        // `<...>`, so `a > &b`, `f(a > &b)` and `x = (a > &b)` stay binary.
        bool ClosesTemplateId(const std::vector<Sig>& sigs, std::size_t gt)
        {
            for (int steps = 0; steps < kMaxTemplateLookback && gt > 0; ++steps)
            {
                --gt;
                if (sigs[gt].kind == TokenKind::Punctuation)
                {
                    const std::string_view text = sigs[gt].text;
                    if (text == "<")
                    {
                        return true;
                    }

                    if (text == ";" || text == "{" || text == "}" || text == "(" || text == ")" ||
                        text == "[" || text == "]")
                    {
                        return false;
                    }

                    continue;
                }

                if (sigs[gt].kind != TokenKind::Identifier && sigs[gt].kind != TokenKind::Number)
                {
                    return false;
                }
            }

            return false;
        }

        bool IsRangeForColon(const std::vector<Sig>& sigs, std::size_t colon)
        {
            if (sigs[colon].text != ":" || sigs[colon].paren_depth == 0)
            {
                return false;
            }

            const auto  depth  = sigs[colon].paren_depth;
            std::size_t nested = 0;
            for (auto i = colon; i > 0;)
            {
                --i;
                const auto& sig = sigs[i];
                if (sig.text == ")" || sig.text == "]" || sig.text == "}")
                {
                    ++nested;
                    continue;
                }

                if (nested > 0)
                {
                    if (sig.text == "(" || sig.text == "[" || sig.text == "{")
                    {
                        --nested;
                    }

                    continue;
                }

                if (sig.text == "(" && sig.paren_depth + 1 == depth)
                {
                    return i > 0 && sigs[i - 1].text == "for";
                }

                if (sig.paren_depth == depth && (sig.text == "?" || sig.text == ":"))
                {
                    return false;
                }

                if (sig.paren_depth < depth)
                {
                    return false;
                }
            }

            return false;
        }

        // True when sigs[k] (`*`, `&` or `&&`) declares rather than computes:
        // `T *p`, `T &r = ...`, `f(T&&)`, `T *f()`. Genuinely ambiguous cases
        // (`f(a & b)`, `x * y;`) follow the declaration reading, matching the
        // grammar; `=`-preceded and condition-paren positions stay binary
        // (`x = a & b`, `if (a && b)`). A call paren after the name (`*f()`)
        // is a function declarator only when the type side is unmistakably a
        // type; plain `a * b()` stays a multiplication.
        bool IsDeclaratorStar(const std::vector<Sig>& sigs, std::size_t k)
        {
            const std::string_view text = sigs[k].text;
            if (sigs[k].kind != TokenKind::Punctuation ||
                (text != "*" && text != "&" && text != "&&"))
            {
                return false;
            }

            if (k == 0 || sigs[k - 1].line != sigs[k].line)
            {
                return false;
            } // deref at line start

            if (IsOpenForUnary(sigs[k - 1]))
            {
                return false;
            }

            // Abstract declarator of a type-id (`sizeof(void*)`, `f(char*, int)`,
            // `static_cast<T*>(p)`): no binary operator can be followed by `)`, `,` or `>`.
            if (!sigs[k].in_condition && k + 1 < sigs.size() && sigs[k + 1].line == sigs[k].line &&
                sigs[k + 1].kind == TokenKind::Punctuation &&
                (sigs[k + 1].text == ")" || sigs[k + 1].text == "," || sigs[k + 1].text == ">"))
            {
                const std::string_view before = sigs[k - 1].text;
                if (sigs[k - 1].kind == TokenKind::Identifier || before == "*" || before == "&" ||
                    ((before == ">" || before == ">>") && ClosesTemplateId(sigs, k - 1)))
                {
                    return true;
                }
            }

            // Stacked declarators (`char **p`, `T *&r`, `*const *p`): look past the
            // rest of the run to the declared name.
            std::size_t name = k + 1;
            while (
                name < sigs.size() && sigs[name].line == sigs[k].line &&
                ((sigs[name].kind == TokenKind::Punctuation &&
                  (sigs[name].text == "*" || sigs[name].text == "&" || sigs[name].text == "&&")) ||
                 (sigs[name].kind == TokenKind::Identifier &&
                  (sigs[name].text == "const" || sigs[name].text == "volatile") &&
                  name + 1 < sigs.size() && sigs[name + 1].kind == TokenKind::Punctuation &&
                  sigs[name + 1].text == "*")))
            {
                ++name;
            }

            if (name >= sigs.size() || sigs[name].line != sigs[k].line)
            {
                return false;
            }

            // Structured bindings are names too: `auto& [key, value] : items`.
            if (sigs[name].text == "[")
            {
                std::size_t depth = 1;
                while (++name < sigs.size())
                {
                    if (sigs[name].text == "[")
                    {
                        ++depth;
                    }
                    else if (sigs[name].text == "]" && --depth == 0)
                    {
                        break;
                    }
                }

                if (name >= sigs.size())
                {
                    return false;
                }
            }
            else if (sigs[name].kind != TokenKind::Identifier)
            {
                return false;
            }

            // Out-of-class definitions have a qualified declarator name:
            // `const Model& Context::Semantic()`. Inspect the token after
            // the complete name, not the first scope-resolution operator.
            while (name + 2 < sigs.size() && sigs[name + 1].line == sigs[k].line &&
                   sigs[name + 2].line == sigs[k].line && sigs[name + 1].text == "::" &&
                   sigs[name + 2].kind == TokenKind::Identifier)
            {
                name += 2;
            }

            if (name + 1 >= sigs.size() || sigs[name + 1].line != sigs[k].line)
            {
                return false;
            }

            const std::string_view after = sigs[name + 1].text;
            const bool range_declarator  = after == ":" && IsRangeForColon(sigs, name + 1);
            if (sigs[k].in_condition && !range_declarator)
            {
                return false;
            }

            const bool function_declarator = after == "(";
            if (after != ";" && after != "=" && after != "," && after != ")" &&
                !function_declarator && !range_declarator)
            {
                return false;
            }

            const std::string_view prev = sigs[k - 1].text;
            if (IsTypeKeyword(prev) || prev == "*" || prev == "&")
            {
                return true;
            }

            if (sigs[k - 1].kind != TokenKind::Identifier)
            {
                // `const std::vector<Token> &tokens`: the type ends in a
                // template-id close rather than an identifier.
                if ((prev == ">" || prev == ">>") && ClosesTemplateId(sigs, k - 1))
                {
                    return true;
                }

                return false;
            }

            if (k < kTwoTokenOffset || sigs[k - kTwoTokenOffset].line != sigs[k].line)
            {
                return true;
            } // `Widget *p;`

            const std::string_view before = sigs[k - kTwoTokenOffset].text;
            if (function_declarator)
            {
                // `int *f()` never reaches here (early `true` above via
                // IsTypeKeyword/`*`/`&`); whatever remains must prove it is a
                // type, otherwise `a * b()` and `f(a * b())` stay multiplications.
                // A template-id `>` is deliberately not accepted: `a > *f()` is
                // a comparison with a dereference, not a declarator.
                return before == "::" || IsQualifierKeyword(before);
            }

            return before == "(" || before == "," || before == ";" || before == "{" ||
                   before == "}" || before == "<" || before == ">" || before == ":" ||
                   before == "::" || IsQualifierKeyword(before);
        }

        // True when sigs[k] (`*`, `&`) is a unary dereference/address-of: `*p`,
        // `&x`, `return *p`, `(*p)`.
        bool IsUnaryStar(const std::vector<Sig>& sigs, std::size_t k)
        {
            const std::string_view text = sigs[k].text;
            if (sigs[k].kind != TokenKind::Punctuation || (text != "*" && text != "&"))
            {
                return false;
            }

            if (k == 0 || sigs[k - 1].line != sigs[k].line)
            {
                return true;
            }

            return IsOpenForUnary(sigs[k - 1]);
        }

        bool IsUnaryMinus(const std::vector<Sig>& sigs, std::size_t k)
        {
            // `-`/`+` is unary unless an operand precedes it: `a - b` vs `-b`.
            // Note: the lexer reports keywords as Identifier, so keyword positions
            // (`return -x`, `case -1:`) need an explicit list.
            if (k == 0)
            {
                return true;
            }

            const Sig& prev = sigs[k - 1];
            if (prev.kind == TokenKind::Identifier)
            {
                static constexpr std::string_view kKeywords[] = {
                    "return",   "case",      "sizeof",   "new", "delete", "throw",
                    "co_await", "co_return", "co_yield", "not", "and",    "or",
                };
                for (const auto keyword : kKeywords)
                {
                    if (prev.text == keyword)
                    {
                        return true;
                    }
                }

                return false;
            }

            if (prev.kind == TokenKind::Number || prev.kind == TokenKind::StringLiteral ||
                prev.kind == TokenKind::CharacterLiteral ||
                prev.kind == TokenKind::RawStringLiteral)
            {
                return false;
            }

            if (prev.kind == TokenKind::Punctuation &&
                (prev.text == ")" || prev.text == "]" || prev.text == "}" || prev.text == "++" ||
                 prev.text == "--"))
            {
                return false;
            }

            return true;
        }

        bool IsContinuationEnd(std::string_view text)
        {
            // A line ending in one of these continues on the next line.
            if (text == "," || text == "(" || text == "[" || text == "?" || text == "return")
            {
                return true;
            }

            return IsBinaryOperator(text);
        }

        // True when the `:` at sigs[colon] is the else-branch separator of a
        // ternary `c ? a : b`: scanning back at the same bracket level, a `?` not
        // already paired with an inner `:` (nested ternaries) precedes it.
        bool IsTernaryColon(const std::vector<Sig>& sigs, std::size_t colon)
        {
            int depth   = 0;
            int pending = 0;
            for (auto k = colon; k > 0;)
            {
                --k;
                const Sig& t = sigs[k];
                if (t.kind != TokenKind::Punctuation)
                {
                    continue;
                }

                const std::string_view text = t.text;
                if (text == ")" || text == "]" || text == "}")
                {
                    if (text == "}" && depth == 0)
                    {
                        return false;
                    } // statement boundary

                    ++depth;
                }
                else if (text == "(" || text == "[" || text == "{")
                {
                    if (depth == 0)
                    {
                        return false;
                    } // left the enclosing bracket

                    --depth;
                }
                else if (depth > 0)
                {
                    continue;
                }
                else if (text == ";")
                {
                    return false;
                }
                else if (text == ":")
                {
                    ++pending;
                }
                else if (text == "?")
                {
                    if (pending == 0)
                    {
                        return true;
                    }

                    --pending;
                }
            }

            return false;
        }

        // True when the `:` at sigs[colon] introduces a base clause or an enum
        // underlying type (`class A : B`, `enum class E : std::uint8_t`): a
        // `class`/`struct`/`union`/`enum` keyword earlier in the same statement.
        bool IsInheritanceColon(const std::vector<Sig>& sigs, std::size_t colon)
        {
            int steps = 0;
            for (auto k = colon; k > 0 && steps++ < kInheritanceScanLimit;)
            {
                --k;
                const std::string_view text = sigs[k].text;
                if (text == ";" || text == "{" || text == "}" || text == "=" || text == "?" ||
                    text == ":")
                {
                    return false;
                }

                if (sigs[k].kind == TokenKind::Identifier &&
                    (text == "class" || text == "struct" || text == "union" || text == "enum"))
                {
                    return true;
                }
            }

            return false;
        }

        // Gap for one side of a declarator `*` (pointer knob) or `&`/`&&`
        // (reference knob). `before_name` selects the gap between the operator
        // and the declared name (Right: none) vs between the type and the
        // operator (Right: one space); Left mirrors both.
        int DeclaratorGap(std::string_view   token,
                          bool               before_name,
                          PointerAlignment   pointer_alignment,
                          ReferenceAlignment reference_alignment)
        {
            const bool right = token == "*" ? pointer_alignment == PointerAlignment::Right
                                            : reference_alignment == ReferenceAlignment::Right;
            if (before_name)
            {
                return right ? 0 : 1;
            }

            return right ? 1 : 0;
        }

        // Gap decision between two adjacent significant tokens of one chunk.
        // Returns -1 to preserve the original gap (exactly one space iff the source
        // had whitespace there), 0 for no space, 1 for exactly one space.
        int SpacingGap(const std::vector<Sig>&   sigs,
                       std::size_t               prev,
                       std::size_t               cur,
                       std::string_view          source,
                       const std::vector<Token>& tokens,
                       PointerAlignment          pointer_alignment,
                       ReferenceAlignment        reference_alignment,
                       bool                      space_before_inheritance_colon,
                       bool                      space_after_c_style_cast,
                       bool                      space_after_logical_not,
                       bool                      space_before_cpp11_braced_list)
        {
            const std::string_view left       = sigs[prev].text;
            const std::string_view right      = sigs[cur].text;
            const TokenKind        left_kind  = sigs[prev].kind;
            const TokenKind        right_kind = sigs[cur].kind;
            const bool             left_word  = IsWordish(left_kind) || IsComment(left_kind);
            const bool             right_word = IsWordish(right_kind) || IsComment(right_kind);

            // `->` after `)` (trailing return `) -> int`): space before, none after.
            if (right == "->")
            {
                return IsTrailingReturnArrow(sigs, cur) ? 1 : 0;
            }

            if (left == "->")
            {
                return IsTrailingReturnArrow(sigs, prev) ? 1 : 0;
            }

            // Member access never takes spaces: `a.b`, `a->b`, `a.*pm`, `a->*pm`.
            if (left == "." || left == ".*" || left == "->*")
            {
                return 0;
            }

            if (right == "." || right == ".*" || right == "->*")
            {
                return 0;
            }

            if (right == "::" || left == "::")
            {
                return 0;
            }

            if (right == "," || right == ";" || right == ")" || right == "]" || right == "}")
            {
                return 0;
            }

            if (right == "[")
            {
                if ((left == "&" || left == "&&") && IsDeclaratorStar(sigs, prev))
                {
                    return DeclaratorGap(left, true, pointer_alignment, reference_alignment);
                }

                return 0;
            } // `a[0]`, `new int[5]`, `delete[] p`

            if (left == "(" || left == "[" || left == "{")
            {
                return 0;
            }

            if (left == "!" && right != "=")
            {
                return space_after_logical_not ? 1 : 0;
            }

            if (left == ")" && sigs[prev].match != kNoSig &&
                sigs[prev].match + 1 < prev &&
                IsTypeKeyword(sigs[sigs[prev].match + 1].text) &&
                right != ";" && right != "," && right != ")" && right != "]" &&
                right != "(" && right != "." && right != "->" && right != "::" &&
                right != "const" && right != "volatile" && right != "noexcept" &&
                right != "override" && right != "final" && right != "requires")
            {
                return space_after_c_style_cast ? 1 : 0;
            }

            if (right == "?")
            {
                return 1;
            }

            if (left == "?")
            {
                return 1;
            }

            // `:` attaches, except after `)` (constructor-init `C() : x(1)` and
            // parenthesized ternary branches `(a) : b` take a space).
            if (right == ":")
            {
                return left == ")" || IsRangeForColon(sigs, cur) || IsTernaryColon(sigs, cur) ||
                               (space_before_inheritance_colon && IsInheritanceColon(sigs, cur))
                           ? 1
                           : 0;
            }

            if (left == ":")
            {
                return 1;
            }

            if (right == "...")
            {
                return 0;
            }

            if (left == "...")
            {
                return right_word ? 1 : 0;
            }

            // `{`: attached for initializers (`Widget w{1}`, `={1}`, `({1})`,
            // `return` keeps its space); spaced for blocks (`) {`, `else {`).
            if (right == "{")
            {
                if (IsBlockOpenBrace(sigs, cur))
                {
                    return 1;
                }

                if (left == "=" || left == ")" || left == "," || left == "else" || left == "do" ||
                    left == "try" || left == ":" || left == "return" || left == "throw" ||
                    left == "co_return" || left == "co_yield")
                {
                    return 1;
                }

                return space_before_cpp11_braced_list && left != "{" && left != ";" ? 1 : 0;
            }

            if (right == "{" && (left == "{" || left == ";"))
            {
                return 0;
            }

            if (left == "}" || left == ")" || left == "]")
            {
                if (right == ";" || right == "," || right == ")" || right == "]" || right == "}" ||
                    right == "." || right == ".*" || right == "->*" || right == ":" ||
                    right == "::" || right == "[" || right == "(")
                {
                    return 0;
                }

                return 1; // `} else`, `) {`, `) const`, `] noexcept`
            }

            if ((left == "*" || left == "&" || left == "&&") && (right == ">" || right == ">>") &&
                left_kind == TokenKind::Punctuation && IsDeclaratorStar(sigs, prev))
            {
                return 0;
            } // `static_cast<void*>(p)`

            // `char **argv`, `T *&r`: stacked declarator operators stay together;
            // `*const` / `*volatile` follow the pointer alignment (`*` can't be binary
            // before a cv-qualifier).
            if ((left == "*" || left == "&" || left == "&&") &&
                (right == "*" || right == "&" || right == "&&") &&
                left_kind == TokenKind::Punctuation && right_kind == TokenKind::Punctuation &&
                IsDeclaratorStar(sigs, prev))
            {
                return 0;
            }

            if (left == "*" && (right == "const" || right == "volatile"))
            {
                return DeclaratorGap(left, true, pointer_alignment, reference_alignment);
            }

            if (right == "*" || right == "&" || right == "&&")
            {
                if (IsUnaryStar(sigs, cur))
                {
                    return 1;
                } // `return *p`, `(*p)`

                if (IsDeclaratorStar(sigs, cur))
                {
                    return DeclaratorGap(right, false, pointer_alignment, reference_alignment);
                }

                return 1; // binary
            }

            if (left == "*" || left == "&" || left == "&&")
            {
                if (IsUnaryStar(sigs, prev))
                {
                    return 0;
                } // `*p`, `&x`

                if (IsDeclaratorStar(sigs, prev))
                {
                    return DeclaratorGap(left, true, pointer_alignment, reference_alignment);
                }

                return 1; // binary
            }

            if (right == "(")
            {
                // Control keywords space out (`if (`) but calls, subscripts and
                // sizeof-like operators attach (`foo(`, `sizeof(`). Note: the lexer
                // reports keywords as Identifier, so the keyword check must come
                // before the wordish attach rule below.
                if (IsSpaceBeforeParenKeyword(left))
                {
                    return 1;
                }

                if (left_word || left == ")" || left == "]" || left == "}" || left == ">")
                {
                    return 0;
                }

                if (left_kind == TokenKind::Punctuation)
                {
                    if (left == "(" || left == "," || left == ";" || left == "{" || left == "[" ||
                        left == "=")
                    {
                        return 1;
                    }

                    return 0;
                }

                return IsSpaceBeforeParenKeyword(left) ? 1 : 0;
            }

            if (left == "," || left == ";")
            {
                return 1;
            }

            if (right == ";" && left == ";")
            {
                return 0;
            } // `for (;;)`

            if (left == "operator")
            {
                // `operator==`, `operator()` attach; `operator int`, `operator""_x` too.
                if (right_kind == TokenKind::StringLiteral ||
                    right_kind == TokenKind::CharacterLiteral ||
                    right_kind == TokenKind::RawStringLiteral)
                {
                    return 0;
                }

                return right_word ? 1 : 0;
            }

            if (left == "template")
            {
                return 1;
            } // `template <typename T>`

            // Template brackets vs. relational operators: preserve as typed (single
            // space iff the source had whitespace). See IsBinaryOperator.
            if (right == "<" || right == ">" || right == "<=" || right == ">=" || right == "<<" ||
                right == ">>")
            {
                return -1;
            }

            if (left == "<" || left == ">" || left == "<=" || left == ">=" || left == "<<" ||
                left == ">>")
            {
                return -1;
            }

            if (right == "!" && left == "if")
            {
                return 1;
            } // `if !consteval {`

            if (right == "~" && left == "=")
            {
                return 1; // `value = ~mask`, not `value =~mask`
            }

            if (right == "!" && (left == "=" || left == "return" || left == "co_return"))
            {
                return 1;
            }

            if (right == "!" || right == "~" || right == "++" || right == "--")
            {
                return 0;
            }

            if (left == "!" || left == "~" || left == "++" || left == "--")
            {
                return 0;
            }

            // Unary minus/plus (`-1`, `return -x`, `(-y)`) attaches after, keeps the
            // space before; binary keeps spaces on both sides.
            if ((right == "-" || right == "+") && IsUnaryMinus(sigs, cur))
            {
                return 1;
            }

            if ((left == "-" || left == "+") && IsUnaryMinus(sigs, prev))
            {
                return 0;
            }

            if (IsBinaryOperator(left) || IsBinaryOperator(right))
            {
                return 1;
            }

            if (left_word && right_word)
            {
                return 1;
            } // `int x`, `long long`

            (void) source;
            (void) tokens;
            return 1;
        }

        struct Chunk
        {
            std::size_t begin         = 0; // range into the Sig array
            std::size_t end           = 0; // empty range => comment-only line
            std::size_t source_line   = 0;
            bool        directive     = false;
            bool        verbatim      = false; // trivia-only line crossed by a multi-line token
            bool        starts_inside = false; // first token inside unclosed (/[ (not just closers)
            bool        closer_only   = false; // every token is `)`, `]`, `}`, `;` or `,`
            bool        prev_continues = false;
            bool        force_continue = false; // later piece of a column-limit break
            bool        is_label       = false;
            bool        label_brace    = false; // Allman `{` on its own line right after `case X:`
            // Dangling control headers (`if (x)`, `else`, `do` with the body on a
            // later chunk) indent following chunks. ends_dangling marks the header;
            // dangle_bonus is the cumulative nesting level. Only enabled for
            // single_line_style != Keep (Indent mode and pre-split inputs). Known
            // limit: an `else` after a nested braceless body aligns to base instead
            // of its `if` (output stays valid and stable).
            bool        ends_dangling  = false;
            int         dangle_bonus   = 0;
            std::size_t label_open_sig = static_cast<std::size_t>(-1); // index into Sigs
        };

        bool IsCloserish(std::string_view text)
        {
            return text == ")" || text == "]" || text == "}" || text == ";" || text == ",";
        }

        // Significant token texts starting a scope label, mirroring the line-head
        // detection of the indent pass (access labels, case/default, goto labels).
        bool ComputeIsLabel(const std::vector<Sig>& sigs, std::size_t begin, std::size_t end)
        {
            std::string_view head[kMaxLabelHeadTokens];
            TokenKind        head_kind[kMaxLabelHeadTokens] = {};
            std::size_t      head_count                     = 0;
            for (auto i = begin; i < end && head_count < kMaxLabelHeadTokens; ++i)
            {
                head[head_count]      = sigs[i].text;
                head_kind[head_count] = sigs[i].kind;
                ++head_count;
            }

            if (head_count >= kMinLabelTokens)
            {
                const bool access_label =
                    (head[0] == "public" || head[0] == "private" || head[0] == "protected") &&
                    head[1] == ":";
                const bool switch_label = head[0] == "case" || head[0] == "default";
                const bool goto_label   = head_count == kMinLabelTokens &&
                                          head_kind[0] == TokenKind::Identifier && head[1] == ":";
                return access_label || switch_label || goto_label;
            }

            if (head_count == 1)
            {
                return head[0] == "default";
            }

            return false;
        }

        // ---- Single-statement control blocks (single_line_style) -----------------
        // Text pre-pass: AddBraces inserts `{`/`}`, RemoveBraces deletes them,
        // SplitHeaders moves the body onto its own line, JoinHeaders pulls it back.
        // The main pipeline then lays everything out, so only token-correct edits
        // are made here. Only `if`/`else`/`for`/`while`/`do` bodies are touched;
        // function, record, namespace, lambda, switch/case and label blocks never
        // are (`catch`/`try` require braces in C++ and are skipped too).

        constexpr std::size_t kNoPos = static_cast<std::size_t>(-1);

        struct PSig
        {
            std::size_t      tok = 0; // lexer token index
            std::string_view text;
            TokenKind        kind   = TokenKind::Unknown;
            std::size_t      line   = 0;
            bool             direct = false; // on a preprocessor-directive line (verbatim)
        };

        bool PassInDirectives(std::size_t offset, const std::vector<PreprocessorDirective>& dirs)
        {
            for (const auto& directive : dirs)
            {
                if (directive.offset <= offset && offset < directive.offset + directive.length)
                {
                    return true;
                }
            }

            return false;
        }

        std::size_t PassMatchForward(
            const std::vector<PSig>& sigs, std::size_t open, char opener, char closer)
        {
            int depth = 0;
            for (auto i = open; i < sigs.size(); ++i)
            {
                if (sigs[i].direct)
                {
                    return kNoPos;
                }

                if (sigs[i].kind != TokenKind::Punctuation || sigs[i].text.size() != 1)
                {
                    continue;
                }

                if (sigs[i].text[0] == opener)
                {
                    ++depth;
                }
                else if (sigs[i].text[0] == closer)
                {
                    if (--depth == 0)
                    {
                        return i;
                    }
                }
            }

            return kNoPos;
        }

        std::size_t PassMatchBackward(
            const std::vector<PSig>& sigs, std::size_t close, char opener, char closer)
        {
            int depth = 0;
            for (auto i = close + 1; i > 0; --i)
            {
                const auto k = i - 1;
                if (sigs[k].direct)
                {
                    return kNoPos;
                }

                if (sigs[k].kind != TokenKind::Punctuation || sigs[k].text.size() != 1)
                {
                    continue;
                }

                if (sigs[k].text[0] == closer)
                {
                    ++depth;
                }
                else if (sigs[k].text[0] == opener)
                {
                    if (--depth == 0)
                    {
                        return k;
                    }
                }
            }

            return kNoPos;
        }

        // End of the single statement starting at `start`: index of its terminating
        // `;`, or kNoPos when the body is braced, empty, malformed, or crosses a
        // directive. Tracks all bracket kinds so `for(;;)`-style semicolons and
        // braced initializers inside expressions never end the scan early.
        std::size_t PassStmtEnd(const std::vector<PSig>& sigs, std::size_t start)
        {
            int paren = 0, bracket = 0, brace = 0;
            for (auto i = start; i < sigs.size(); ++i)
            {
                if (sigs[i].direct)
                {
                    return kNoPos;
                }

                // A bare `else` at depth 0 means malformed input (`if (a) else ...`):
                // never wrap it. (Keywords lex as Identifier.)
                if (sigs[i].kind == TokenKind::Identifier && sigs[i].text == "else" && paren == 0 &&
                    bracket == 0 && brace == 0)
                {
                    return kNoPos;
                }

                if (sigs[i].kind != TokenKind::Punctuation || sigs[i].text.size() != 1)
                {
                    continue;
                }

                const char c = sigs[i].text[0];
                if (c == '(')
                {
                    ++paren;
                }
                else if (c == ')')
                {
                    if (paren == 0)
                    {
                        return kNoPos;
                    }

                    --paren;
                }
                else if (c == '[')
                {
                    ++bracket;
                }
                else if (c == ']')
                {
                    if (bracket == 0)
                    {
                        return kNoPos;
                    }

                    --bracket;
                }
                else if (c == '{')
                {
                    if (paren == 0 && bracket == 0 && brace == 0)
                    {
                        return kNoPos;
                    } // braced body

                    ++brace;
                }
                else if (c == '}')
                {
                    if (brace == 0)
                    {
                        return kNoPos;
                    }

                    --brace;
                }
                else if (c == ';')
                {
                    if (paren == 0 && bracket == 0 && brace == 0)
                    {
                        return i;
                    }
                }
            }

            return kNoPos;
        }

        struct ControlHeader
        {
            std::size_t keyword        = 0; // sig index of if/for/while/else/do
            std::size_t header_end_off = 0; // offset just past `)` or the keyword
            std::size_t body_start     = 0; // sig index of the body
        };

        bool PassParseHeader(const std::vector<PSig>&  sigs,
                             const std::vector<Token>& tokens,
                             std::size_t               s,
                             ControlHeader&            out)
        {
            const std::string_view keyword = sigs[s].text;
            const bool takes_parens = keyword == "if" || keyword == "for" || keyword == "while";
            const bool bare         = keyword == "else" || keyword == "do";
            if ((!takes_parens && !bare) || sigs[s].kind != TokenKind::Identifier)
            {
                return false;
            }

            if (takes_parens)
            {
                if (s + 1 >= sigs.size() || sigs[s + 1].text != "(" ||
                    sigs[s + 1].kind != TokenKind::Punctuation)
                {
                    return false;
                }

                const std::size_t close = PassMatchForward(sigs, s + 1, '(', ')');
                if (close == kNoPos || close + 1 >= sigs.size())
                {
                    return false;
                }

                const Token& close_tok = tokens[sigs[close].tok];
                out                    = { s, close_tok.offset + close_tok.length, close + 1 };
                return true;
            }

            if (s + 1 >= sigs.size())
            {
                return false;
            }

            const Token& kw_tok = tokens[sigs[s].tok];
            out                 = { s, kw_tok.offset + kw_tok.length, s + 1 };
            return true;
        }

        bool PassBodyIsDeclaration(const std::vector<PSig>& sigs, std::size_t begin,
                                   std::size_t end)
        {
            // `if (a) { Widget w; }` must keep its braces: an unbraced declaration
            // substatement is ill-formed. Statements (`return x;`, `f();`, `x = y;`)
            // are recognized by their leading keyword or by the absence of
            // declaration-shaped token runs; anything uncertain keeps the braces
            // (keeping braces is always legal).
            if (begin >= end)
            {
                return false;
            }

            static constexpr std::string_view kStatements[] = {
                "return",    "goto",     "throw",    "delete", "new",
                "co_return", "co_yield", "co_await", "break",  "continue",
            };
            for (const auto keyword : kStatements)
            {
                if (sigs[begin].text == keyword)
                {
                    return false;
                }
            }

            if (IsTypeKeyword(sigs[begin].text) || IsQualifierKeyword(sigs[begin].text) ||
                sigs[begin].text == "static_assert" || sigs[begin].text == "template" ||
                sigs[begin].text == "typename")
            {
                return true;
            }

            // `Ident Ident` (`Widget w;`) or `Ident (*|&)… Ident` (`Widget *w;`);
            // `a * b;` (multiplication) is sacrificed to the safe side.
            for (auto i = begin; i + 1 < end; ++i)
            {
                if (sigs[i].kind != TokenKind::Identifier || sigs[i].text == "else")
                {
                    continue;
                }

                if (sigs[i + 1].kind == TokenKind::Identifier)
                {
                    return true;
                }

                std::size_t k = i + 1;
                while (k < end && (sigs[k].text == "*" || sigs[k].text == "&"))
                {
                    ++k;
                }

                if (k > i + 1 && k < end && sigs[k].kind == TokenKind::Identifier)
                {
                    return true;
                }
            }

            return false;
        }

        struct PassText
        {
            std::string                        work;
            std::vector<Token>                 tokens;
            std::vector<PreprocessorDirective> directives;
            std::vector<std::size_t>           line_starts;
            std::vector<PSig>                  sigs;

            explicit PassText(std::string source) : work(std::move(source))
            {
                tokens     = Lexer(work).Lex();
                directives = Preprocessor().Process(work).directives;
                line_starts.push_back(0);
                for (std::size_t i = 0; i < work.size(); ++i)
                {
                    if (work[i] == '\n')
                    {
                        line_starts.push_back(i + 1);
                    }
                }

                for (std::size_t t = 0; t < tokens.size(); ++t)
                {
                    if (IsTrivia(tokens[t].kind))
                    {
                        continue;
                    }

                    std::size_t lo = 0, hi = line_starts.size();
                    while (lo + 1 < hi)
                    {
                        const std::size_t mid = (lo + hi) / kMidpointDivisor;
                        if (line_starts[mid] <= tokens[t].offset)
                        {
                            lo = mid;
                        }
                        else
                        {
                            hi = mid;
                        }
                    }

                    PSig sig;
                    sig.tok    = t;
                    sig.text   = std::string_view(work).substr(tokens[t].offset, tokens[t].length);
                    sig.kind   = tokens[t].kind;
                    sig.line   = lo;
                    sig.direct = PassInDirectives(tokens[t].offset, directives);
                    sigs.push_back(sig);
                }
            }

            std::size_t TokEnd(std::size_t s) const
            {
                return tokens[sigs[s].tok].offset + tokens[sigs[s].tok].length;
            }

            bool RangeTouchesDirective(std::size_t begin, std::size_t end) const
            {
                for (auto i = begin; i <= end && i < sigs.size(); ++i)
                {
                    if (sigs[i].direct)
                    {
                        return true;
                    }
                }

                return false;
            }
        };

        std::string_view PassTrim(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                                     text.front() == '\n' || text.front() == '\r'))
            {
                text.remove_prefix(1);
            }

            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                                     text.back() == '\n' || text.back() == '\r'))
            {
                text.remove_suffix(1);
            }

            return text;
        }

        // Delete `{`/`}` around single-statement control bodies (shared by
        // SingleLine and Indent). Returns true when the text changed.
        bool RemoveBracesPass(std::string& work)
        {
            PassText pass(work);
            struct Span
            {
                std::size_t start = 0, end = 0;
                std::string replacement;
            };

            std::vector<Span> spans;
            for (std::size_t i = 0; i < pass.sigs.size(); ++i)
            {
                if (pass.sigs[i].text != "{" || pass.sigs[i].kind != TokenKind::Punctuation ||
                    pass.sigs[i].direct)
                {
                    continue;
                }

                // Owned by a control header? `)` of if/for/while, or `else`/`do`.
                // Anything else (functions, records, namespaces, lambdas, labels,
                // `try`/`catch`, switch bodies) is never touched.
                bool owned = false;
                if (i > 0 && !pass.sigs[i - 1].direct)
                {
                    const std::string_view prev = pass.sigs[i - 1].text;
                    if (prev == ")")
                    {
                        const std::size_t open = PassMatchBackward(pass.sigs, i - 1, '(', ')');
                        if (open != kNoPos && open > 0 &&
                            pass.sigs[open - 1].kind == TokenKind::Identifier)
                        {
                            const std::string_view head = pass.sigs[open - 1].text;
                            owned = head == "if" || head == "for" || head == "while";
                        }
                    }
                    else if (prev == "else" || prev == "do")
                    {
                        owned = pass.sigs[i - 1].kind == TokenKind::Identifier;
                    }
                }

                if (!owned)
                {
                    continue;
                }

                const std::size_t close = PassMatchForward(pass.sigs, i, '{', '}');
                if (close == kNoPos)
                {
                    continue;
                }

                if (pass.RangeTouchesDirective(i, close))
                {
                    continue;
                }

                // Body: non-empty, brace-free, exactly one depth-0 `;` at the end.
                if (close == i + 1)
                {
                    continue;
                } // `{}`

                bool        nested = false;
                std::size_t semis  = 0;
                {
                    int paren = 0, bracket = 0;
                    for (auto k = i + 1; k < close; ++k)
                    {
                        if (pass.sigs[k].kind != TokenKind::Punctuation ||
                            pass.sigs[k].text.size() != 1)
                        {
                            continue;
                        }

                        const char c = pass.sigs[k].text[0];
                        if (c == '(')
                        {
                            ++paren;
                        }
                        else if (c == ')')
                        {
                            --paren;
                        }
                        else if (c == '[')
                        {
                            ++bracket;
                        }
                        else if (c == ']')
                        {
                            --bracket;
                        }
                        else if (c == '{' || c == '}')
                        {
                            nested = true;
                        }
                        else if (c == ';' && paren == 0 && bracket == 0)
                        {
                            ++semis;
                        }
                    }
                }
                if (nested || semis != 1)
                {
                    continue;
                }

                if (pass.sigs[close - 1].text != ";")
                {
                    continue;
                }

                // Dangling else: `if (a) { if (b) x(); } else y();` keeps its braces.
                if (close + 1 < pass.sigs.size() && pass.sigs[close + 1].text == "else" &&
                    pass.sigs[i + 1].text == "if")
                {
                    continue;
                }

                if (PassBodyIsDeclaration(pass.sigs, i + 1, close - 1))
                {
                    continue;
                }

                const std::size_t      brace_start = pass.tokens[pass.sigs[i].tok].offset;
                const Token&           close_tok   = pass.tokens[pass.sigs[close].tok];
                const std::size_t      close_end   = close_tok.offset + close_tok.length;
                const std::string_view inner       = PassTrim(std::string_view(pass.work).substr(
                    pass.TokEnd(i), close_tok.offset - pass.TokEnd(i)));
                spans.push_back({ brace_start, close_end, std::string(inner) });
            }

            if (spans.empty())
            {
                return false;
            }

            std::sort(spans.begin(), spans.end(),
                      [](const Span& left, const Span& right) { return left.start > right.start; });
            for (const auto& span : spans)
            {
                work.replace(span.start, span.end - span.start, span.replacement);
            }

            return true;
        }

        // Insert `{`/`}` around unbraced single-statement bodies (IndentWithBraces).
        bool AddBracesPass(std::string& work)
        {
            PassText pass(work);
            struct Insertion
            {
                std::size_t pos;
                std::string text;
                std::size_t erase; // bytes removed at `pos` before inserting
            };

            std::vector<Insertion> insertions;
            for (std::size_t s = 0; s < pass.sigs.size(); ++s)
            {
                const std::string_view keyword = pass.sigs[s].text;
                if (keyword != "if" && keyword != "for" && keyword != "while" &&
                    keyword != "else" && keyword != "do")
                {
                    continue;
                }

                if (pass.sigs[s].kind != TokenKind::Identifier || pass.sigs[s].direct)
                {
                    continue;
                }

                ControlHeader header;
                if (!PassParseHeader(pass.sigs, pass.tokens, s, header))
                {
                    continue;
                }

                if (pass.RangeTouchesDirective(s, header.body_start))
                {
                    continue;
                }

                if (pass.sigs[header.body_start].text == "{")
                {
                    continue;
                } // already braced

                if (pass.sigs[header.body_start].text == ";")
                {
                    continue;
                } // empty body stays bare

                // `else if` chains stay together; the inner `if` is braced on its own.
                if (keyword == "else" && pass.sigs[header.body_start].text == "if")
                {
                    continue;
                }

                const std::size_t stmt_end = PassStmtEnd(pass.sigs, header.body_start);
                if (stmt_end == kNoPos || pass.RangeTouchesDirective(s, stmt_end))
                {
                    continue;
                }

                insertions.push_back({ header.header_end_off, " {", 0 });
                // `x(); }` + newline + `else`: join so the `}` cuddles the `else`
                // (the layout pass only keeps line breaks as typed).
                const std::size_t close_at = pass.TokEnd(stmt_end);
                std::size_t       erase    = 0;
                if (stmt_end + 1 < pass.sigs.size() && pass.sigs[stmt_end + 1].text == "else" &&
                    !pass.sigs[stmt_end + 1].direct)
                {
                    const std::size_t else_off = pass.tokens[pass.sigs[stmt_end + 1].tok].offset;
                    const std::string_view gap =
                        std::string_view(pass.work).substr(close_at, else_off - close_at);
                    if (gap.find_first_not_of(" \t\r\n") == std::string_view::npos)
                    {
                        erase = gap.size();
                    }
                }

                insertions.push_back({ close_at, erase ? " } " : " }", erase });
            }

            if (insertions.empty())
            {
                return false;
            }

            std::sort(insertions.begin(), insertions.end(),
                      [](const auto& left, const auto& right) { return left.pos > right.pos; });
            for (const auto& ins : insertions)
            {
                work.erase(ins.pos, ins.erase);
                work.insert(ins.pos, ins.text);
            }

            return true;
        }

        // Move an unbraced body on the header line onto its own line (Indent).
        bool SplitHeadersPass(std::string& work)
        {
            PassText                 pass(work);
            std::vector<std::size_t> breaks;
            for (std::size_t s = 0; s < pass.sigs.size(); ++s)
            {
                const std::string_view keyword = pass.sigs[s].text;
                if (keyword != "if" && keyword != "for" && keyword != "while" &&
                    keyword != "else" && keyword != "do")
                {
                    continue;
                }

                if (pass.sigs[s].kind != TokenKind::Identifier || pass.sigs[s].direct)
                {
                    continue;
                }

                ControlHeader header;
                if (!PassParseHeader(pass.sigs, pass.tokens, s, header))
                {
                    continue;
                }

                if (pass.RangeTouchesDirective(s, header.body_start))
                {
                    continue;
                }

                if (pass.sigs[header.body_start].text == "{")
                {
                    continue;
                }

                if (pass.sigs[header.body_start].text == ";")
                {
                    continue;
                }

                // `else if` chains stay together; the inner `if` splits on its own.
                if (keyword == "else" && pass.sigs[header.body_start].text == "if")
                {
                    continue;
                }

                const std::size_t body_off = pass.tokens[pass.sigs[header.body_start].tok].offset;
                if (pass.sigs[header.body_start].line != pass.sigs[s].line)
                {
                    continue;
                } // already split

                const std::string_view gap = std::string_view(pass.work).substr(
                    header.header_end_off, body_off - header.header_end_off);
                if (gap.find('/') != std::string_view::npos)
                {
                    continue;
                } // keep comments in place

                breaks.push_back(body_off);
            }

            if (breaks.empty())
            {
                return false;
            }

            std::sort(breaks.begin(), breaks.end(),
                      [](const std::size_t left, const std::size_t right) { return left > right; });
            for (const auto pos : breaks)
            {
                work.insert(pos, "\n");
            }

            return true;
        }

        // Pull a split unbraced body back onto the header line (SingleLine).
        bool JoinHeadersPass(std::string& work)
        {
            PassText pass(work);
            struct Join
            {
                std::size_t start = 0, end = 0;
            };

            std::vector<Join> joins;
            for (std::size_t s = 0; s < pass.sigs.size(); ++s)
            {
                const std::string_view keyword = pass.sigs[s].text;
                if (keyword != "if" && keyword != "for" && keyword != "while" &&
                    keyword != "else" && keyword != "do")
                {
                    continue;
                }

                if (pass.sigs[s].kind != TokenKind::Identifier || pass.sigs[s].direct)
                {
                    continue;
                }

                ControlHeader header;
                if (!PassParseHeader(pass.sigs, pass.tokens, s, header))
                {
                    continue;
                }

                if (pass.RangeTouchesDirective(s, header.body_start))
                {
                    continue;
                }

                if (pass.sigs[header.body_start].text == "{")
                {
                    continue;
                }

                if (pass.sigs[header.body_start].text == ";")
                {
                    continue;
                }

                const std::size_t body_off = pass.tokens[pass.sigs[header.body_start].tok].offset;
                if (pass.sigs[header.body_start].line == pass.sigs[s].line)
                {
                    continue;
                } // already joined

                const std::string_view gap = std::string_view(pass.work).substr(
                    header.header_end_off, body_off - header.header_end_off);
                if (gap.find('/') != std::string_view::npos)
                {
                    continue;
                }

                joins.push_back({ header.header_end_off, body_off });
            }

            if (joins.empty())
            {
                return false;
            }

            std::sort(joins.begin(), joins.end(),
                      [](const Join& left, const Join& right) { return left.start > right.start; });
            for (const auto& join : joins)
            {
                work.replace(join.start, join.end - join.start, " ");
            }

            return true;
        }

        bool IsTrailingReturnArrow(const std::vector<Sig>& sigs, std::size_t k)
        {
            // `->` introducing a trailing return type (`auto f() -> int {`) takes
            // spaces; member access (`p->x`, `foo()->x()`) stays attached. The
            // lookahead past the type name disambiguates: a trailing type is followed
            // by `{`, `;`, `,`, `noexcept`/`requires`, `= default`/`= delete`, or
            // pointer markers before those — a member name never is.
            if (sigs[k].kind != TokenKind::Punctuation || sigs[k].text != "->")
            {
                return false;
            }

            if (k + 1 >= sigs.size() || sigs[k + 1].kind != TokenKind::Identifier)
            {
                return false;
            }

            // A trailing return follows a function declarator: `)`, a qualifier
            // (`const`, `noexcept`, `mutable`, `&`...), or the capture list of a
            // parameterless lambda (`[] -> int {`). After a name, a literal or `]`
            // it is member access written with spaces: `a -> b;`, `v[0] -> s;`.
            if (k > 0)
            {
                const std::string_view before = sigs[k - 1].text;
                const bool             declarator_end =
                    before == ")" || before == "const" || before == "volatile" ||
                    before == "noexcept" || before == "mutable" || before == "override" ||
                    before == "final" || before == "&" || before == "&&" || before == "constexpr" ||
                    before == "consteval";
                if (before == "]")
                {
                    return k + kTwoTokenOffset < sigs.size() &&
                           sigs[k + kTwoTokenOffset].text == "{";
                }

                if (!declarator_end)
                {
                    return false;
                }
            }

            if (k + kTwoTokenOffset >= sigs.size())
            {
                return true;
            } // incomplete trailing type at EOF

            const std::string_view after = sigs[k + kTwoTokenOffset].text;
            if (after == "{" || after == ";" || after == "," || after == "noexcept" ||
                after == "requires")
            {
                return true;
            }

            if (after == "*" || after == "&")
            {
                if (k + kThreeTokenOffset >= sigs.size())
                {
                    return true;
                }

                const std::string_view tail = sigs[k + kThreeTokenOffset].text;
                return tail == "{" || tail == ";" || tail == ",";
            }

            if (after == "=" && k + kThreeTokenOffset < sigs.size())
            {
                const std::string_view tail = sigs[k + kThreeTokenOffset].text;
                return tail == "default" || tail == "delete";
            }

            return false;
        }

        // True when the `,` at sigs[comma] separates entries of an enclosing
        // enumerator list or braced initializer (`enum E { A, B }`, `{1, 2}`):
        // the next entry then sits at the same level instead of continuing a
        // statement.
        bool IsEntryComma(const std::vector<Sig>& sigs, std::size_t comma)
        {
            int         depth = 0;
            std::size_t open  = kNoSig;
            for (auto k = comma; k > 0;)
            {
                --k;
                if (sigs[k].kind != TokenKind::Punctuation)
                {
                    continue;
                }

                if (sigs[k].text == "}")
                {
                    ++depth;
                }
                else if (sigs[k].text == "{")
                {
                    if (depth == 0)
                    {
                        open = k;
                        break;
                    }

                    --depth;
                }
            }

            if (open == kNoSig)
            {
                return false;
            }

            if (!IsBlockOpenBrace(sigs, open))
            {
                return true;
            }

            for (auto k = open, steps = std::size_t { 0 }; k > 0 && steps < kEntryCommaScanLimit;
                 ++steps)
            {
                --k;
                if (sigs[k].text == ";" || sigs[k].text == "{" || sigs[k].text == "}")
                {
                    break;
                }

                if (sigs[k].kind == TokenKind::Identifier &&
                    (sigs[k].text == "enum" || sigs[k].text == "new"))
                {
                    return true;
                }
            }

            return false;
        }

        // True when the `}` at sigs[close] ends a `class`/`struct`/`union`/`enum`
        // definition body (not a function that merely returns such a type).
        bool IsTypeDefinitionClose(const std::vector<Sig>& sigs, std::size_t close)
        {
            if (close >= sigs.size() || sigs[close].text != "}" ||
                sigs[close].kind != TokenKind::Punctuation)
            {
                return false;
            }

            const std::size_t open = sigs[close].match;
            if (open == kNoSig || open == 0 || open >= sigs.size() || sigs[open].text != "{")
            {
                return false;
            }

            if (sigs[open - 1].text == ")" || sigs[open - 1].text == "]")
            {
                return false;
            }

            int steps = 0;
            for (auto k = open; k > 0 && steps++ < kTypeDefinitionScanLimit;)
            {
                --k;
                const std::string_view text = sigs[k].text;
                if (text == ";" || text == "{" || text == "}" || text == "=")
                {
                    return false;
                }

                if (sigs[k].kind == TokenKind::Identifier &&
                    (text == "class" || text == "struct" || text == "union" || text == "enum"))
                {
                    return true;
                }
            }

            return false;
        }

        // True when the `}` at sigs[close] ends the braced body of a control
        // statement: `if`/`else if`/`else`/`for`/`while`/`switch`/`do`/`try`/`catch`
        // (including `if constexpr` and range-for headers).
        bool IsControlBlockClose(const std::vector<Sig>& sigs, std::size_t close)
        {
            if (close >= sigs.size() || sigs[close].text != "}" ||
                sigs[close].kind != TokenKind::Punctuation)
            {
                return false;
            }

            const std::size_t open = sigs[close].match;
            if (open == kNoSig || open == 0 || open >= sigs.size() || sigs[open].text != "{")
            {
                return false;
            }

            const Sig& before = sigs[open - 1];
            if (before.kind == TokenKind::Identifier)
            {
                return before.text == "else" || before.text == "do" || before.text == "try" ||
                       IsConstevalIfHead(sigs, open - 1);
            }

            if (before.text != ")" || before.match == kNoSig || before.match == 0)
            {
                return false;
            }

            std::size_t keyword = before.match - 1;
            if (sigs[keyword].text == "constexpr" || sigs[keyword].text == "consteval")
            {
                if (keyword == 0)
                {
                    return false;
                }

                --keyword;
            }

            const std::string_view text = sigs[keyword].text;
            return sigs[keyword].kind == TokenKind::Identifier &&
                   (text == "if" || text == "for" || text == "while" || text == "switch" ||
                    text == "catch");
        }

        // True when sigs[k] sits in the base list of a record header: a `:` and then
        // a `class`/`struct`/`union`/`enum` keyword earlier in the same statement.
        bool InBaseList(const std::vector<Sig>& sigs, std::size_t k)
        {
            bool colon = false;
            int  steps = 0;
            while (k > 0 && steps++ < kBaseListScanLimit)
            {
                --k;
                const std::string_view text = sigs[k].text;
                if (text == ";" || text == "{" || text == "}" || text == "=" || text == "(" ||
                    text == ")")
                {
                    return false;
                }

                if (text == ":")
                {
                    colon = true;
                }
                else if (colon && sigs[k].kind == TokenKind::Identifier &&
                         (text == "class" || text == "struct" || text == "union" || text == "enum"))
                {
                    return true;
                }
            }

            return false;
        }

        // True when the `{` at sigs[brace] opens a block (function body, named
        // scope, control/label/bare block) rather than an initializer or
        // expression. Backward scan over the declarator tail: reaching an unmatched
        // `)` (function-style, unless the parens belong to `new`/`delete`),
        // `]` (lambda), `{`/`}`/`:` or a block-introducing keyword means block;
        // reaching `=`/`(`/`[`/`,`, `;`, a number or an expression operator means
        // initializer/expression. Covers declarator suffixes the old
        // previous-token check missed: `const`, `noexcept`, `override`, `-> Type`,
        // `requires`-clauses, `&`/`&&` qualifiers, `new`/`delete` guards.
        // `if consteval {` and `if !consteval {` have no parentheses to recognize.
        bool IsConstevalIfHead(const std::vector<Sig>& sigs, std::size_t consteval_index)
        {
            if (consteval_index == 0 || sigs[consteval_index].text != "consteval")
            {
                return false;
            }

            const std::size_t before = consteval_index - 1;
            return sigs[before].text == "if" ||
                   (sigs[before].text == "!" && before > 0 && sigs[before - 1].text == "if");
        }

        bool IsBlockOpenBrace(const std::vector<Sig>& sigs, std::size_t brace)
        {
            if (brace > 0 && IsConstevalIfHead(sigs, brace - 1))
            {
                return true;
            }

            // Member/variable brace-initializer `Type name{...}` (`Command command{};`):
            // a declared name right after a type, with no record/enum/namespace
            // header or base-clause colon earlier in the statement.
            if (brace >= kTwoTokenOffset && sigs[brace - 1].kind == TokenKind::Identifier &&
                (sigs[brace - kTwoTokenOffset].kind == TokenKind::Identifier ||
                 sigs[brace - kTwoTokenOffset].text == ">" ||
                 sigs[brace - kTwoTokenOffset].text == "*" ||
                 sigs[brace - kTwoTokenOffset].text == "&" ||
                 sigs[brace - kTwoTokenOffset].text == "::"))
            {
                static constexpr std::string_view kHeaderWords[] = {
                    "else",    "do",       "try",      "namespace", "class",    "struct",
                    "union",   "enum",     "extern",   "export",    "const",    "volatile",
                    "mutable", "noexcept", "override", "final",     "requires", "template",
                    "return",  "throw",    "new",      "delete",    "public",   "protected",
                    "private", "virtual",  "case",     "default",   "typename",
                };
                bool header = false;
                for (auto k = brace; k > 0 && !header;)
                {
                    --k;
                    const Sig& t = sigs[k];
                    if (t.text == ";" || t.text == "{" || t.text == "}" || t.text == ")" ||
                        t.text == "]" || t.text == "=" || t.text == "(")
                    {
                        break;
                    }

                    if (t.text == ":" || t.text == "->")
                    {
                        header = true;
                    }
                    else if (t.kind == TokenKind::Identifier)
                    {
                        // `class A : public B, private C {`: a record/enum keyword
                        // ahead of the base list makes this a definition body.
                        if (t.text == "class" || t.text == "struct" || t.text == "union" ||
                            t.text == "enum")
                        {
                            return true;
                        }

                        for (const auto word : kHeaderWords)
                        {
                            if (t.text == word)
                            {
                                header = true;
                            }
                        }
                    }
                }

                if (!header)
                {
                    return false;
                }
            }

            static constexpr std::string_view kTrueKeywords[] = {
                "else",     "do",     "try",      "namespace", "class",    "struct",  "union",
                "enum",     "extern", "export",   "const",     "volatile", "mutable", "noexcept",
                "override", "final",  "requires", "typename",  "template",
            };
            static constexpr std::string_view kFalseKeywords[] = {
                "return",   "sizeof",    "alignof",  "static_assert",
                "co_await", "co_return", "co_yield", "throw",
            };
            int steps = 0;
            for (auto k = brace; k > 0 && steps++ < kBlockBraceScanLimit;)
            {
                --k;
                const Sig& s = sigs[k];
                if (s.kind == TokenKind::Identifier)
                {
                    for (const auto keyword : kTrueKeywords)
                    {
                        if (s.text == keyword)
                        {
                            return true;
                        }
                    }

                    for (const auto keyword : kFalseKeywords)
                    {
                        if (s.text == keyword)
                        {
                            return false;
                        }
                    }

                    // new and delete followed by a brace-init are block-open braces
                    // (e.g., `new Node{...}`, `delete p{...}`) so they should trigger
                    // Allman style formatting.
                    if (s.text == "new" || s.text == "delete")
                    {
                        // Look ahead to see if there's a `{` after new/delete
                        // If so, treat it as a block-open brace
                        for (auto m = k + 1; m < sigs.size(); ++m)
                        {
                            if (sigs[m].text == "{")
                            {
                                return true;
                            }

                            if (sigs[m].text == ";" || sigs[m].text == "}" || sigs[m].text == "]")
                            {
                                break;
                            }
                        }
                    }

                    continue; // names and qualifiers are skipped over
                }

                if (s.kind == TokenKind::StringLiteral || s.kind == TokenKind::CharacterLiteral ||
                    s.kind == TokenKind::RawStringLiteral)
                {
                    continue;
                } // `extern "C" {`

                if (s.kind != TokenKind::Punctuation)
                {
                    return false;
                } // numbers etc.

                if (s.text.size() != 1)
                {
                    // `::`, `...`, `>>`, `&&` and `->` are skipped over (nested
                    // names, variadics, nested template closers, ref-qualifiers and
                    // trailing returns); anything else (`==`, `||`, `<<`) sits in an
                    // expression.
                    if (s.text == "::" || s.text == "..." || s.text == ">>" || s.text == "&&" ||
                        s.text == "->")
                    {
                        continue;
                    }

                    return false;
                }

                switch (s.text[0])
                {
                    case '<':
                    case '>':
                        continue; // template brackets inside declarator tails
                    case '*':
                    case '&':
                        continue; // pointer/reference declarator parts
                    case ')': {
                        // Function-style body — unless the parens belong to an
                        // expression operator (`new (buf) T{1}`, `delete (p) x{...}`).
                        if (s.match != kNoPos && s.match > 0)
                        {
                            const Sig& before = sigs[s.match - 1];
                            if (before.kind == TokenKind::Identifier &&
                                (before.text == "new" || before.text == "delete"))
                            {
                                return false;
                            }
                        }

                        return true;
                    }
                    case ']':
                        return true; // `[&] {` lambda (or `[&]() {`, via `)`)
                    case '{':
                    case '}':
                    case ':':
                        return true;
                    case ';':
                    case '=':
                    case '(':
                    case '[':
                        return false;
                    case ',':
                        // `class A : B, C {`: a comma inside a base list.
                        return InBaseList(sigs, k);
                    default:
                        return false; // expression operators
                }
            }

            return false;
        }

        // An empty `{}` never breaks: pull `{` and `}` together (`{\n}` -> `{}`) and
        // the pair up onto the header line (`) : x(1)` + newline + `{}` joins).
        // Comments in either gap and directive lines keep the text as typed.
        bool JoinEmptyBracesPass(std::string& work)
        {
            PassText pass(work);
            struct Edit
            {
                std::size_t start = 0, end = 0;
                const char* text = "";
            };

            std::vector<Edit> edits;
            const auto        blank_gap = [&](std::size_t from, std::size_t to) {
                const std::string_view gap = std::string_view(pass.work).substr(from, to - from);
                return gap.find_first_not_of(" \t\r\n") == std::string_view::npos;
            };
            for (std::size_t s = 1; s + 1 < pass.sigs.size(); ++s)
            {
                if (pass.sigs[s].text != "{" || pass.sigs[s].kind != TokenKind::Punctuation ||
                    pass.sigs[s + 1].text != "}" || pass.sigs[s].direct ||
                    pass.sigs[s + 1].direct || pass.RangeTouchesDirective(s - 1, s + 1))
                {
                    continue;
                }

                const std::size_t open_off  = pass.tokens[pass.sigs[s].tok].offset;
                const std::size_t close_off = pass.tokens[pass.sigs[s + 1].tok].offset;
                if (pass.sigs[s + 1].line != pass.sigs[s].line &&
                    blank_gap(open_off + 1, close_off))
                {
                    edits.push_back({ open_off + 1, close_off, "" });
                }

                const std::string_view prev = pass.sigs[s - 1].text;
                if (prev == ";" || prev == "{" || prev == "}")
                {
                    continue;
                }

                const std::size_t prev_end = pass.TokEnd(s - 1);
                if (pass.sigs[s - 1].line != pass.sigs[s].line && blank_gap(prev_end, open_off))
                {
                    edits.push_back({ prev_end, open_off, " " });
                }
            }

            if (edits.empty())
            {
                return false;
            }

            std::sort(edits.begin(), edits.end(),
                      [](const Edit& left, const Edit& right) { return left.start > right.start; });
            for (const auto& edit : edits)
            {
                work.replace(edit.start, edit.end - edit.start, edit.text);
            }

            return true;
        }

        std::string ApplySingleLineStyle(std::string_view source, SingleLineStyle mode)
        {
            std::string work(source);
            for (int iter = 0; iter < kSingleLineMaxPasses; ++iter)
            {
                if (mode == SingleLineStyle::SingleLine || mode == SingleLineStyle::Indent)
                {
                    if (RemoveBracesPass(work))
                    {
                        continue;
                    }
                }

                if (mode == SingleLineStyle::IndentWithBraces)
                {
                    if (AddBracesPass(work))
                    {
                        continue;
                    }
                }
                else if (mode == SingleLineStyle::Indent)
                {
                    if (SplitHeadersPass(work))
                    {
                        continue;
                    }
                }
                else if (mode == SingleLineStyle::SingleLine)
                {
                    if (JoinHeadersPass(work))
                    {
                        continue;
                    }
                }

                break;
            }

            return work;
        }

    } // namespace

    std::string Formatter::Format(std::string_view source) const
    {
        if (m_options.blank_line_between_methods || m_options.max_parameters_per_line != 0)
        {
            return Format(ParseTree::Parse(source, {}));
        }

        const auto tokens     = Lexer(source).Lex();
        const auto directives = Preprocessor().Process(source).directives;
        return FormatImpl(source, tokens, directives);
    }

    std::string Formatter::Format(const ParseTree& tree) const
    {
        const auto layout = DeclarationLayout(tree);
        if (layout != tree.Source())
        {
            const auto tokens     = Lexer(layout).Lex();
            const auto directives = Preprocessor().Process(layout).directives;
            return FormatImpl(layout, tokens, directives);
        }

        return FormatImpl(tree.Source(), tree.Tokens(), tree.Directives());
    }

    std::string Formatter::FormatImpl(std::string_view source, const std::vector<Token>& tokens,
                                      const std::vector<PreprocessorDirective>& directives) const
    {
        std::string altered(source);
        JoinEmptyBracesPass(altered);
        if (m_options.single_line_style != SingleLineStyle::Keep)
        {
            altered = ApplySingleLineStyle(altered, m_options.single_line_style);
        }

        if (altered == source)
        {
            return FormatShaped(source, tokens, directives);
        }

        const auto fresh_tokens     = Lexer(altered).Lex();
        const auto fresh_directives = Preprocessor().Process(altered).directives;
        return FormatShaped(altered, fresh_tokens, fresh_directives);
    }

    std::string Formatter::FormatShaped(std::string_view source, const std::vector<Token>& tokens,
                                        const std::vector<PreprocessorDirective>& directives) const
    {
        if (source.empty())
            return {};

        // ---- Line table -------------------------------------------------------
        std::vector<std::size_t> line_starts = { 0 };
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            if (source[i] == '\n')
            {
                line_starts.push_back(i + 1);
            }
        }

        const std::size_t line_count = line_starts.size();
        auto line_range = [&](std::size_t line) -> std::pair<std::size_t, std::size_t> {
            // [content_start, content_end): content_end excludes a trailing '\r'.
            const std::size_t start = line_starts[line];
            const std::size_t stop =
                line + 1 < line_starts.size() ? line_starts[line + 1] - 1 : source.size();
            std::size_t end = stop;
            if (end > start && source[end - 1] == '\r')
            {
                --end;
            }

            return { start, end };
        };
        auto line_ending = [&](std::size_t line) -> std::string_view {
            const std::size_t stop =
                line + 1 < line_starts.size() ? line_starts[line + 1] - 1 : source.size();
            if (stop < source.size() && source[stop] == '\n')
            {
                if (stop > line_starts[line] && source[stop - 1] == '\r')
                {
                    return source.substr(stop - 1, kCrlfLength);
                }

                return source.substr(stop, 1);
            }

            if (stop < source.size() && source[stop] == '\r')
            {
                return source.substr(stop, 1);
            }

            return {};
        };
        const bool file_uses_crlf = source.find("\r\n") != std::string_view::npos;

        // Lines crossed by a multi-line token (raw string, block comment):
        // precomputed once — any non-whitespace token spanning lines marks
        // every line it touches. Those lines go verbatim: rendering such a
        // token inline would splice its embedded newlines into the output and
        // duplicate the spanned lines.
        std::vector<char> line_spanned(line_count, 0);
        for (std::size_t t = 0; t < tokens.size(); ++t)
        {
            if (tokens[t].kind == TokenKind::Whitespace || tokens[t].length == 0)
            {
                continue;
            }

            const std::size_t tok_end = tokens[t].offset + tokens[t].length;
            std::size_t       first = 0, last = 0;
            {
                std::size_t lo = 0, hi = line_starts.size();
                while (lo + 1 < hi)
                {
                    const std::size_t mid = (lo + hi) / kMidpointDivisor;
                    if (line_starts[mid] <= tokens[t].offset)
                    {
                        lo = mid;
                    }
                    else
                    {
                        hi = mid;
                    }
                }

                first = lo;
            }
            {
                std::size_t lo = 0, hi = line_starts.size();
                while (lo + 1 < hi)
                {
                    const std::size_t mid = (lo + hi) / kMidpointDivisor;
                    if (line_starts[mid] <= tok_end - 1)
                    {
                        lo = mid;
                    }
                    else
                    {
                        hi = mid;
                    }
                }

                last = lo;
            }
            // Only multi-line tokens mark lines; single-line tokens touch just
            // their own line and must not force verbatim.
            if (last == first)
            {
                continue;
            }

            for (auto line = first; line <= last && line < line_count; ++line)
            {
                line_spanned[line] = 1;
            }
        }

        // ---- Directive lines --------------------------------------------------
        std::vector<char> is_directive_line(line_count, 0);
        {
            std::size_t cursor = 0;
            for (std::size_t line = 0; line < line_count; ++line)
            {
                is_directive_line[line] =
                    static_cast<char>(IsDirectiveLine(line_starts[line], directives, cursor));
            }
        }

        // ---- Significant tokens with global depth context ---------------------
        std::vector<Sig> sigs;
        sigs.reserve(tokens.size());
        for (std::size_t t = 0; t < tokens.size(); ++t)
        {
            if (IsTrivia(tokens[t].kind))
            {
                continue;
            }

            std::size_t line = 0;
            {
                std::size_t lo = 0, hi = line_starts.size();
                while (lo + 1 < hi)
                {
                    const std::size_t mid = (lo + hi) / kMidpointDivisor;
                    if (line_starts[mid] <= tokens[t].offset)
                    {
                        lo = mid;
                    }
                    else
                    {
                        hi = mid;
                    }
                }

                line = lo;
            }
            if (line < line_count && is_directive_line[line])
            {
                continue;
            } // verbatim

            Sig sig;
            sig.token = t;
            sig.text  = TokenText(source, tokens[t]);
            sig.kind  = tokens[t].kind;
            sig.line  = line;
            sigs.push_back(sig);
        }

        {
            std::size_t              paren = 0, bracket = 0;
            std::vector<char>        cond_stack;
            std::vector<std::size_t> open_stack;  // `(` sig indices for match links
            std::vector<std::size_t> brace_stack; // `{` sig indices for match links
            for (std::size_t s = 0; s < sigs.size(); ++s)
            {
                Sig& sig          = sigs[s];
                sig.paren_depth   = paren;
                sig.bracket_depth = bracket;
                sig.in_condition  = !cond_stack.empty() && cond_stack.back();
                if (sig.kind != TokenKind::Punctuation || sig.text.size() != 1)
                {
                    continue;
                }

                const char c = sig.text[0];
                if (c == '(')
                {
                    // The `(` is a condition when the previous significant token
                    // on the same line is if/for/while/switch.
                    bool cond = false;
                    if (s > 0 && sigs[s - 1].line == sig.line &&
                        sigs[s - 1].kind == TokenKind::Identifier &&
                        IsControlKeyword(sigs[s - 1].text))
                    {
                        cond = true;
                    }

                    cond_stack.push_back(static_cast<char>(cond));
                    open_stack.push_back(s);
                    ++paren;
                }
                else if (c == ')')
                {
                    if (paren > 0)
                    {
                        --paren;
                    }

                    if (!cond_stack.empty())
                    {
                        cond_stack.pop_back();
                    }

                    if (!open_stack.empty())
                    {
                        const std::size_t open = open_stack.back();
                        open_stack.pop_back();
                        sig.match        = open;
                        sigs[open].match = s;
                    }
                }
                else if (c == '[')
                {
                    ++bracket;
                }
                else if (c == ']')
                {
                    if (bracket > 0)
                    {
                        --bracket;
                    }
                }
                else if (c == '{')
                {
                    brace_stack.push_back(s);
                }
                else if (c == '}')
                {
                    if (!brace_stack.empty())
                    {
                        const std::size_t open = brace_stack.back();
                        brace_stack.pop_back();
                        // Don't clobber paren links (a sig is only one of those).
                        if (sigs[open].match == kNoPos)
                        {
                            sigs[open].match = s;
                        }

                        if (sig.match == kNoPos)
                        {
                            sig.match = open;
                        }
                    }
                }
            }
        }
        auto original_gap_had_space = [&](std::size_t prev, std::size_t cur) -> bool {
            const std::size_t gap_start =
                tokens[sigs[prev].token].offset + tokens[sigs[prev].token].length;
            const std::size_t gap_end = tokens[sigs[cur].token].offset;
            for (auto i = gap_start; i < gap_end; ++i)
            {
                if (source[i] == ' ' || source[i] == '\t')
                {
                    return true;
                }
            }

            return false;
        };

        // ---- Chunking: split code lines at block boundaries -------------------
        // A chunk is a same-line token run rendered and indented as one line.
        std::vector<Chunk> chunks;
        {
            std::size_t sig_cursor = 0;
            for (std::size_t line = 0; line < line_count; ++line)
            {
                const auto [content_start, content_end] = line_range(line);
                std::size_t probe                       = content_start;
                while (probe < content_end && IsIndent(source[probe]))
                {
                    ++probe;
                }

                const bool  blank = probe == content_end;
                std::size_t begin = sig_cursor;
                while (begin < sigs.size() && sigs[begin].line < line)
                {
                    ++begin;
                }

                std::size_t end = begin;
                while (end < sigs.size() && sigs[end].line == line)
                {
                    ++end;
                }

                sig_cursor = end;
                if (is_directive_line[line])
                {
                    Chunk chunk;
                    chunk.source_line = line;
                    chunk.directive   = true;
                    chunks.push_back(chunk);
                    continue;
                }

                if (line_spanned[line] != 0)
                {
                    // Crossed by a multi-line token (raw string, block comment):
                    // copy verbatim so literals never change. Rendering such a
                    // token inline would splice its embedded newlines into the
                    // output and duplicate the spanned lines.
                    Chunk chunk;
                    chunk.source_line = line;
                    chunk.verbatim    = true;
                    chunks.push_back(chunk);
                    continue;
                }

                if (blank || begin == end)
                {
                    if (!blank)
                    {
                        // Comment-only line (normalize spacing). Multi-line
                        // interiors were already routed to verbatim above.
                        Chunk chunk;
                        chunk.begin       = begin;
                        chunk.end         = end;
                        chunk.source_line = line;
                        chunk.verbatim    = false;
                        chunks.push_back(chunk);
                    }

                    continue; // blank lines handled by the blank-run budget below
                }

                // Split points inside [begin, end).
                std::size_t piece = begin;
                auto        emit  = [&](std::size_t piece_end) {
                    if (piece_end > piece)
                    {
                        Chunk chunk;
                        chunk.begin       = piece;
                        chunk.end         = piece_end;
                        chunk.source_line = line;
                        chunks.push_back(chunk);
                    }

                    piece = piece_end;
                };
                for (std::size_t i = begin; i < end; ++i)
                {
                    const std::string_view text  = sigs[i].text;
                    const bool             last  = i + 1 == end;
                    const bool             first = i == piece;
                    const std::string_view next  = last ? std::string_view() : sigs[i + 1].text;
                    if (text == "{" && sigs[i].kind == TokenKind::Punctuation)
                    {
                        // Block-open `{` (function body with any declarator
                        // suffix, named scope, control/label/bare block) vs.
                        // initializer/expression brace: decided by a backward
                        // scan, see IsBlockOpenBrace.
                        const bool block_open = IsBlockOpenBrace(sigs, i);
                        // Allman puts the brace on its own line; independently,
                        // a `{` with code after it (`int g(){return 1;}`) splits
                        // after `{` too (unless only closers/`;` follow on the
                        // line, e.g. `if (x) {}`). Both splits can fire for the
                        // same brace.
                        // An empty `{}` never moves to its own line.
                        const bool empty_pair = i + 1 < end && sigs[i + 1].text == "}";
                        if (m_options.brace_style == BraceStyle::Allman && block_open && !first &&
                            !empty_pair)
                        {
                            emit(i); // `{` starts its own line
                            piece = i;
                        }

                        if (block_open && i + 1 < end && !empty_pair)
                        {
                            bool only_tail = true;
                            for (auto k = i + 1; k < end; ++k)
                            {
                                const std::string_view tail = sigs[k].text;
                                if (tail != "}" && tail != ";" && tail != ",")
                                {
                                    only_tail = false;
                                }
                            }

                            if (!only_tail)
                            {
                                emit(i + 1);
                            }
                        }
                    }
                    else if (text == "}" && sigs[i].kind == TokenKind::Punctuation)
                    {
                        // Code before a block-closing `}` moves it to its own
                        // line, including consecutive block closes (`}}`). Empty
                        // blocks (`{}`) and initializer closes stay together. The
                        // match link tells block closes apart from initializers.
                        const std::size_t open = sigs[i].match;
                        const bool        closes_block =
                            open == kNoPos || open >= sigs.size() || IsBlockOpenBrace(sigs, open);
                        const std::string_view prev = first ? std::string_view() : sigs[i - 1].text;
                        if (!first && prev != "{" && closes_block)
                        {
                            emit(i);
                        } // `}` starts a line

                        // Break after `}` unless attached: `};`, `},`, `} else`,
                        // `} catch`, `} while` (do-while), `})`, `}.`, `}:`, and
                        // `} Name;` (typedef members, anonymous instances).
                        // Allman additionally detaches `else`/`catch`.
                        const TokenKind next_kind = last ? TokenKind::Unknown : sigs[i + 1].kind;
                        const bool      attached =
                            next == ";" || next == "," || next == "else" || next == "catch" ||
                            next == "while" || next == ")" || next == "]" || next == "}" ||
                            next == "." || next == ":" || next == "->" ||
                            next_kind == TokenKind::Identifier;
                        const bool detach =
                            !attached || (m_options.brace_style == BraceStyle::Allman &&
                                          (next == "else" || next == "catch"));
                        if (!last && detach && closes_block)
                        {
                            emit(i + 1);
                        }
                    }
                    else if (text == ";" && sigs[i].kind == TokenKind::Punctuation)
                    {
                        // One statement per line, but never inside `(...)` (for
                        // headers) or `[...]`; `;` before `}` stays attached.
                        if (!last && next != "}" && sigs[i].paren_depth == 0 &&
                            sigs[i].bracket_depth == 0)
                        {
                            emit(i + 1);
                        }
                    }
                }

                emit(end);
                // Label colon split (`case 1: foo();`, `public: int x;`), except
                // when a block opens right after the colon (`case 2: {` stays
                // together for the label-brace tracking below).
                if (chunks.size() >= 1)
                {
                    Chunk& first_chunk = chunks.back();
                    // Find the first chunk of this line (brace splits above may
                    // have produced several; only the first can hold the label).
                    std::size_t first_of_line = chunks.size() - 1;
                    while (first_of_line > 0 && chunks[first_of_line - 1].source_line == line &&
                           !chunks[first_of_line - 1].directive)
                    {
                        --first_of_line;
                    }

                    Chunk& head = chunks[first_of_line];
                    (void) first_chunk;
                    if (!head.directive && head.begin != head.end &&
                        ComputeIsLabel(sigs, head.begin, head.end))
                    {
                        for (std::size_t k = head.end; k > head.begin; --k)
                        {
                            if (sigs[k - 1].text == ":" &&
                                sigs[k - 1].kind == TokenKind::Punctuation && k < head.end &&
                                sigs[k].text != "{")
                            {
                                Chunk tail;
                                tail.begin       = k;
                                tail.end         = head.end;
                                tail.source_line = line;
                                head.end         = k;
                                chunks.insert(
                                    chunks.begin() + static_cast<std::ptrdiff_t>(first_of_line) + 1,
                                    tail);
                                break;
                            }
                        }
                    }
                }
            }
        }
        // Per-chunk context: labels, closers, continuation.
        const Chunk* prev_code_chunk = nullptr;
        for (auto& chunk : chunks)
        {
            if (chunk.directive || chunk.verbatim || chunk.begin == chunk.end)
            {
                continue;
            }

            // Allman splits `case 2: {` into `case 2:` + `{`; that `{` sits at the
            // case level like the attached form (body one deeper, `}` back).
            if (prev_code_chunk && prev_code_chunk->is_label && sigs[chunk.begin].text == "{" &&
                sigs[chunk.begin].kind == TokenKind::Punctuation &&
                sigs[prev_code_chunk->end - 1].text == ":")
            {
                chunk.label_brace    = true;
                chunk.label_open_sig = chunk.begin;
            }

            prev_code_chunk  = &chunk;
            chunk.is_label   = ComputeIsLabel(sigs, chunk.begin, chunk.end);
            bool all_closers = true;
            for (auto i = chunk.begin; i < chunk.end; ++i)
            {
                if (!IsCloserish(sigs[i].text))
                {
                    all_closers = false;
                }
            }

            chunk.closer_only       = all_closers;
            std::size_t eff_paren   = sigs[chunk.begin].paren_depth;
            std::size_t eff_bracket = sigs[chunk.begin].bracket_depth;
            if (sigs[chunk.begin].text == ")")
            {
                eff_paren = eff_paren > 0 ? eff_paren - 1 : 0;
            }

            if (sigs[chunk.begin].text == "]")
            {
                eff_bracket = eff_bracket > 0 ? eff_bracket - 1 : 0;
            }

            chunk.starts_inside = (eff_paren > 0 || eff_bracket > 0) && !all_closers;
            // `case 2: {` on one line: the `{` belongs to the case level and must
            // not push the body deeper (tracked as a depth stack, labels nest).
            if (chunk.is_label)
            {
                std::size_t last_colon = static_cast<std::size_t>(-1);
                for (auto i = chunk.begin; i < chunk.end; ++i)
                {
                    if (sigs[i].text == ":" && sigs[i].kind == TokenKind::Punctuation)
                    {
                        last_colon = i;
                    }
                }

                if (last_colon != static_cast<std::size_t>(-1))
                {
                    int balance = 0;
                    for (auto i = last_colon + 1; i < chunk.end; ++i)
                    {
                        if (sigs[i].kind != TokenKind::Punctuation || sigs[i].text.size() != 1)
                        {
                            continue;
                        }

                        if (sigs[i].text[0] == '{')
                        {
                            if (balance == 0 &&
                                chunk.label_open_sig == static_cast<std::size_t>(-1))
                            {
                                chunk.label_open_sig = i;
                            }

                            ++balance;
                        }
                        else if (sigs[i].text[0] == '}')
                        {
                            --balance;
                        }
                    }

                    if (balance == 0)
                    {
                        chunk.label_open_sig = static_cast<std::size_t>(-1);
                    }
                }
            }
        }

        {
            // prev_continues from the nearest previous chunk carrying code. A
            // trailing `:` continues only for ternaries, not for scope labels
            // (whose body already sits one level deeper via the label dedent).
            // Dangling control headers (`if (x)`, `else`, `do` with the body
            // starting on a later chunk) indent following chunks by a cumulative
            // dangle_bonus (nesting stacks; a body consumes the nest to base).
            bool pending       = false;
            bool prev_label    = false;
            bool prev_dangling = false;
            int  dangle_level  = 0;
            for (auto& chunk : chunks)
            {
                if (chunk.directive)
                {
                    pending       = false; // directives break continuation chains
                    prev_label    = false;
                    prev_dangling = false;
                    dangle_level  = 0;
                    continue;
                }

                if (chunk.verbatim || chunk.begin == chunk.end)
                {
                    continue;
                } // neutral

                chunk.prev_continues        = pending;
                chunk.dangle_bonus          = prev_dangling ? dangle_level : 0;
                const std::string_view last = sigs[chunk.end - 1].text;
                pending =
                    IsContinuationEnd(last) && !(last == "," && IsEntryComma(sigs, chunk.end - 1));
                if (last == ":" && prev_label)
                {
                    pending = false;
                }

                prev_label                 = chunk.is_label;
                const bool was_dangling    = prev_dangling;
                prev_dangling              = false;
                chunk.ends_dangling        = false;
                const std::size_t last_idx = chunk.end - 1;
                if (last == ")" && sigs[last_idx].match != kNoPos)
                {
                    const std::size_t open = sigs[last_idx].match;
                    if (open > 0 && sigs[open - 1].kind == TokenKind::Identifier &&
                        IsControlKeyword(sigs[open - 1].text))
                    {
                        chunk.ends_dangling = true;
                        prev_dangling       = true;
                    }
                }
                else if ((last == "else" || last == "do") &&
                         sigs[last_idx].kind == TokenKind::Identifier)
                {
                    chunk.ends_dangling = true;
                    prev_dangling       = true;
                }

                if (chunk.ends_dangling)
                {
                    ++dangle_level;
                }
                else if (was_dangling)
                {
                    dangle_level = 0;
                }
            }
        }

        // ---- Render chunks with spacing ---------------------------------------
        // Comments ride along with their chunk: leading block comments precede
        // the first token, a trailing `//` comment closes the line (and feeds the
        // alignment pass). Whitespace between tokens is recomputed from scratch.
        std::vector<std::vector<std::size_t>> line_comments(line_count);
        for (std::size_t t = 0; t < tokens.size(); ++t)
        {
            if (!IsComment(tokens[t].kind))
            {
                continue;
            }

            std::size_t line = 0;
            {
                std::size_t lo = 0, hi = line_starts.size();
                while (lo + 1 < hi)
                {
                    const std::size_t mid = (lo + hi) / kMidpointDivisor;
                    if (line_starts[mid] <= tokens[t].offset)
                    {
                        lo = mid;
                    }
                    else
                    {
                        hi = mid;
                    }
                }

                line = lo;
            }
            if (line < line_count)
            {
                line_comments[line].push_back(t);
            }
        }

        struct Rendered
        {
            std::string text;
            int         trailing_comment_col = -1; // byte offset of `//` in text, or -1
        };

        auto normalize_comment = [](std::string piece, bool is_line_comment) -> std::string {
            if (is_line_comment && piece.size() > kCommentMarkerLength &&
                piece[kCommentMarkerLength] != ' ' && piece[kCommentMarkerLength] != '\t' &&
                piece[kCommentMarkerLength] != '/' && piece[kCommentMarkerLength] != '!' &&
                piece[kCommentMarkerLength] != '<')
            {
                piece.insert(kCommentMarkerLength, " ");
            } // `//c` -> `// c`

            return piece;
        };
        auto render_chunk = [&](const Chunk& chunk, std::size_t chunk_pos) -> Rendered {
            Rendered out;
            if (chunk.directive || chunk.verbatim)
            {
                const auto [start, end] = line_range(chunk.source_line);
                out.text                = std::string(source.substr(start, end - start));
                if (!chunk.verbatim)
                {
                    // Trim trailing spaces/tabs (P0: trailing whitespace).
                    while (!out.text.empty() && (out.text.back() == ' ' || out.text.back() == '\t'))
                    {
                        out.text.pop_back();
                    }
                }

                return out;
            }

            // Comment span owned by this chunk: from the previous chunk's last
            // token (or line content start) to the next chunk's first token (or
            // content end). Chunks of one line partition its sig range in order.
            const auto [content_start, content_end] = line_range(chunk.source_line);
            std::size_t span_start                  = content_start;
            std::size_t span_end                    = content_end;
            if (chunk.begin != chunk.end)
            {
                if (chunk_pos > 0)
                {
                    const Chunk& prev_chunk = chunks[chunk_pos - 1];
                    if (prev_chunk.source_line == chunk.source_line &&
                        prev_chunk.end > prev_chunk.begin && !prev_chunk.directive)
                    {
                        const Token& prev_tok = tokens[sigs[prev_chunk.end - 1].token];
                        span_start            = prev_tok.offset + prev_tok.length;
                    }
                }

                if (chunk_pos + 1 < chunks.size())
                {
                    const Chunk& next_chunk = chunks[chunk_pos + 1];
                    if (next_chunk.source_line == chunk.source_line &&
                        next_chunk.end > next_chunk.begin && !next_chunk.directive)
                    {
                        span_end = tokens[sigs[next_chunk.begin].token].offset;
                    }
                }
            }

            std::vector<std::size_t> leading;
            std::vector<std::size_t> trailing;
            if (chunk.begin != chunk.end)
            {
                const std::size_t first_off = tokens[sigs[chunk.begin].token].offset;
                const Token&      last_tok  = tokens[sigs[chunk.end - 1].token];
                const std::size_t last_end  = last_tok.offset + last_tok.length;
                for (const auto t : line_comments[chunk.source_line])
                {
                    if (tokens[t].offset < first_off && tokens[t].offset >= span_start)
                    {
                        leading.push_back(t);
                    }
                    else if (tokens[t].offset >= last_end && tokens[t].offset < span_end)
                    {
                        trailing.push_back(t);
                    }
                }
            }
            else
            {
                // Comment-only line: every comment of the line belongs to it.
                for (const auto t : line_comments[chunk.source_line])
                {
                    if (tokens[t].offset >= span_start && tokens[t].offset < span_end)
                    {
                        leading.push_back(t);
                    }
                }
            }

            std::string text;
            auto        emit_comment = [&](std::size_t t, bool first_piece) {
                const bool  is_line = tokens[t].kind == TokenKind::LineComment;
                std::string piece =
                    normalize_comment(std::string(TokenText(source, tokens[t])), is_line);
                if (!first_piece)
                {
                    text += ' ';
                }

                text += piece;
                return is_line;
            };
            bool first_piece = true;
            for (const auto t : leading)
            {
                const bool is_line = emit_comment(t, first_piece);
                first_piece        = false;
                if (is_line)
                {
                    out.trailing_comment_col = 0;
                }
            }

            for (auto i = chunk.begin; i < chunk.end; ++i)
            {
                const Token& token = tokens[sigs[i].token];
                // Note: sigs never hold comments (see line_comments); the cases
                // below only handle code tokens.
                const std::string piece(TokenText(source, token));
                if (first_piece && i == chunk.begin && leading.empty())
                {
                    text += piece;
                    continue;
                }

                if (!leading.empty() && i == chunk.begin)
                {
                    // Gap after leading comment(s): space unless a closer follows.
                    const std::string_view sig_text = sigs[i].text;
                    if (sig_text != "," && sig_text != ";" && sig_text != ")" && sig_text != "]" &&
                        sig_text != "}" && sig_text != "." && sig_text != ":" && sig_text != "::")
                    {
                        text += ' ';
                    }

                    text += piece;
                    continue;
                }

                const int gap = SpacingGap(
                    sigs, i - 1, i, source, tokens, m_options.pointer_alignment,
                    m_options.reference_alignment, m_options.space_before_inheritance_colon,
                    m_options.space_after_c_style_cast, m_options.space_after_logical_not,
                    m_options.space_before_cpp11_braced_list);
                if (gap < 0)
                {
                    if (original_gap_had_space(i - 1, i))
                    {
                        text += ' ';
                    }
                }
                else if (gap > 0)
                {
                    text += ' ';
                }

                text += piece;
            }

            for (const auto t : trailing)
            {
                const bool  is_line = tokens[t].kind == TokenKind::LineComment;
                std::string piece =
                    normalize_comment(std::string(TokenText(source, tokens[t])), is_line);
                // Exactly one space before a trailing comment (P0 trims the rest).
                text += ' ';
                if (is_line)
                {
                    out.trailing_comment_col = static_cast<int>(text.size());
                }

                text += piece;
            }

            out.text = std::move(text);
            return out;
        };

        std::vector<Rendered> rendered;
        rendered.reserve(chunks.size());
        for (std::size_t r = 0; r < chunks.size(); ++r)
        {
            rendered.push_back(render_chunk(chunks[r], r));
        }

        // ---- Column-limit breaks (after spacing, before indent) ----------------
        // Only at commas: split off the tail after the chosen comma into a new
        // continuation chunk. Deterministic and idempotent: re-runs find segments
        // within the limit.
        if (m_options.column_limit > 0)
        {
            for (std::size_t r = 0; r < rendered.size(); ++r)
            {
                int guard = 0;
                while (rendered[r].text.size() > m_options.column_limit &&
                       guard++ < kColumnSplitMaxAttempts)
                {
                    if (chunks[r].directive || chunks[r].verbatim ||
                        chunks[r].begin == chunks[r].end)
                    {
                        break;
                    }

                    const std::size_t cbegin = chunks[r].begin;
                    const std::size_t cend   = chunks[r].end;
                    // Rendered x-offsets per sig token.
                    std::vector<std::size_t> x_end(cend - cbegin, 0);
                    {
                        std::size_t x = 0;
                        for (auto i = cbegin; i < cend; ++i)
                        {
                            if (i > cbegin)
                            {
                                const int gap = SpacingGap(
                                    sigs, i - 1, i, source, tokens, m_options.pointer_alignment,
                                    m_options.reference_alignment,
                                    m_options.space_before_inheritance_colon,
                                    m_options.space_after_c_style_cast,
                                    m_options.space_after_logical_not,
                                    m_options.space_before_cpp11_braced_list);
                                if (gap < 0)
                                {
                                    if (original_gap_had_space(i - 1, i))
                                    {
                                        x += 1;
                                    }
                                }
                                else if (gap > 0)
                                {
                                    x += 1;
                                }
                            }

                            x += TokenText(source, tokens[sigs[i].token]).size();
                            x_end[i - cbegin] = x;
                        }
                    }
                    // Best comma: minimal bracket depth; among those, the last one
                    // within the limit (else the first minimal one, for progress).
                    std::size_t min_depth = static_cast<std::size_t>(-1);
                    for (auto i = cbegin; i < cend; ++i)
                    {
                        if (sigs[i].text != "," || sigs[i].kind != TokenKind::Punctuation)
                        {
                            continue;
                        }

                        if (i + 1 >= cend)
                        {
                            continue;
                        } // trailing comma: no tail

                        min_depth =
                            std::min(min_depth, sigs[i].paren_depth + sigs[i].bracket_depth);
                    }

                    if (min_depth == static_cast<std::size_t>(-1))
                    {
                        break;
                    }

                    std::size_t best = static_cast<std::size_t>(-1);
                    for (auto i = cbegin; i < cend; ++i)
                    {
                        if (sigs[i].text != "," || sigs[i].kind != TokenKind::Punctuation)
                        {
                            continue;
                        }

                        if (i + 1 >= cend)
                        {
                            continue;
                        }

                        if (sigs[i].paren_depth + sigs[i].bracket_depth != min_depth)
                        {
                            continue;
                        }

                        if (x_end[i - cbegin] <= m_options.column_limit)
                        {
                            best = i; // later fitting commas supersede
                            continue;
                        }

                        if (best == static_cast<std::size_t>(-1))
                        {
                            best = i;
                        } // fallback

                        break; // further commas only run longer
                    }

                    if (best == static_cast<std::size_t>(-1))
                    {
                        break;
                    }

                    Chunk tail          = chunks[r];
                    tail.begin          = best + 1;
                    tail.force_continue = true;
                    tail.is_label       = false;
                    tail.closer_only    = false;
                    tail.starts_inside  = true;
                    tail.prev_continues = true;
                    chunks[r].end       = best + 1;
                    chunks.insert(chunks.begin() + static_cast<std::ptrdiff_t>(r) + 1, tail);
                    rendered[r] = render_chunk(chunks[r], r);
                    rendered.insert(rendered.begin() + static_cast<std::ptrdiff_t>(r) + 1,
                                    render_chunk(chunks[r + 1], r + 1));
                }
            }
        }

        // ---- Indent pass --------------------------------------------------------
        struct OutLine
        {
            std::string text;
            std::string ending; // "\n", "\r\n", "\r", or "" (no final newline)
            bool        blank                = false;
            bool        is_include           = false;
            int         trailing_comment_col = -1;
        };

        std::vector<OutLine> out_lines;
        {
            std::size_t              brace_depth = 0;
            std::vector<std::size_t> label_brace_depths;
            std::size_t              last_source_line = static_cast<std::size_t>(-1);
            // Set when the previous code chunk closed a control block; consumed by
            // the next chunk to decide on a separating blank line.
            bool after_control_close = false;
            bool after_type_close    = false; // `};` ending a class/struct/union/enum
            for (std::size_t r = 0; r < rendered.size(); ++r)
            {
                const Chunk& chunk = chunks[r];
                // Collapsed blank lines between this and the previous chunk.
                if (last_source_line != static_cast<std::size_t>(-1))
                {
                    std::size_t blanks = 0;
                    for (auto line = last_source_line + 1; line < chunk.source_line; ++line)
                    {
                        const auto [start, end] = line_range(line);
                        std::size_t probe       = start;
                        while (probe < end && IsIndent(source[probe]))
                        {
                            ++probe;
                        }

                        if (probe == end && !is_directive_line[line])
                        {
                            ++blanks;
                        }
                    }

                    std::size_t emit_blanks = std::min(blanks, m_options.max_empty_lines);
                    if (emit_blanks == 0 &&
                        ((after_control_close && m_options.blank_line_after_control_block) ||
                         (after_type_close && m_options.blank_line_after_type_definition)) &&
                        !chunk.directive && !chunk.verbatim && chunk.source_line > last_source_line)
                    {
                        // Not before a `}`, a continuation (`else`/`catch`) or a
                        // label, which belong with the block just closed.
                        const std::string_view head =
                            chunk.begin == chunk.end ? std::string_view("//")
                                                     : sigs[chunk.begin].text;
                        if (head != "}" && head != "else" && head != "catch" && !chunk.is_label)
                        {
                            emit_blanks = 1;
                        }
                    }

                    for (std::size_t b = 0; b < emit_blanks; ++b)
                    {
                        OutLine blank_line;
                        blank_line.blank  = true;
                        blank_line.ending = file_uses_crlf ? "\r\n" : "\n";
                        out_lines.push_back(std::move(blank_line));
                    }
                }

                last_source_line = chunk.source_line;
                if (chunk.directive || chunk.verbatim)
                {
                    after_control_close = false;
                    after_type_close    = false;
                }
                else if (chunk.begin != chunk.end)
                {
                    std::size_t tail = chunk.end;
                    while (tail > chunk.begin && IsComment(sigs[tail - 1].kind))
                    {
                        --tail;
                    }

                    after_control_close = false;
                    after_type_close    = false;
                    if (tail > chunk.begin)
                    {
                        if (sigs[tail - 1].text == ";" && tail - 1 > chunk.begin &&
                            sigs[tail - kTwoTokenOffset].text == "}")
                        {
                            after_type_close = IsTypeDefinitionClose(sigs, tail - kTwoTokenOffset);
                        }

                        if (sigs[tail - 1].text == "}")
                        {
                            after_control_close = IsControlBlockClose(sigs, tail - 1);
                        }
                        else if (sigs[tail - 1].text == ";" && sigs[chunk.begin].text == "}" &&
                                 chunk.begin + 1 < tail && sigs[chunk.begin + 1].text == "while")
                        {
                            // `} while (x);` ending a do-while.
                            const std::size_t open = sigs[chunk.begin].match;
                            after_control_close =
                                open != kNoSig && open > 0 && sigs[open - 1].text == "do";
                        }
                    }
                }
                else
                {
                    after_control_close = false; // comment-only line
                    after_type_close    = false;
                }

                if (chunk.directive || chunk.verbatim)
                {
                    OutLine line;
                    line.text   = rendered[r].text;
                    line.ending = std::string(line_ending(chunk.source_line));
                    if (chunk.directive)
                    {
                        std::string_view view(line.text);
                        std::size_t      pos = 0;
                        while (pos < view.size() && IsIndent(view[pos]))
                        {
                            ++pos;
                        }

                        if (pos < view.size() && view[pos] == '#')
                        {
                            ++pos;
                            while (pos < view.size() && IsIndent(view[pos]))
                            {
                                ++pos;
                            }

                            if (view.substr(pos, kIncludeKeywordLength) == "include" &&
                                (pos + kIncludeKeywordLength == view.size() ||
                                 view[pos + kIncludeKeywordLength] == ' ' ||
                                 view[pos + kIncludeKeywordLength] == '\t' ||
                                 view[pos + kIncludeKeywordLength] == '<' ||
                                 view[pos + kIncludeKeywordLength] == '"'))
                            {
                                line.is_include = true;
                            }
                        }
                    }

                    out_lines.push_back(std::move(line));
                    continue;
                }

                if (chunk.begin == chunk.end)
                {
                    // Comment-only line: indent like code at this depth.
                    std::string text;
                    if (m_options.use_tabs)
                    {
                        text.append(brace_depth, '\t');
                    }
                    else
                    {
                        text.append(brace_depth * m_options.indent_width, ' ');
                    }

                    text += rendered[r].text;
                    OutLine line;
                    line.text   = std::move(text);
                    line.ending = std::string(line_ending(chunk.source_line));
                    out_lines.push_back(std::move(line));
                    continue;
                }

                // Scope-aware indent with continuation support.
                std::size_t leading_closers = 0;
                for (auto i = chunk.begin; i < chunk.end; ++i)
                {
                    if (sigs[i].kind != TokenKind::Punctuation)
                    {
                        break;
                    }

                    if (sigs[i].text.size() != 1 || sigs[i].text[0] != '}')
                    {
                        break;
                    }

                    ++leading_closers;
                }

                const bool        is_scope_label = chunk.is_label && leading_closers == 0;
                const std::size_t dedent =
                    leading_closers + ((is_scope_label || chunk.label_brace) ? 1 : 0);
                const std::size_t indent_depth = brace_depth > dedent ? brace_depth - dedent : 0;
                // A dangling control header (`if (x)` / `else` / `do` with the
                // body on a later chunk) indents following chunks by the
                // cumulative dangle_bonus, unless the chunk opens with `{`
                // (normal block indent applies). Gated on single_line_style so
                // Keep mode preserves legacy behavior.
                const int dangle_bonus =
                    m_options.single_line_style != SingleLineStyle::Keep ? chunk.dangle_bonus : 0;
                const bool opens_with_brace =
                    chunk.begin != chunk.end && sigs[chunk.begin].text == "{";
                // An Allman `{` after a trailing operator-like token (`void f() &`)
                // is a block open, not a continuation of the previous line.
                const bool brace_after_suffix =
                    opens_with_brace && chunk.prev_continues && !chunk.starts_inside &&
                    !chunk.force_continue && IsBlockOpenBrace(sigs, chunk.begin);
                const bool continuation =
                    !chunk.closer_only && !brace_after_suffix &&
                    (chunk.starts_inside || chunk.prev_continues || chunk.force_continue);
                const std::size_t total_indent =
                    indent_depth + (continuation ? 1 : 0) +
                    (opens_with_brace ? 0 : static_cast<std::size_t>(dangle_bonus));
                std::string text;
                if (m_options.use_tabs)
                {
                    text.append(total_indent, '\t');
                }
                else
                {
                    text.append(total_indent * m_options.indent_width, ' ');
                }

                const int comment_col =
                    rendered[r].trailing_comment_col >= 0
                        ? static_cast<int>(text.size() + static_cast<std::size_t>(
                                                             rendered[r].trailing_comment_col))
                        : -1;
                text += rendered[r].text;
                OutLine line;
                line.text                 = std::move(text);
                line.ending               = std::string(line_ending(chunk.source_line));
                line.trailing_comment_col = comment_col;
                out_lines.push_back(std::move(line));

                for (auto i = chunk.begin; i < chunk.end; ++i)
                {
                    if (sigs[i].kind != TokenKind::Punctuation)
                    {
                        continue;
                    }

                    if (i == chunk.label_open_sig)
                    {
                        label_brace_depths.push_back(brace_depth);
                    }
                    else if (IsBrace(source, tokens[sigs[i].token], '{'))
                    {
                        ++brace_depth;
                    }
                    else if (IsBrace(source, tokens[sigs[i].token], '}') && brace_depth > 0)
                    {
                        if (!label_brace_depths.empty() && brace_depth == label_brace_depths.back())
                        {
                            label_brace_depths.pop_back();
                        }
                        else
                        {
                            --brace_depth;
                        }
                    }
                }
            }
        }

        if (m_options.align_consecutive_macros || m_options.align_consecutive_assignments)
        {
            auto alignment_column = [](std::string_view text,
                                       bool macro) -> std::optional<std::size_t> {
                if (text.empty() || text.back() == '\\')
                {
                    return std::nullopt;
                }

                const auto lexed = Lexer(text).Lex();
                std::vector<Token> significant;
                significant.reserve(lexed.size());
                for (const auto& token : lexed)
                {
                    if (token.kind != TokenKind::Whitespace &&
                        token.kind != TokenKind::LineComment &&
                        token.kind != TokenKind::BlockComment)
                    {
                        significant.push_back(token);
                    }
                }

                if (macro)
                {
                    if (significant.size() < 4 || significant[0].tok != Tok::Hash ||
                        TokenText(text, significant[1]) != "define" ||
                        significant[2].kind != TokenKind::Identifier)
                    {
                        return std::nullopt;
                    }

                    std::size_t nameEnd = 2;
                    if (significant[3].tok == Tok::LParen &&
                        significant[3].offset == significant[2].offset + significant[2].length)
                    {
                        std::size_t depth = 0;
                        for (auto i = 3; i < significant.size(); ++i)
                        {
                            if (significant[i].tok == Tok::LParen)
                            {
                                ++depth;
                            }
                            else if (significant[i].tok == Tok::RParen && --depth == 0)
                            {
                                nameEnd = i;
                                break;
                            }
                        }
                    }

                    const auto replacement = nameEnd + 1;
                    if (replacement >= significant.size() ||
                        significant[replacement].kind == TokenKind::LineComment ||
                        significant[replacement].offset <=
                            significant[nameEnd].offset + significant[nameEnd].length)
                    {
                        return std::nullopt;
                    }

                    return significant[replacement].offset;
                }

                if (significant.empty() || significant.back().tok != Tok::Semi)
                {
                    return std::nullopt;
                }

                std::size_t depth = 0;
                std::optional<std::size_t> column;
                for (const auto& token : significant)
                {
                    if (token.tok == Tok::LParen || token.tok == Tok::LBracket ||
                        token.tok == Tok::LBrace)
                    {
                        ++depth;
                    }
                    else if ((token.tok == Tok::RParen || token.tok == Tok::RBracket ||
                              token.tok == Tok::RBrace) && depth > 0)
                    {
                        --depth;
                    }
                    else if (token.tok == Tok::Eq && depth == 0)
                    {
                        if (column)
                        {
                            return std::nullopt;
                        }

                        column = token.offset;
                    }
                }

                return column;
            };

            auto align_runs = [&](bool macro) {
                std::size_t runStart = 0;
                while (runStart < out_lines.size())
                {
                    const auto first = out_lines[runStart].text.find_first_not_of(" \t");
                    const bool macroLine = first != std::string::npos &&
                                           out_lines[runStart].text[first] == '#';
                    if (out_lines[runStart].blank ||
                        out_lines[runStart].is_include ||
                        macro != macroLine ||
                        !alignment_column(out_lines[runStart].text, macro))
                    {
                        ++runStart;
                        continue;
                    }

                    const auto indentEnd = first;
                    const auto indent = out_lines[runStart].text.substr(0, indentEnd);
                    std::size_t runEnd = runStart;
                    std::size_t target = 0;
                    while (runEnd < out_lines.size() && !out_lines[runEnd].blank &&
                           !out_lines[runEnd].is_include &&
                           out_lines[runEnd].text.starts_with(indent) &&
                           out_lines[runEnd].text.find_first_not_of(" \t") == indentEnd &&
                           (macro == (out_lines[runEnd].text[indentEnd] == '#')))
                    {
                        const auto column = alignment_column(out_lines[runEnd].text, macro);
                        if (!column)
                        {
                            break;
                        }

                        target = std::max(target, *column);
                        ++runEnd;
                    }

                    if (runEnd - runStart > 1)
                    {
                        for (auto i = runStart; i < runEnd; ++i)
                        {
                            const auto column = *alignment_column(out_lines[i].text, macro);
                            const auto padding = target - column;
                            out_lines[i].text.insert(column, padding, ' ');
                            if (out_lines[i].trailing_comment_col >= 0 &&
                                static_cast<std::size_t>(out_lines[i].trailing_comment_col) >=
                                    column)
                            {
                                out_lines[i].trailing_comment_col += static_cast<int>(padding);
                            }
                        }
                    }

                    runStart = runEnd;
                }
            };

            if (m_options.align_consecutive_macros)
            {
                align_runs(true);
            }

            if (m_options.align_consecutive_assignments)
            {
                align_runs(false);
            }
        }

        // ---- Trailing `//` alignment -------------------------------------------
        if (m_options.align_trailing_comments)
        {
            std::size_t run_start = 0;
            bool        in_run    = false;
            auto        flush_run = [&](std::size_t start, std::size_t end) {
                if (end - start < kMinCommentRunLength)
                {
                    return;
                } // single lines keep one space

                std::size_t target = 0;
                for (auto i = start; i < end; ++i)
                {
                    target = std::max(target,
                                      static_cast<std::size_t>(out_lines[i].trailing_comment_col));
                }

                for (auto i = start; i < end; ++i)
                {
                    const std::size_t col =
                        static_cast<std::size_t>(out_lines[i].trailing_comment_col);
                    if (col < target)
                    {
                        out_lines[i].text.insert(col, target - col, ' ');
                        out_lines[i].trailing_comment_col = static_cast<int>(target);
                    }
                }
            };
            for (std::size_t i = 0; i <= out_lines.size(); ++i)
            {
                if (i < out_lines.size() && out_lines[i].trailing_comment_col >= 0 &&
                    !out_lines[i].blank)
                {
                    if (!in_run)
                    {
                        run_start = i;
                        in_run    = true;
                    }

                    continue;
                }

                if (in_run)
                {
                    flush_run(run_start, i);
                    in_run = false;
                }
            }
        }

        // ---- Include sorting ----------------------------------------------------
        if (m_options.sort_includes)
        {
            auto header_key = [](const std::string& line) -> std::string {
                const auto pos = line.find("include");
                if (pos == std::string::npos)
                {
                    return line;
                }

                std::string_view rest(line.c_str() + pos + kIncludeKeywordLength);
                while (!rest.empty() && IsIndent(rest.front()))
                {
                    rest.remove_prefix(1);
                }

                return std::string(rest);
            };
            std::size_t i = 0;
            while (i < out_lines.size())
            {
                if (!out_lines[i].is_include)
                {
                    ++i;
                    continue;
                }

                std::size_t j = i;
                while (j < out_lines.size() && out_lines[j].is_include)
                {
                    ++j;
                }

                std::stable_sort(out_lines.begin() + static_cast<std::ptrdiff_t>(i),
                                 out_lines.begin() + static_cast<std::ptrdiff_t>(j),
                                 [&](const OutLine& left, const OutLine& right) {
                                     return header_key(left.text) < header_key(right.text);
                                 });
                i = j;
            }
        }

        std::string output;
        for (const auto& line : out_lines)
        {
            if (line.blank)
            {
                output += line.ending;
                continue;
            }

            output += line.text;
            output += line.ending;
        }

        // Trim trailing blank lines: the file ends with at most its final
        // newline (a missing final newline is preserved as-is).
        std::size_t tail = output.size();
        while (tail > 0 && output[tail - 1] == '\n')
        {
            std::size_t blank_start = tail - 1;
            while (blank_start > 0 && output[blank_start - 1] != '\n')
            {
                --blank_start;
            }

            std::size_t blank_end = tail - 1;
            if (blank_end > blank_start && output[blank_end - 1] == '\r')
            {
                --blank_end;
            }

            if (blank_end != blank_start)
            {
                break;
            }

            tail = blank_start;
        }

        output.erase(tail);
        return output;
    }

    // ---- Range/diff API ---------------------------------------------------------

    namespace
    {

        struct SplitLine
        {
            std::size_t start = 0;
            std::string text; // includes the terminator, if any
        };

        std::vector<SplitLine> SplitLines(std::string_view source)
        {
            std::vector<SplitLine> lines;
            std::size_t            pos = 0;
            while (pos < source.size())
            {
                const std::size_t nl = source.find('\n', pos);
                if (nl == std::string_view::npos)
                {
                    lines.push_back({ pos, std::string(source.substr(pos)) });
                    break;
                }

                lines.push_back({ pos, std::string(source.substr(pos, nl - pos + 1)) });
                pos = nl + 1;
            }

            return lines;
        }

    } // namespace

    std::vector<FormatEdit> Formatter::FormatEdits(std::string_view source) const
    {
        const std::string formatted = Format(source);
        if (formatted == source)
            return {};
        const auto               src = SplitLines(source);
        const auto               dst = SplitLines(formatted);
        std::vector<std::size_t> src_hash(src.size());
        for (std::size_t i = 0; i < src.size(); ++i)
            src_hash[i] = std::hash<std::string> {}(src[i].text);
        std::vector<std::size_t> dst_hash(dst.size());
        for (std::size_t j = 0; j < dst.size(); ++j)
            dst_hash[j] = std::hash<std::string> {}(dst[j].text);
        // Greedy alignment with bounded resync: formatter output is mostly 1:1
        // with occasional splits/joins, so a full Myers diff is overkill.
        constexpr std::size_t kLookahead = 16;
        struct Hunk
        {
            std::size_t s = 0, e = 0, fs = 0, fe = 0;
        };

        std::vector<Hunk> hunks;
        std::size_t       i = 0, j = 0;
        auto              values_equal = [&](std::size_t a, std::size_t b) {
            return src_hash[a] == dst_hash[b] && src[a].text == dst[b].text;
        };
        while (i < src.size() || j < dst.size())
        {
            if (i < src.size() && j < dst.size() && values_equal(i, j))
            {
                ++i;
                ++j;
                continue;
            }

            // Resync: a source line matching ahead in formatted (insertion), or a
            // formatted line matching ahead in source (deletion).
            std::size_t ni = i, nj = j;
            bool        found = false;
            for (std::size_t d = 1; d <= kLookahead && !found; ++d)
            {
                if (i + d < src.size() && j < dst.size() && values_equal(i + d, j))
                {
                    ni    = i + d;
                    nj    = j;
                    found = true;
                }
                else if (j + d < dst.size() && i < src.size() && values_equal(i, j + d))
                {
                    ni    = i;
                    nj    = j + d;
                    found = true;
                }
            }

            if (!found)
            {
                // 1:1 replace step (or drain whichever side ran out).
                if (i < src.size())
                {
                    ++ni;
                }

                if (j < dst.size())
                {
                    ++nj;
                }
            }

            if (!hunks.empty() && hunks.back().e == i && hunks.back().fe == j)
            {
                hunks.back().e  = ni;
                hunks.back().fe = nj;
            }
            else
            {
                hunks.push_back({ i, ni, j, nj });
            }

            i = ni;
            j = nj;
        }

        std::vector<FormatEdit> edits;
        for (const auto& hunk : hunks)
        {
            FormatEdit edit;
            edit.start_line   = hunk.s;
            edit.end_line     = hunk.e;
            edit.start_offset = hunk.s < src.size() ? src[hunk.s].start : source.size();
            edit.end_offset   = hunk.e < src.size() ? src[hunk.e].start : source.size();
            for (auto k = hunk.fs; k < hunk.fe; ++k)
            {
                edit.replacement += dst[k].text;
            }

            edits.push_back(std::move(edit));
        }

        return edits;
    }

    std::string Formatter::FormatRange(std::string_view source, std::size_t start_line,
                                       std::size_t end_line) const
    {
        const auto edits = FormatEdits(source);
        if (edits.empty())
        {
            return std::string(source);
        }

        std::string output;
        std::size_t cursor = 0;
        for (const auto& edit : edits)
        {
            // Hunk lines [start_line, end_line) vs the inclusive end_line.
            if (edit.start_line > end_line || edit.end_line <= start_line)
            {
                continue;
            }

            output.append(source.substr(cursor, edit.start_offset - cursor));
            output += edit.replacement;
            cursor = edit.end_offset;
        }

        output.append(source.substr(cursor));
        return output;
    }

} // namespace heimdall
