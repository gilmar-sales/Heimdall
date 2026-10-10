#include "detail/GrammarParser.hpp"
#include <Heimdall/Lexer.hpp>
#include <Heimdall/ParseTree.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stop_token>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heimdall
{

    namespace
    {
        constexpr int kTwo          = 2;
        constexpr int kThree        = 3;
        constexpr int kFour         = 4;
        constexpr int kFive         = 5;
        constexpr int kSix          = 6;
        constexpr int kSeven        = 7;
        constexpr int kEight        = 8;
        constexpr int kNine         = 9;
        constexpr int kTen          = 10;
        constexpr int kEleven       = 11;
        constexpr int kTwelve       = 12;
        constexpr int kThirteen     = 13;
        constexpr int kFourteen     = 14;
        constexpr int kScratchBytes = 32768;

        // Predefined macros that only decorate a declaration (`_GLIBCXX_NOEXCEPT`,
        // `EXPORT`, `[[nodiscard]]` wrappers) or open/close a namespace
        // (`_GLIBCXX_BEGIN_NAMESPACE_CXX11` -> `namespace __cxx11 {`). The grammar
        // does not expand macros, so these identifiers would otherwise glue onto
        // the neighbouring declaration and hide its members. They are dropped from
        // the significant-token view; the tokens stay in the tree.
        bool IsDecorationMacro(std::string_view value)
        {
            const std::vector<Token>          tokens    = Lexer(value).Lex();
            static constexpr std::string_view allowed[] = {
                "noexcept",   "constexpr",     "consteval",     "constinit",     "inline",
                "const",      "volatile",      "static",        "explicit",      "extern",
                "__inline",   "__inline__",    "__attribute__", "__declspec",    "__extension__",
                "__restrict", "__restrict__",  "__const",       "__volatile__",  "alignas",
                "nodiscard",  "__nodiscard__", "deprecated",    "__deprecated__"
            };
            int  nest            = 0;
            bool opens_namespace = false;
            bool saw_brace       = false;
            for (const auto& token : tokens)
            {
                if (token.kind == TokenKind::Whitespace)
                {
                    continue;
                }

                const std::string_view text = value.substr(token.offset, token.length);
                if (token.kind == TokenKind::Punctuation)
                {
                    if (text == "(" || text == "[")
                    {
                        ++nest;
                    }
                    else if (text == ")" || text == "]")
                    {
                        --nest;
                    }
                    else if (nest == 0 && (text == "{" || text == "}"))
                    {
                        saw_brace = true;
                    }
                    else if (nest == 0 && text != "[[" && text != "]]")
                    {
                        return false;
                    }

                    continue;
                }

                if (nest > 0)
                {
                    continue;
                }

                if (token.kind != TokenKind::Identifier)
                {
                    return false;
                }

                if (text == "namespace")
                {
                    opens_namespace = true;
                    continue;
                }

                if (std::find(std::begin(allowed), std::end(allowed), text) == std::end(allowed) &&
                    !(opens_namespace && !saw_brace))
                {
                    return false;
                }
            }

            // Braces are only decoration when they belong to a namespace open/close.
            return !saw_brace || opens_namespace ||
                   value.find_first_not_of(" 	}") == std::string_view::npos;
        }

    } // namespace

    class GrammarParser
    {
      public:
        GrammarParser(ParseTree&                    tree,
                      const PreprocessorResult&     preprocessing,
                      std::stop_token               stop,
                      const Preprocessor::MacroMap* macros,
                      const ParseReuse*             reuse,
                      const TypeNameOracle*         type_names) :
            m_tree(tree), m_stop(std::move(stop)), m_reuse(reuse), m_oracle(type_names)
        {
            m_names_hash = type_names != nullptr ? type_names->Fingerprint() : 0;
            // identifier text -> "is a decoration macro", memoized per parse
            std::unordered_map<std::string_view, bool> decoration;
            for (const auto& diagnostic : preprocessing.diagnostics)
            {
                tree.m_diagnostics.push_back({ diagnostic.offset, diagnostic.message });
            }

            std::size_t active_cursor    = 0;
            std::size_t directive_cursor = 0;
            for (std::size_t i = 0; i < tree.Tokens().size(); ++i)
            {
                const auto& token = tree.Tokens()[i];
                while (active_cursor < preprocessing.active_ranges.size() &&
                       preprocessing.active_ranges[active_cursor].offset +
                               preprocessing.active_ranges[active_cursor].length <=
                           token.offset)
                {
                    ++active_cursor;
                }

                while (directive_cursor < preprocessing.directives.size() &&
                       preprocessing.directives[directive_cursor].offset +
                               preprocessing.directives[directive_cursor].length <=
                           token.offset)
                {
                    ++directive_cursor;
                }

                const bool active =
                    active_cursor < preprocessing.active_ranges.size() &&
                    preprocessing.active_ranges[active_cursor].offset <= token.offset;
                const bool directive =
                    directive_cursor < preprocessing.directives.size() &&
                    preprocessing.directives[directive_cursor].offset <= token.offset;
                if ((active || directive) && token.kind != TokenKind::Whitespace &&
                    token.kind != TokenKind::LineComment && token.kind != TokenKind::BlockComment)
                {
                    if (macros != nullptr &&
                        (!macros->empty() || !preprocessing.local_macros.empty()) && !directive &&
                        token.kind == TokenKind::Identifier)
                    {
                        const std::string_view word =
                            tree.m_source.substr(token.offset, token.length);
                        auto cached = decoration.find(word);
                        if (cached == decoration.end())
                        {
                            const auto local = preprocessing.local_macros.find(std::string(word));
                            const auto found = macros->find(word);
                            const bool is_decoration =
                                local != preprocessing.local_macros.end()
                                    ? IsDecorationMacro(local->second)
                                    : found != macros->end() && IsDecorationMacro(found->second);
                            cached = decoration.emplace(word, is_decoration).first;
                        }

                        if (cached->second)
                        {
                            if (tree.m_decoration.empty())
                            {
                                tree.m_decoration.assign(tree.Tokens().size(), false);
                            }

                            tree.m_decoration[i] = true;
                            continue;
                        }
                    }

                    m_sig.push_back(static_cast<std::uint32_t>(i));
                }
            }

            m_sig_tok.reserve(m_sig.size());
            for (const std::size_t token_index : m_sig)
            {
                m_sig_tok.push_back(tree.Tokens()[token_index].tok);
            }

            m_match.resize(m_sig.size(), Invalid);

            std::pmr::vector<std::uint32_t> stack(&m_scratch);
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(m_sig.size()); ++i)
            {
                const Tok t = m_sig_tok[i];
                if (t == Tok::LParen || t == Tok::LBracket || t == Tok::LBrace)
                {
                    stack.push_back(i);
                }
                else if (t == Tok::RParen || t == Tok::RBracket || t == Tok::RBrace)
                {
                    if (!stack.empty() && Closes(m_sig_tok[stack.back()], t))
                    {
                        m_match[i]            = stack.back();
                        m_match[stack.back()] = i;
                        stack.pop_back();
                    }
                    else if (!stack.empty())
                    {
                        std::size_t found = stack.size();
                        for (std::size_t k = stack.size(); k > 0; --k)
                        {
                            if (Closes(m_sig_tok[stack[k - 1]], t))
                            {
                                found = k - 1;
                                break;
                            }
                        }

                        if (found < stack.size())
                        {
                            for (std::size_t k = found + 1; k < stack.size(); ++k)
                            {
                                const std::uint32_t open_tok = m_sig[stack[k]];
                                tree.m_diagnostics.push_back(
                                    { tree.Tokens()[open_tok].offset, "unclosed delimiter" });
                            }

                            m_match[i]            = stack[found];
                            m_match[stack[found]] = i;
                            stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(found),
                                        stack.end());
                        }
                    }
                }
            }
        }

        void Run()
        {
            m_tree.m_nodes_soa.push_back(
                static_cast<std::uint8_t>(GrammarKind::TranslationUnit),
                0,
                static_cast<std::uint32_t>(m_tree.Tokens().size()),
                static_cast<std::uint32_t>(ParseTree::RootNode),
                1);
            m_tree.m_nodes_aos_dirty = true;
            ParseScope(0, m_sig.size(), ParseTree::RootNode, false);
        }

      private:
        static constexpr std::uint32_t Invalid = static_cast<std::uint32_t>(-1);

        alignas(alignof(std::max_align_t)) std::array<std::byte, kScratchBytes> m_scratch_buffer;
        std::pmr::monotonic_buffer_resource m_scratch { m_scratch_buffer.data(),
                                                        m_scratch_buffer.size() };
        ParseTree&                          m_tree;
        std::stop_token                     m_stop;
        std::pmr::vector<std::uint32_t>     m_sig { &m_scratch };

        std::pmr::vector<Tok>           m_sig_tok { &m_scratch };
        std::pmr::vector<std::uint32_t> m_match { &m_scratch };
        std::uint32_t                   m_last_expression_node = Invalid;
        const ParseReuse*               m_reuse                = nullptr;
        const TypeNameOracle*           m_oracle               = nullptr;
        // File-level names (flags above) and their order-independent hash, seeded
        // with the oracle's fingerprint; block scopes live on a stack.
        struct BlockName
        {
            std::string_view name;
            std::uint8_t     flags;
        };

        // Open-addressing table (power-of-two size, linear probing) over views into the
        // source: file-level names are looked up and added thousands of times in a header.
        struct NameSlot
        {
            std::string_view name;
            std::uint64_t    hash  = 0;
            std::uint8_t     flags = 0;
        };

        std::vector<NameSlot>    m_slots;
        std::size_t              m_slot_count = 0;
        std::uint64_t            m_names_hash = 0;
        std::vector<BlockName>   m_block_names;
        std::vector<std::size_t> m_block_starts;
        std::size_t              m_registered_upto = 0;
        std::uint64_t            m_item_names_hash = 0;
        // Top-level item currently being parsed (see FinishItem / TryReuseItem).
        bool        m_item_open       = false;
        std::size_t m_item_first_sig  = 0;
        std::size_t m_item_node_begin = 0;
        std::size_t m_item_diag_begin = 0;

        std::string_view Text(std::size_t sig) const
        {
            if (sig >= m_sig.size())
                return {};
            const auto& token = m_tree.Tokens()[m_sig[sig]];
            return m_tree.m_source.substr(token.offset, token.length);
        }

        static bool Closes(Tok open, Tok close)
        {
            return (open == Tok::LParen && close == Tok::RParen) ||
                   (open == Tok::LBracket && close == Tok::RBracket) ||
                   (open == Tok::LBrace && close == Tok::RBrace);
        }

        std::size_t Add(GrammarKind kind, std::size_t begin, std::size_t end, std::size_t parent)
        {
            const std::size_t first = begin < m_sig.size() ? m_sig[begin] : m_tree.Tokens().size();
            const std::size_t past =
                end > begin && end - 1 < m_sig.size() ? m_sig[end - 1] + 1 : first;
            const std::uint32_t index = static_cast<std::uint32_t>(m_tree.m_nodes_soa.size());
            m_tree.m_nodes_soa.push_back(
                static_cast<std::uint8_t>(kind),
                static_cast<std::uint32_t>(first),
                static_cast<std::uint32_t>(past - first),
                static_cast<std::uint32_t>(parent),
                index + 1);
            m_tree.m_nodes_aos_dirty = true; // Invalidate AoS cache
            return m_tree.m_nodes_soa.size() - 1;
        }

        // `Is(i, "template")`: the literal is resolved to a Tok at compile time, so
        // keywords and punctuators cost one byte comparison.
        bool Is(std::size_t i, TokLiteral literal) const
        {
            if (i >= m_sig_tok.size())
            {
                return false;
            }

            if (literal.tok != Tok::None)
            {
                return m_sig_tok[i] == literal.tok;
            }

            return Text(i) == literal.text;
        }

        // Runtime text (e.g. a table of operators): classify once, then compare.
        template <std::same_as<std::string_view> S>
        bool Is(std::size_t i, S text) const
        {
            if (i >= m_sig_tok.size())
            {
                return false;
            }

            const Tok tok = LookupTok(text);
            return tok != Tok::None ? m_sig_tok[i] == tok : Text(i) == text;
        }

        void SetNodeRange(std::size_t node, std::size_t begin, std::size_t end)
        {
            m_tree.m_nodes_soa.first_token[node] =
                begin < m_sig.size() ? m_sig[begin] : m_tree.Tokens().size();
            const auto past                      = end > begin && end - 1 < m_sig.size()
                                                       ? m_sig[end - 1] + 1
                                                       : m_tree.m_nodes_soa.first_token[node];
            m_tree.m_nodes_soa.token_count[node] = past - m_tree.m_nodes_soa.first_token[node];
            m_tree.m_nodes_aos_dirty             = true;
        }

        bool IsDirective(std::size_t sig) const
        {
            if (sig >= m_sig.size() || (!Is(sig, "#") && !Is(sig, "%:")))
            {
                return false;
            }

            const auto offset = m_tree.Tokens()[m_sig[sig]].offset;

            const auto nl =
                offset == 0 ? std::string_view::npos : m_tree.m_source.rfind('\n', offset - 1);
            const auto line_start = nl == std::string_view::npos ? 0 : nl + 1;
            for (auto i = line_start; i < offset; ++i)
            {
                if (m_tree.m_source[i] != ' ' && m_tree.m_source[i] != '\t' &&
                    m_tree.m_source[i] != '\r')
                {
                    return false;
                }
            }

            return true;
        }

        std::size_t SkipDirective(std::size_t sig, std::size_t end, std::size_t parent)
        {
            const auto start    = sig;
            const auto offset   = m_tree.Tokens()[m_sig[sig]].offset;
            auto       line_end = m_tree.m_source.find('\n', offset);
            if (line_end == std::string_view::npos)
            {
                line_end = m_tree.m_source.size();
            }
            else
            {
                ++line_end;
            }

            // A directive continues over lines ending in a backslash,
            // mirroring the preprocessor span (Preprocessor::Process):
            // otherwise the continuation lines leak into the parse as code.
            while (line_end < m_tree.m_source.size())
            {
                auto body_end = line_end;
                if (m_tree.m_source[body_end - 1] == '\n')
                {
                    --body_end;
                }

                if (body_end > 0 && m_tree.m_source[body_end - 1] == '\r')
                {
                    --body_end;
                }

                if (body_end == 0 || m_tree.m_source[body_end - 1] != '\\')
                {
                    break;
                }

                const auto next = m_tree.m_source.find('\n', line_end);
                line_end = next == std::string_view::npos ? m_tree.m_source.size() : next + 1;
            }

            while (sig < end && m_tree.Tokens()[m_sig[sig]].offset < line_end)
            {
                ++sig;
            }

            Add(GrammarKind::PreprocessorDirective, start, sig, parent);
            return sig;
        }

        std::size_t SkipGroup(std::size_t i, std::size_t end) const
        {
            if (i < end && m_match[i] != Invalid && m_match[i] > i)
            {
                return m_match[i] + 1;
            }

            return i + 1;
        }

        std::size_t FindComma(std::size_t begin, std::size_t end) const
        {
            std::size_t angle_depth = 0;
            for (auto i = begin; i < end;)
            {
                if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid &&
                    m_match[i] > i)
                {
                    i = m_match[i] + 1;
                    continue;
                }

                if (Is(i, "<") && i > begin &&
                    (m_tree.Tokens()[m_sig[i - 1]].kind == TokenKind::Identifier ||
                     Is(i - 1, ">") || Is(i - 1, ">>")))
                {
                    ++angle_depth;
                }
                else if (Is(i, ">") && angle_depth != 0)
                {
                    --angle_depth;
                }
                else if (Is(i, ">>") && angle_depth != 0)
                {
                    angle_depth = angle_depth > 1 ? angle_depth - kTwo : 0;
                }
                else if (Is(i, ",") && angle_depth == 0)
                {
                    return i;
                }

                ++i;
            }

            return end;
        }

        std::size_t FindTemplateClose(std::size_t open, std::size_t end) const
        {
            if (!Is(open, "<"))
            {
                return Invalid;
            }

            std::size_t depth = 1;
            for (auto i = open + 1; i < end;)
            {
                if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid &&
                    m_match[i] > i)
                {
                    i = m_match[i] + 1;
                    continue;
                }

                if (Is(i, "<"))
                {
                    ++depth;
                }
                else if (Is(i, ">"))
                {
                    if (--depth == 0)
                    {
                        return i;
                    }
                }
                else if (Is(i, ">>"))
                {
                    if (depth <= kTwo)
                    {
                        return i;
                    }

                    depth -= kTwo;
                }
                else if ((Is(i, ";") || Is(i, "=")) && depth == 1)
                {
                    return Invalid;
                }

                ++i;
            }

            return Invalid;
        }

        std::size_t FindSemicolon(std::size_t begin, std::size_t end) const
        {
            for (auto i = begin; i < end;)
            {
                if (Is(i, ";"))
                {
                    return i;
                }

                if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid &&
                    m_match[i] > i)
                {
                    i = m_match[i] + 1;
                }
                else
                {
                    ++i;
                }
            }

            return end;
        }

        std::size_t TopLevelAssignment(std::size_t begin, std::size_t end) const
        {
            for (auto i = begin; i < end;)
            {
                if (Is(i, "=") || Is(i, "{"))
                {
                    return i;
                }

                if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid &&
                    m_match[i] > i)
                {
                    i = m_match[i] + 1;
                }
                else
                {
                    ++i;
                }
            }

            return end;
        }

        bool IsIdentifierToken(std::size_t sig) const
        {
            return sig < m_sig.size() && m_tree.Tokens()[m_sig[sig]].kind == TokenKind::Identifier;
        }

        static bool IsBuiltinType(std::string_view text)
        {
            switch (text.size())
            {
                case kThree:
                    return text == "int";
                case kFour:
                    switch (text[0])
                    {
                        case 'v':
                            return text == "void";
                        case 'b':
                            return text == "bool";
                        case 'c':
                            return text == "char";
                        case 'l':
                            return text == "long";
                        case 'a':
                            return text == "auto";
                        default:
                            return false;
                    }
                case kFive:
                    switch (text[0])
                    {
                        case 's':
                            return text == "short";
                        case 'f':
                            return text == "float";
                        default:
                            return false;
                    }
                case kSix:
                    switch (text[0])
                    {
                        case 'd':
                            return text == "double";
                        case 's':
                            return text == "signed";
                        default:
                            return false;
                    }
                case kSeven:
                    switch (text[0])
                    {
                        case 'w':
                            return text == "wchar_t";
                        case 'c':
                            return text == "char8_t";
                        default:
                            return false;
                    }
                case kEight:
                    switch (text[0])
                    {
                        case 'u':
                            return text == "unsigned";
                        case 'c':
                            return text == "char16_t" || text == "char32_t";
                        case 'd':
                            return text == "decltype";
                        default:
                            return false;
                    }
                default:
                    return false;
            }
        }

        static bool IsDeclSpecifierKeyword(std::string_view text)
        {
            if (IsBuiltinType(text))
            {
                return true;
            }

            switch (text.size())
            {
                case kFour:
                    return text == "enum";
                case kFive:
                    switch (text[0])
                    {
                        case 'c':
                            return text == "const" || text == "class";
                        case 'u':
                            return text == "union";
                        default:
                            return false;
                    }
                case kSix:
                    switch (text[0])
                    {
                        case 's':
                            return text == "static" || text == "struct";
                        case 'e':
                            return text == "extern";
                        case 'i':
                            return text == "inline";
                        case 'f':
                            return text == "friend";
                        default:
                            return false;
                    }
                case kSeven:
                    switch (text[0])
                    {
                        case 'v':
                            return text == "virtual";
                        case 't':
                            return text == "typedef";
                        case 'm':
                            return text == "mutable";
                        default:
                            return false;
                    }
                case kEight:
                    switch (text[0])
                    {
                        case 'v':
                            return text == "volatile";
                        case 'e':
                            return text == "explicit";
                        case 't':
                            return text == "typename";
                        default:
                            return false;
                    }
                case kNine:
                    switch (text[0])
                    {
                        case 'c':
                            return text == "constexpr" || text == "consteval" ||
                                   text == "constinit";
                        default:
                            return false;
                    }
                case kTwelve:
                    return text == "thread_local";
                default:
                    return false;
            }
        }

        static bool IsDeclarationStartKeyword(std::string_view text)
        {
            return IsDeclSpecifierKeyword(text) || text == "::" || text == "requires";
        }

        bool IsAttributeStart(std::size_t i, std::size_t end) const
        {
            return i + 1 < end && Is(i, "[") && Is(i + 1, "[");
        }

        // `alignas(expr)` / `alignas(type)` is an attribute-like specifier: it
        // may appear anywhere `[[...]]` may, so it is skipped the same way.
        bool IsAlignasStart(std::size_t i, std::size_t end) const
        {
            return i + 1 < end && Is(i, "alignas") && Is(i + 1, "(") && m_match[i + 1] != Invalid &&
                   m_match[i + 1] < end;
        }

        std::size_t SkipAttributes(std::size_t i, std::size_t end, std::size_t parent)
        {
            while (IsAttributeStart(i, end) || IsAlignasStart(i, end))
            {
                const bool alignas_spec = Is(i, "alignas");
                const auto outer_close  = alignas_spec ? m_match[i + 1] : m_match[i];
                if (outer_close == Invalid || outer_close >= end)
                {
                    break;
                }

                Add(GrammarKind::AttributeSpecifier, i, outer_close + 1, parent);
                i = outer_close + 1;
            }

            return i;
        }

        bool IsRequiresExpressionAt(std::size_t pos, std::size_t end) const
        {
            if (!Is(pos, "requires") || pos >= end)
            {
                return false;
            }

            auto next = pos + 1;
            if (next < end && Is(next, "(") && m_match[next] != Invalid && m_match[next] < end)
            {
                const auto after = m_match[next] + 1;
                if (after < end && Is(after, "{"))
                {
                    return true;
                }

                if (after == end)
                {
                    return true;
                }

                return false;
            }

            if (next < end && Is(next, "{"))
            {
                return true;
            }

            return false;
        }

        std::size_t FindTopLevelRequires(std::size_t begin, std::size_t end) const
        {
            std::unordered_map<std::size_t, std::size_t> close_cache;
            auto template_close = [&](std::size_t open) -> std::size_t {
                if (const auto hit = close_cache.find(open); hit != close_cache.end())
                {
                    return hit->second;
                }

                const std::size_t close = FindTemplateClose(open, end);
                close_cache.emplace(open, close);
                return close;
            };
            std::size_t angle_depth = 0;
            for (auto i = begin; i < end;)
            {
                if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid &&
                    m_match[i] > i)
                {
                    i = m_match[i] + 1;
                    continue;
                }

                if (Is(i, "<") && i > begin &&
                    (IsIdentifierToken(i - 1) || Is(i - 1, ">") || Is(i - 1, ">>")))
                {
                    const auto close = template_close(i);
                    if (close != Invalid)
                    {
                        ++angle_depth;
                        ++i;
                        continue;
                    }
                }

                if (Is(i, ">") && angle_depth != 0)
                {
                    --angle_depth;
                    ++i;
                    continue;
                }

                if (Is(i, ">>") && angle_depth != 0)
                {
                    angle_depth = angle_depth > 1 ? angle_depth - kTwo : 0;
                    ++i;
                    continue;
                }

                if (angle_depth == 0 && Is(i, "requires") && !IsRequiresExpressionAt(i, end))
                {
                    return i;
                }

                ++i;
            }

            return Invalid;
        }

        // Parse `::`? (identifier [<...>] `::`)* and return position of unqualified-id.
        // Emits NestedNameSpecifier nodes for each `prefix ::` segment.
        std::size_t ParseNestedNamePrefix(std::size_t begin, std::size_t end, std::size_t parent)
        {
            auto i = begin;
            if (i < end && Is(i, "::"))
            {
                ++i;
            }

            while (i < end)
            {
                auto head = i;
                if (Is(head, "template"))
                {
                    ++head;
                }

                if (head >= end || !IsIdentifierToken(head))
                {
                    break;
                }

                auto after_name = head + 1;
                if (after_name < end && Is(after_name, "<"))
                {
                    const auto close = FindTemplateClose(after_name, end);
                    if (close == Invalid)
                    {
                        break;
                    }

                    after_name = close + 1;
                }

                if (after_name + 1 < end && Is(after_name, ":") && Is(after_name + 1, ":"))
                {
                    break;
                }

                if (after_name < end && Is(after_name, "::"))
                {
                    Add(GrammarKind::NestedNameSpecifier, i, after_name + 1, parent);
                    i = after_name + 1;
                    continue;
                }

                break;
            }

            return i;
        }

        std::size_t ParseQualifiedName(
            std::size_t begin, std::size_t end, std::size_t parent, std::size_t& name_pos)
        {
            name_pos = Invalid;
            auto i   = ParseNestedNamePrefix(begin, end, parent);
            if (i < end && Is(i, "::"))
            {
                ++i;
            }

            if (i < end && Is(i, "template"))
            {
                ++i;
            }

            if (i < end && Is(i, "~"))
            {
                if (i + 1 < end && IsIdentifierToken(i + 1))
                {
                    name_pos = i + 1;
                    return i + kTwo;
                }

                return i + 1;
            }

            if (i < end && Is(i, "operator"))
            {
                auto op = i + 1;
                if (op < end && Is(op, "(") && m_match[op] != Invalid)
                {
                    op = m_match[op] + 1;
                }
                else if (op < end && Is(op, "[") && m_match[op] != Invalid)
                {
                    op = m_match[op] + 1;
                }
                else
                {
                    ++op;
                }

                if (op < end && Is(op, "<"))
                {
                    const auto close = FindTemplateClose(op, end);
                    if (close != Invalid)
                    {
                        op = close + 1;
                    }
                }

                name_pos = i;
                return op <= end ? op : end;
            }

            if (i < end && IsIdentifierToken(i))
            {
                name_pos   = i;
                auto after = i + 1;
                if (after < end && Is(after, "<"))
                {
                    const auto close = FindTemplateClose(after, end);
                    if (close != Invalid)
                    {
                        after = close + 1;
                    }
                }

                return after;
            }

            return i;
        }

        // Consume decl-specifier-seq starting at begin. Returns position after specifiers.
        // Creates one TypeSpecifier node plus NestedNameSpecifier / AttributeSpecifier children.
        std::size_t ParseDeclSpecifiers(std::size_t begin, std::size_t end, std::size_t parent)
        {
            auto       i          = SkipAttributes(begin, end, parent);
            const auto spec_start = i;
            bool       saw_type   = false;
            while (i < end)
            {
                i = SkipAttributes(i, end, parent);
                if (i >= end)
                {
                    break;
                }

                const auto text = Text(i);
                if (text == "decltype" && i + 1 < end && Is(i + 1, "(") &&
                    m_match[i + 1] != Invalid)
                {
                    i        = m_match[i + 1] + 1;
                    saw_type = true;
                    continue;
                }

                if (text == "struct" || text == "class" || text == "union" || text == "enum")
                {
                    ++i;
                    i                 = SkipAttributes(i, end, parent);
                    std::size_t dummy = Invalid;
                    i                 = ParseQualifiedName(i, end, parent, dummy);
                    saw_type          = true;

                    // Inline definition `struct S [final] [: Base] { ... } name(args);`:
                    // the body belongs to the type specifier, not to the declarator.
                    auto body = i;
                    if (body < end && Is(body, "final"))
                    {
                        ++body;
                    }

                    if (body < end && Is(body, ":"))
                    {
                        while (body < end && !Is(body, "{") && !Is(body, ";"))
                        {
                            ++body;
                        }
                    }

                    if (body < end && Is(body, "{") && m_match[body] != Invalid &&
                        m_match[body] < end)
                    {
                        i = m_match[body] + 1;
                    }

                    continue;
                }

                if (IsDeclSpecifierKeyword(text))
                {
                    if (text == "explicit" && i + 1 < end && Is(i + 1, "(") &&
                        m_match[i + 1] != Invalid)
                    {
                        i = m_match[i + 1] + 1;
                        continue;
                    }

                    // `signed long`, `unsigned long long`, `long double` etc. all stay in
                    // specifiers.
                    ++i;
                    saw_type |=
                        IsBuiltinType(text) || text == "struct" || text == "class" ||
                        text == "union" || text == "enum" || text == "typename" || text == "auto";
                    // A user type name directly after `typename` belongs to the specifiers.
                    if ((text == "typename" || text == "const" || text == "volatile") && i < end)
                    {
                        auto probe = ParseNestedNamePrefix(i, end, parent);
                        if (probe < end && IsIdentifierToken(probe))
                        {
                            auto after = probe + 1;
                            if (after < end && Is(after, "<"))
                            {
                                const auto close = FindTemplateClose(after, end);
                                if (close != Invalid)
                                {
                                    after = close + 1;
                                }
                            }

                            // Only absorb it as the core type if nothing follows that looks like
                            // a second declarator name (e.g. `Widget input`, not `int` alone).
                            auto look = after;
                            while (look < end && (Is(look, "*") || Is(look, "&") || Is(look, "&&")))
                            {
                                ++look;
                            }

                            if (look < end && IsIdentifierToken(look))
                            {
                                i        = after;
                                saw_type = true;
                            }
                        }
                    }

                    continue;
                }

                // User-defined type: nested-name-specifier? identifier [<...>] (but not the
                // declarator name). Only absorb a type when none has been seen yet; otherwise the
                // identifier is the declarator name (e.g. `left` in `int left`).
                if ((IsIdentifierToken(i) || Is(i, "::")) && !saw_type)
                {
                    const auto  save  = i;
                    std::size_t dummy = Invalid;
                    const auto  after = ParseQualifiedName(i, end, parent, dummy);
                    if (after == save)
                    {
                        break;
                    }

                    // If what follows looks like a declarator name/operator, this was the type.
                    auto look = after;
                    while (look < end && (Is(look, "*") || Is(look, "&") || Is(look, "&&")))
                    {
                        ++look;
                    }

                    // A name immediately followed by its parameter list is a
                    // constructor declarator, not a user-defined return type.
                    // A parenthesized pointer/reference declarator still needs
                    // the preceding name as its type: `Widget (*factory)(int)`.
                    const bool grouped_declarator =
                        Is(look, "(") && look + 1 < end &&
                        (Is(look + 1, "*") || Is(look + 1, "&") || Is(look + 1, "&&") ||
                         Is(look + 1, "("));
                    if (look < end && (IsIdentifierToken(look) || grouped_declarator ||
                                       Is(look, "~") || Is(look, "operator") || Is(look, "::")))
                    {
                        i        = after;
                        saw_type = true;
                        break;
                    }

                    // `const A::B *` (unnamed parameter): once a cv-qualifier/specifier has
                    // been consumed, a name followed only by pointer/reference operators
                    // is the type; leaving it as a declarator name would strand `A::`.
                    if (spec_start < save &&
                        (look >= end || Is(look, ",") || Is(look, "=") || Is(look, ")")))
                    {
                        i        = after;
                        saw_type = true;
                        break;
                    }

                    // `A f` where f is missing (e.g. end of parameter) still counts as type,
                    // but a lone `name{...}` / `name[...]` is a declarator with init/suffix.
                    if (after == end ||
                        (after < end && (Is(after, ",") || Is(after, "=") || Is(after, ";") ||
                                         Is(after, ")") || Is(after, "requires"))))
                    {
                        i        = after;
                        saw_type = true;
                        break;
                    }

                    break;
                }

                break;
            }

            if (saw_type && spec_start < i)
            {
                Add(GrammarKind::TypeSpecifier, spec_start, i, parent);
            }

            // Qualifiers such as explicit/constexpr decorate a constructor even
            // though it has no return type. Leave its name as the declarator.
            return i;
        }

        bool LooksLikeDeclarationStart(std::size_t pos, std::size_t end) const
        {
            if (pos >= end)
            {
                return false;
            }

            const auto text = Text(pos);
            if (text == "::" || text == "template")
            {
                return true;
            }

            if (IsDeclSpecifierKeyword(text))
            {
                return true;
            }

            if (!IsIdentifierToken(pos))
            {
                return false;
            }

            auto after = pos + 1;
            if (after < end && Is(after, "<"))
            {
                const auto close = FindTemplateClose(after, end);
                if (close == Invalid)
                {
                    return false;
                }

                after = close + 1;
            }

            if (after < end && Is(after, "::"))
            {
                return true;
            }

            // `Name * x`, `Name & x`, `Name x`
            auto look = after;
            while (look < end && (Is(look, "*") || Is(look, "&") || Is(look, "&&")))
            {
                ++look;
            }

            if (look < end && IsIdentifierToken(look))
            {
                return true;
            }

            if (look < end && Is(look, "("))
            {
                return true;
            }

            return false;
        }

        // Extent of one constraint expression starting at pos (no tree mutation).
        // Mirrors the expression grammar's primary/postfix/binary structure so a
        // leading `requires Constraint decl` splits after the constraint, not inside it.
        std::size_t ConstraintPrimaryExtent(std::size_t pos, std::size_t end) const
        {
            if (pos >= end)
            {
                return pos;
            }

            if (Is(pos, "(") && m_match[pos] != Invalid && m_match[pos] < end)
            {
                return m_match[pos] + 1;
            }

            if (Is(pos, "[") && m_match[pos] != Invalid && m_match[pos] < end)
            {
                return m_match[pos] + 1;
            }

            if (Is(pos, "{") && m_match[pos] != Invalid && m_match[pos] < end)
            {
                return m_match[pos] + 1;
            }

            if (Is(pos, "requires"))
            {
                auto body = pos + 1;
                if (body < end && Is(body, "(") && m_match[body] != Invalid)
                {
                    body = m_match[body] + 1;
                }

                while (body < end && !Is(body, "{") && !Is(body, ";"))
                {
                    ++body;
                }

                if (body < end && Is(body, "{") && m_match[body] != Invalid)
                {
                    return m_match[body] + 1;
                }

                return body;
            }

            auto i = pos;
            if (i < end && Is(i, "::"))
            {
                ++i;
            }

            if (i < end && Is(i, "template"))
            {
                ++i;
            }

            if (i < end && Is(i, "~"))
            {
                if (i + 1 < end && IsIdentifierToken(i + 1))
                {
                    return i + kTwo;
                }

                return i + 1;
            }

            if (i < end && Is(i, "operator"))
            {
                auto op = i + 1;
                if (op < end && (Is(op, "(") || Is(op, "[")) && m_match[op] != Invalid)
                {
                    op = m_match[op] + 1;
                }
                else
                {
                    ++op;
                }

                return op <= end ? op : end;
            }

            if (i < end &&
                (IsIdentifierToken(i) || Is(i, "decltype") || Is(i, "sizeof") || Is(i, "alignof") ||
                 Is(i, "noexcept") || Is(i, "true") || Is(i, "false") || Is(i, "nullptr") ||
                 m_tree.Tokens()[m_sig[i]].kind == TokenKind::Number ||
                 m_tree.Tokens()[m_sig[i]].kind == TokenKind::StringLiteral ||
                 m_tree.Tokens()[m_sig[i]].kind == TokenKind::CharacterLiteral ||
                 m_tree.Tokens()[m_sig[i]].kind == TokenKind::RawStringLiteral))
            {
                ++i;
                if (i < end && Is(i, "<"))
                {
                    const auto close = FindTemplateClose(i - 1, end);
                    // Only treat `<...>` as template args when it closes; otherwise it is a
                    // relational operator terminating the constraint primary.
                    if (close != Invalid)
                    {
                        i = close + 1;
                    }
                }

                while (i + 1 < end && Is(i, "::") &&
                       (IsIdentifierToken(i + 1) || Is(i + 1, "template")))
                {
                    ++i;
                    if (Is(i, "template"))
                    {
                        ++i;
                    }

                    if (i < end && IsIdentifierToken(i))
                    {
                        ++i;
                        if (i < end && Is(i, "<"))
                        {
                            const auto close = FindTemplateClose(i - 1, end);
                            if (close == Invalid)
                            {
                                break;
                            }

                            i = close + 1;
                        }
                    }
                }

                return i;
            }

            return pos;
        }

        std::size_t ConstraintExtent(std::size_t pos, std::size_t end) const
        {
            auto i = ConstraintPrimaryExtent(pos, end);
            if (i == pos)
            {
                return pos;
            }

            while (i < end)
            {
                // Postfix continuation: calls, subscripts, member access, template-ids.
                if ((Is(i, "(") || Is(i, "[")) && m_match[i] != Invalid && m_match[i] < end)
                {
                    i = m_match[i] + 1;
                    continue;
                }

                if (Is(i, ".") || Is(i, "->") || Is(i, "::"))
                {
                    ++i;
                    if (i < end && (IsIdentifierToken(i) || Is(i, "template")))
                    {
                        ++i;
                    }

                    continue;
                }

                if ((Is(i, "++") || Is(i, "--")) && i > pos)
                {
                    ++i;
                    continue;
                }

                if (Is(i, "<") && i > pos &&
                    (IsIdentifierToken(i - 1) || Is(i - 1, ">") || Is(i - 1, ">>")))
                {
                    const auto close = FindTemplateClose(i - 1, end);
                    if (close != Invalid && close + 1 < end &&
                        (Is(close + 1, "(") || Is(close + 1, "{") || Is(close + 1, "::") ||
                         Is(close + 1, ",") || Is(close + 1, ">") || Is(close + 1, ">>") ||
                         IsIdentifierToken(close + 1)))
                    {
                        i = close + 1;
                        continue;
                    }
                }

                // Ternary.
                if (Is(i, "?"))
                {
                    ++i;
                    const auto mid = ConstraintExtent(i, end);
                    i              = mid;
                    if (i < end && Is(i, ":"))
                    {
                        ++i;
                    }

                    const auto rhs = ConstraintExtent(i, end);
                    i              = rhs;
                    continue;
                }

                static constexpr std::string_view binary_ops[] = {
                    "||", "&&", "|", "^", "&", "==", "!=", "<=", ">=", "<=>",
                    "<<", ">>", "<", ">", "+", "-",  "*",  "/",  "%"
                };
                bool is_binary = false;
                for (const auto op : binary_ops)
                {
                    if (!Is(i, op))
                    {
                        continue;
                    }

                    if ((op == "*" || op == "&") && i + 1 < end && IsIdentifierToken(i + 1))
                    {
                        // `*` / `&` followed by a name at top level more likely starts a
                        // declarator (`void use`) than continues the constraint.
                        // Only treat as binary when the left side cannot end a constraint
                        // primary... handled below by requiring a valid right-hand primary.
                    }

                    const auto rhs = ConstraintPrimaryExtent(i + 1, end);
                    if (rhs == i + 1)
                    {
                        is_binary = false;
                        break;
                    }

                    // For `*`/`&`, demand that the right side continues as an expression
                    // (operator, `(`, `,`, `;`, end) rather than a lone declarator name
                    // followed by `(` params or `;`.
                    if ((op == "*" || op == "&") && rhs < end && IsIdentifierToken(rhs - 1))
                    {
                        auto look = rhs;
                        while (look < end && (Is(look, "*") || Is(look, "&") || Is(look, "&&")))
                        {
                            ++look;
                        }

                        if (look < end &&
                            (Is(look, "(") || Is(look, ";") || Is(look, ",") || Is(look, "=")))
                        {
                            // Ambiguous pointer-vs-binary: prefer declaration split unless the
                            // `(`/`;` clearly belongs to the constraint (e.g. call args).
                            // Calls were already consumed as postfix groups above, so stop here.
                            is_binary = false;
                            break;
                        }
                    }

                    i         = rhs;
                    is_binary = true;
                    break;
                }

                if (is_binary)
                {
                    // Allow chained postfix on the right-hand side result.
                    continue;
                }

                break;
            }

            return i;
        }

        // For `requires Constraint decl`, find where the constraint ends and the declaration
        // begins. Parses the constraint extent without mutating the tree, then verifies the
        // remainder looks like a declaration.
        std::size_t FindLeadingRequiresSplit(std::size_t req, std::size_t end) const
        {
            if (req == Invalid || req >= end)
            {
                return Invalid;
            }

            const auto extent = ConstraintExtent(req + 1, end);
            if (extent > req + 1 && extent < end && LooksLikeDeclarationStart(extent, end))
            {
                return extent;
            }

            return Invalid;
        }

        std::size_t FindDeclaredName(std::size_t begin, std::size_t end) const
        {
            std::size_t candidate = Invalid;
            for (auto i = begin; i < end;)
            {
                if (Is(i, "<") && i > begin && IsIdentifierToken(i - 1))
                {
                    const auto close = FindTemplateClose(i, end);
                    if (close != Invalid)
                    {
                        i = close + 1;
                        continue;
                    }
                }

                if (Is(i, "(") && m_match[i] != Invalid && m_match[i] > i)
                {
                    bool pointer_group = false;
                    for (auto j = i + 1; j < m_match[i]; ++j)
                    {
                        pointer_group |= Is(j, "*") || Is(j, "&") || Is(j, "&&");
                    }

                    if (pointer_group)
                    {
                        for (auto j = i + 1; j < m_match[i]; ++j)
                        {
                            if (IsIdentifierToken(j) && m_match[j] == Invalid)
                            {
                                candidate = j;
                            }
                        }
                    }

                    i = m_match[i] + 1;
                    continue;
                }

                if ((Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid && m_match[i] > i)
                {
                    i = m_match[i] + 1;
                    continue;
                }

                if (IsIdentifierToken(i))
                {
                    candidate = i;
                }

                ++i;
            }

            return candidate;
        }

        void ParseRequiresConstraint(std::size_t req, std::size_t clause_end, std::size_t parent)
        {
            if (req == Invalid || req + 1 >= clause_end)
            {
                return;
            }

            const auto clause = Add(GrammarKind::RequiresClause, req, clause_end, parent);
            ParseExpression(req + 1, clause_end, clause);
        }

        std::size_t FindBitfieldColon(std::size_t begin, std::size_t end) const
        {
            std::size_t angle_depth = 0;
            for (auto i = begin; i < end; ++i)
            {
                if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid &&
                    m_match[i] > i)
                {
                    i = m_match[i];
                    continue;
                }

                if (Is(i, "<") && i > begin && IsIdentifierToken(i - 1))
                {
                    const auto close = FindTemplateClose(i, end);
                    if (close != Invalid)
                    {
                        ++angle_depth;
                        continue;
                    }
                }

                if ((Is(i, ">") || Is(i, ">>")) && angle_depth != 0)
                {
                    continue;
                }

                if (Is(i, ":") && !(i + 1 < end && Is(i + 1, ":")))
                {
                    return i;
                }

                if (Is(i, ";") || Is(i, ",") || Is(i, "="))
                {
                    return Invalid;
                }
            }

            return Invalid;
        }

        // Parse `( params ) [cv/ref/noexcept/attrs] [-> type] [requires ...]` at pos==open.
        // Returns position after the suffix. Creates FunctionSuffix + children.
        std::size_t ParseFunctionSuffix(std::size_t open, std::size_t end,
                                        std::size_t declarator_node)
        {
            if (open >= end || !Is(open, "(") || m_match[open] == Invalid)
            {
                return open;
            }

            const auto close = m_match[open];
            if (close >= end)
            {
                return open;
            }

            const auto suffix = Add(GrammarKind::FunctionSuffix, open, close + 1, declarator_node);
            auto       part   = open + 1;
            if (part == close && open > 0)
            {
                // `()` empty parameter list: no ParameterDeclaration children.
            }

            while (part < close)
            {
                const auto comma    = FindComma(part, close);
                const bool ellipsis = comma == part + 1 && Is(part, "...");
                if (!ellipsis && comma > part)
                {
                    // Skip `void` alone in `()` / `(void)`.
                    const bool lone_void = comma == close && part + 1 == comma && Is(part, "void");
                    if (!lone_void || close - (open + 1) != 1)
                    {
                        if (!(lone_void && part == open + 1))
                        {
                            const auto parameter =
                                Add(GrammarKind::ParameterDeclaration, part, comma, suffix);
                            const auto consumed = AddTypeAndDeclarator(part, comma, parameter);
                            // A parameter that stops early while the next one clearly begins
                            // means the separating comma was forgotten.
                            if (consumed > part && consumed < comma &&
                                LooksLikeDeclarationStart(consumed, comma))
                            {
                                m_tree.m_diagnostics.push_back(
                                    { m_tree.Tokens()[m_sig[consumed]].offset,
                                      std::string("expected ',' before '") +
                                          std::string(Text(consumed)) + "'" });
                                SetNodeRange(parameter, part, consumed);
                                part = consumed;
                                continue;
                            }
                        }
                    }
                }

                if (comma == close)
                {
                    break;
                }

                part = comma + 1;
            }

            auto tail = close + 1;
            tail      = SkipAttributes(tail, end, suffix);
            while (tail < end && (Is(tail, "const") || Is(tail, "volatile") || Is(tail, "&") ||
                                  Is(tail, "&&") || Is(tail, "override") || Is(tail, "final")))
            {
                ++tail;
            }

            if (tail < end && Is(tail, "noexcept"))
            {
                auto spec_end = tail + 1;
                if (spec_end < end && Is(spec_end, "(") && m_match[spec_end] != Invalid)
                {
                    spec_end = m_match[spec_end] + 1;
                }

                Add(GrammarKind::NoexceptSpecifier, tail, spec_end, suffix);
                tail = spec_end;
                tail = SkipAttributes(tail, end, suffix);
            }
            else if (tail < end && Is(tail, "throw") && tail + 1 < end && Is(tail + 1, "(") &&
                     m_match[tail + 1] != Invalid)
            {
                const auto spec_end = m_match[tail + 1] + 1;
                Add(GrammarKind::NoexceptSpecifier, tail, spec_end, suffix);
                tail = spec_end;
            }

            if (tail < end && Is(tail, "->"))
            {
                auto type_end = tail + 1;
                while (type_end < end &&
                       (Is(type_end, "*") || Is(type_end, "&") || Is(type_end, "&&") ||
                        Is(type_end, "const") || Is(type_end, "volatile")))
                {
                    ++type_end;
                }

                std::size_t dummy = Invalid;
                // Reuse qualified-name scanning for the trailing type core.
                const auto name_start = type_end;
                type_end              = ParseQualifiedName(type_end, end, suffix, dummy);
                if (type_end == name_start)
                {
                    ++type_end;
                }

                Add(GrammarKind::TrailingReturnType, tail, type_end, suffix);
                tail = type_end;
            }

            // Trailing requires-clause belongs to the suffix when present here.
            const auto req = FindTopLevelRequires(tail, end);
            if (req != Invalid)
            {
                ParseRequiresConstraint(req, end, suffix);
                SetNodeRange(suffix, open, end);
                return end;
            }

            SetNodeRange(suffix, open, tail);
            return tail;
        }

        // Parse one declarator (no top-level comma) with full pointer / core / suffix grammar.
        // Creates Declarator node with DeclaredName, PointerOperator, ArraySuffix, FunctionSuffix,
        // etc.
        std::size_t ParseSingleDeclarator(std::size_t begin, std::size_t end, std::size_t parent)
        {
            if (begin >= end)
            {
                return begin;
            }

            const auto declarator = Add(GrammarKind::Declarator, begin, end, parent);
            auto       i          = SkipAttributes(begin, end, declarator);
            // Leading pointer operators: `*`, `&`, `&&` with cv-qualifiers and attributes.
            while (i < end && (Is(i, "*") || Is(i, "&") || Is(i, "&&")))
            {
                const auto start = i++;
                while (i < end && (Is(i, "const") || Is(i, "volatile")))
                {
                    ++i;
                }

                i = SkipAttributes(i, end, declarator);
                Add(GrammarKind::PointerOperator, start, i, declarator);
                i = SkipAttributes(i, end, declarator);
            }

            std::size_t name_pos = Invalid;
            std::size_t core_end = i;
            if (i < end && Is(i, "(") && m_match[i] != Invalid && m_match[i] < end)
            {
                // Parenthesized declarator: `(*fp)`, `(&ref)`, `(name)` for function
                // pointers/references.
                const auto inner_open  = i;
                const auto inner_close = m_match[i];
                // Emit inner pointer operators and name recursively.
                ParseSingleDeclarator(inner_open + 1, inner_close, declarator);
                core_end = inner_close + 1;
                // The outer Declarator keeps the full range; inner Declarator holds the name.
            }
            else
            {
                core_end = ParseQualifiedName(i, end, declarator, name_pos);
                if (name_pos == Invalid && core_end < end &&
                    (Is(core_end, "*") || Is(core_end, "&") || Is(core_end, "&&")))
                {
                    // Pointer-to-member `Nested::*name` / `Nested::&name`: the nested-name
                    // prefix was emitted above; consume the member pointer operator here.
                    const auto start = core_end++;
                    while (core_end < end && (Is(core_end, "const") || Is(core_end, "volatile")))
                    {
                        ++core_end;
                    }

                    core_end = SkipAttributes(core_end, end, declarator);
                    Add(GrammarKind::PointerOperator, start, core_end, declarator);
                    core_end = ParseQualifiedName(core_end, end, declarator, name_pos);
                }

                if (name_pos != Invalid)
                {
                    Add(GrammarKind::DeclaredName, name_pos, name_pos + 1, declarator);
                }
                else if (core_end == i)
                {
                    // Abstract declarator (e.g. lone `void` parameter or `...`): nothing more to
                    // parse.
                    SetNodeRange(declarator, begin, core_end);
                    return core_end;
                }
            }

            auto tail = core_end;
            // Suffix loop: arrays, function parameter lists, trailing return, noexcept, attributes,
            // bitfield.
            while (tail < end)
            {
                tail = SkipAttributes(tail, end, declarator);
                if (tail >= end)
                {
                    break;
                }

                if (Is(tail, "[") && m_match[tail] != Invalid && m_match[tail] < end)
                {
                    const auto close  = m_match[tail];
                    const auto suffix = Add(GrammarKind::ArraySuffix, tail, close + 1, declarator);
                    if (close > tail + 1)
                    {
                        ParseExpression(tail + 1, close, suffix);
                    }

                    tail = close + 1;
                    continue;
                }

                if (Is(tail, "(") && m_match[tail] != Invalid && m_match[tail] < end)
                {
                    tail = ParseFunctionSuffix(tail, end, declarator);
                    continue;
                }

                if (Is(tail, "->"))
                {
                    auto        type_end = tail + 1;
                    std::size_t dummy    = Invalid;
                    type_end             = ParseQualifiedName(type_end, end, declarator, dummy);
                    Add(GrammarKind::TrailingReturnType, tail, type_end, declarator);
                    tail = type_end;
                    continue;
                }

                if (Is(tail, "noexcept") ||
                    (Is(tail, "throw") && tail + 1 < end && Is(tail + 1, "(")))
                {
                    auto spec_end = tail + 1;
                    if (spec_end < end && Is(spec_end, "(") && m_match[spec_end] != Invalid)
                    {
                        spec_end = m_match[spec_end] + 1;
                    }

                    Add(GrammarKind::NoexceptSpecifier, tail, spec_end, declarator);
                    tail = spec_end;
                    continue;
                }

                break;
            }

            // Bitfield `: width` (member declarations only, but harmless to recognize generally).
            const auto colon = FindBitfieldColon(tail, end);
            if (colon != Invalid && colon >= tail)
            {
                const auto suffix = Add(GrammarKind::BitfieldSuffix, colon, end, declarator);
                ParseExpression(colon + 1, end, suffix);
                tail = end;
            }

            SetNodeRange(declarator, begin, tail);
            return tail;
        }

        std::size_t AddTypeAndDeclarator(
            std::size_t begin, std::size_t end, std::size_t parent, bool include_type = true)
        {
            if (begin >= end)
            {
                if (include_type)
                {
                    Add(GrammarKind::TypeSpecifier, begin, end, parent);
                }

                return begin;
            }

            // Ellipsis parameter `...` has no type or name.
            if (end == begin + 1 && Is(begin, "..."))
            {
                return end;
            }

            // Trailing requires-clause: split `decl requires Constraint` at top level.
            auto        declarator_end = end;
            std::size_t trailing_req   = Invalid;
            const auto  req_pos        = FindTopLevelRequires(begin, end);
            if (req_pos != Invalid && !IsRequiresExpressionAt(req_pos, end))
            {
                // Distinguish initializer `= requires...` (constraint as initializer) from a
                // trailing clause: if `=` precedes requires at top level, it is an initializer.
                bool has_equals_before = false;
                for (auto k = begin; k < req_pos;)
                {
                    if ((Is(k, "(") || Is(k, "[") || Is(k, "{")) && m_match[k] != Invalid &&
                        m_match[k] > k)
                    {
                        k = m_match[k] + 1;
                        continue;
                    }

                    if (Is(k, "="))
                    {
                        has_equals_before = true;
                        break;
                    }

                    ++k;
                }

                if (!has_equals_before)
                {
                    trailing_req   = req_pos;
                    declarator_end = req_pos;
                }
            }

            // Initializer split: top-level `=` or braced/paren init that is not a function suffix.
            auto init_pos = TopLevelAssignment(begin, declarator_end);
            // A top-level `(` group here is ambiguous between direct-initialization and a
            // function declarator suffix; ParseSingleDeclarator disambiguates via FunctionSuffix.
            // Only treat `(` as initializer when no FunctionSuffix will claim it: check whether
            // the range before `(` already contains a declarator name.
            if (init_pos >= declarator_end)
            {
                for (auto k = begin; k < declarator_end;)
                {
                    if ((Is(k, "[") || Is(k, "{")) && m_match[k] != Invalid && m_match[k] > k)
                    {
                        k = m_match[k] + 1;
                        continue;
                    }

                    if (Is(k, "(") && m_match[k] != Invalid && m_match[k] > k)
                    {
                        init_pos = k;
                        break;
                    }

                    ++k;
                }

                // If the paren group parses as a function suffix, it is not an initializer.
                if (init_pos < declarator_end && Is(init_pos, "("))
                {
                    const auto maybe_name = FindDeclaredName(begin, init_pos);
                    if (maybe_name != Invalid)
                    {
                        init_pos = declarator_end;
                    }
                }
                else
                {
                    init_pos = declarator_end;
                }
            }

            auto code_end = init_pos < declarator_end ? init_pos : declarator_end;
            // Bitfield width is part of the declarator, not the initializer.
            const auto colon = FindBitfieldColon(begin, code_end);
            if (colon != Invalid)
            {
                code_end = end >= colon ? code_end : code_end;
            }

            // Shared vs. per-declarator type handling: when include_type is false the caller
            // already emitted the shared TypeSpecifier; otherwise parse specifiers here.
            std::size_t spec_end = begin;
            if (include_type)
            {
                // Parse specifiers over the full declarator range so a trailing lone
                // identifier is not mistaken for a type when an initializer follows
                // (e.g. `second` in `second{right}`); clamp back to code_end.
                spec_end = ParseDeclSpecifiers(begin, declarator_end, parent);
                if (spec_end > code_end)
                {
                    spec_end = code_end;
                }

                if (spec_end == begin)
                {
                    // No type specifiers (e.g. constructor `Widget();`): parse bare declarator.
                    spec_end = begin;
                }
            }
            else
            {
                // Skip the shared type prefix already emitted by AddDeclarationDetails.
                // Re-parse to find its extent without emitting a duplicate TypeSpecifier.
                auto probe = begin;
                // Temporarily parse with emission suppressed by using a scratch parent?
                // Instead compute extent via a non-emitting scan: reuse ParseDeclSpecifiers logic
                // by parsing into the InitDeclarator then removing? Simpler: find first name.
                const auto first_name = FindDeclaredName(begin, code_end);
                if (first_name != Invalid)
                {
                    // Walk back over pointer operators to find declarator start.
                    spec_end = first_name;
                    // The shared TypeSpecifier ends before pointer operators; keep them in
                    // declarator. Find where pointer run starts by scanning from begin for `*`/`&`
                    // after type. Heuristic: spec_end for suffix parsing is begin of pointer run or
                    // name.
                    auto scan = begin;
                    // Consume attributes + decl-specifier keywords + user type to locate split.
                    // Reuse ParseDeclSpecifiers extent by probing with a temporary node then
                    // rolling back.
                    const auto nodes_before = m_tree.m_nodes_soa.size();
                    const auto probe_end    = ParseDeclSpecifiers(begin, code_end, parent);
                    // Remove the duplicate TypeSpecifier + children emitted by the probe.
                    while (m_tree.m_nodes_soa.size() > nodes_before)
                    {
                        m_tree.m_nodes_soa.kind.pop_back();
                        m_tree.m_nodes_soa.first_token.pop_back();
                        m_tree.m_nodes_soa.token_count.pop_back();
                        m_tree.m_nodes_soa.parent.pop_back();
                        m_tree.m_nodes_soa.subtree_end.pop_back();
                    }

                    m_tree.m_nodes_aos_dirty = true;

                    spec_end = probe_end;
                    if (spec_end <= begin)
                    {
                        spec_end = begin;
                    }

                    (void) scan;
                }
                else
                {
                    spec_end = begin;
                }
            }

            // Track how far parsing actually advanced so callers can detect leftovers
            // such as a declaration that lost its terminating semicolon.
            std::size_t consumed = spec_end > begin ? spec_end : begin;
            if (spec_end < code_end)
            {
                consumed = ParseSingleDeclarator(spec_end, code_end, parent);
            }
            else if (spec_end == begin && code_end > begin)
            {
                // Fallback: retain old name-only behavior so counts never regress.
                const auto name = FindDeclaredName(begin, code_end);
                if (name != Invalid)
                {
                    const auto declarator = Add(GrammarKind::Declarator, name, code_end, parent);
                    Add(GrammarKind::DeclaredName, name, name + 1, declarator);
                    consumed = code_end;
                }
            }

            if (trailing_req != Invalid)
            {
                ParseRequiresConstraint(trailing_req, end, parent);
                return end;
            }

            if (init_pos < end && Is(init_pos, "="))
            {
                const auto expr_end = ParseExpression(
                    init_pos + 1, declarator_end == end ? end : declarator_end, parent);
                if (expr_end > consumed)
                {
                    consumed = expr_end;
                }
            }
            else if (init_pos < end && Is(init_pos, "{") && m_match[init_pos] != Invalid)
            {
                if (m_match[init_pos] + 1 > consumed)
                {
                    consumed = m_match[init_pos] + 1;
                }
            }
            else if (init_pos < end && Is(init_pos, "(") && m_match[init_pos] != Invalid)
            {
                // Direct-list initialization `Type name(args)`: parse args as expressions.
                const auto close = m_match[init_pos];
                auto       arg   = init_pos + 1;
                while (arg < close)
                {
                    const auto next = FindComma(arg, close);
                    if (arg < next)
                    {
                        ParseExpression(arg, next, parent);
                    }

                    if (next == close)
                    {
                        break;
                    }

                    arg = next + 1;
                }

                if (close + 1 > consumed)
                {
                    consumed = close + 1;
                }
            }

            return consumed;
        }

        void AddDeclarationDetails(
            std::size_t begin, std::size_t end, std::size_t parent, bool parameter = false)
        {
            if (begin >= end)
            {
                return;
            }

            // Leading requires-clause: `requires Constraint decl` (constrained template /
            // function).
            auto body_start = begin;
            if (Is(body_start, "requires") && !IsRequiresExpressionAt(body_start, end))
            {
                const auto split = FindLeadingRequiresSplit(body_start, end);
                if (split != Invalid && split > body_start)
                {
                    ParseRequiresConstraint(body_start, split, parent);
                    body_start = split;
                }
            }

            // C++ declarations share one specifier sequence across comma-separated
            // declarators; keep that prefix explicit instead of duplicating it.
            const auto first_comma     = FindComma(body_start, end);
            const auto nodes_before    = m_tree.m_nodes_soa.size();
            const auto spec_end        = ParseDeclSpecifiers(body_start, first_comma, parent);
            const bool has_shared_type = spec_end > body_start;
            if (!has_shared_type)
            {
                while (m_tree.m_nodes_soa.size() > nodes_before)
                {
                    m_tree.m_nodes_soa.kind.pop_back();
                    m_tree.m_nodes_soa.first_token.pop_back();
                    m_tree.m_nodes_soa.token_count.pop_back();
                    m_tree.m_nodes_soa.parent.pop_back();
                    m_tree.m_nodes_soa.subtree_end.pop_back();
                }

                m_tree.m_nodes_aos_dirty = true;
            }

            auto part = body_start;
            while (part < end)
            {
                const auto  comma      = FindComma(part, end);
                const auto  declarator = Add(GrammarKind::InitDeclarator, part, comma, parent);
                std::size_t consumed   = part;
                if (part == body_start && has_shared_type)
                {
                    // First declarator reuses the shared TypeSpecifier node above.
                    const auto code_begin = spec_end;
                    const auto init       = TopLevelAssignment(code_begin, comma);
                    const auto code_end   = init < comma ? init : comma;
                    if (code_begin < code_end)
                    {
                        consumed = ParseSingleDeclarator(code_begin, code_end, declarator);
                    }
                    else
                    {
                        consumed = code_end;
                    }

                    const auto req = FindTopLevelRequires(code_begin, comma);
                    if (req != Invalid && req >= code_begin && !IsRequiresExpressionAt(req, comma))
                    {
                        ParseRequiresConstraint(req, comma, declarator);
                        consumed = comma;
                    }

                    if (init < comma && Is(init, "="))
                    {
                        const auto expr_end = ParseExpression(init + 1, comma, declarator);
                        if (expr_end > consumed)
                        {
                            consumed = expr_end;
                        }
                    }
                    else if (init < comma && Is(init, "{") && m_match[init] != Invalid)
                    {
                        if (m_match[init] + 1 > consumed)
                        {
                            consumed = m_match[init] + 1;
                        }
                    }
                }
                else
                {
                    consumed = AddTypeAndDeclarator(part, comma, declarator, true);
                }

                // Parsing stopped before the `;`/`,` while a new declaration clearly
                // begins: the previous member lost its terminating semicolon. Report it
                // and re-parse the remainder as another declaration so cascading gaps
                // (two missing semicolons in a row) are all surfaced.
                if (consumed > part && consumed < comma &&
                    LooksLikeDeclarationStart(consumed, comma))
                {
                    m_tree.m_diagnostics.push_back({ m_tree.Tokens()[m_sig[consumed]].offset,
                                                     std::string("expected ';' before '") +
                                                         std::string(Text(consumed)) + "'" });
                    SetNodeRange(declarator, part, consumed);
                    part = consumed;
                    continue;
                }

                if (comma == end)
                {
                    break;
                }

                part = comma + 1;
            }

            (void) parameter;
        }

        void AddFunctionParameters(std::size_t begin, std::size_t brace, std::size_t parent)
        {
            std::size_t open = Invalid;
            for (auto i = begin; i < brace; ++i)
            {
                if (Is(i, "(") && m_match[i] != Invalid && m_match[i] < brace)
                {
                    open = i;
                }
            }

            if (open == Invalid)
            {
                return;
            }

            const auto close = m_match[open];
            auto       part  = open + 1;
            // `()` and `(void)` carry no parameters.
            if (part == close)
            {
                return;
            }

            if (part + 1 == close && Is(part, "void"))
            {
                return;
            }

            while (part < close)
            {
                const auto comma = FindComma(part, close);
                if (comma > part && !(comma == part + 1 && Is(part, "...")))
                {
                    const auto parameter =
                        Add(GrammarKind::ParameterDeclaration, part, comma, parent);
                    AddTypeAndDeclarator(part, comma, parameter);
                }

                if (comma == close)
                {
                    break;
                }

                part = comma + 1;
            }
        }

        GrammarKind StatementKind(std::size_t begin, std::size_t end) const
        {
            if (begin >= end)
            {
                return GrammarKind::Error;
            }

            if (Is(begin, "return") || Is(begin, "co_return"))
            {
                return GrammarKind::ReturnStatement;
            }

            if (Is(begin, "if"))
            {
                return GrammarKind::IfStatement;
            }

            if (Is(begin, "while") || Is(begin, "for") || Is(begin, "do"))
            {
                return GrammarKind::LoopStatement;
            }

            if (Is(begin, "switch"))
            {
                return GrammarKind::SwitchStatement;
            }

            if (Is(begin, "break") || Is(begin, "continue") || Is(begin, "goto") ||
                Is(begin, "throw") || Is(begin, "co_yield"))
            {
                return GrammarKind::JumpStatement;
            }

            // `using namespace x;` / `using a::b;` / `using T = U;` inside a body:
            // same node as at namespace scope, not a declaration of a name.
            if (Is(begin, "using"))
            {
                return GrammarKind::UsingDeclaration;
            }

            // `alignas(...)` only decorates declarations.
            if (IsAlignasStart(begin, end))
            {
                return GrammarKind::DeclarationStatement;
            }

            constexpr std::string_view type_words[] = {
                "auto",     "bool",  "char",    "char8_t",  "char16_t",  "char32_t",
                "double",   "float", "int",     "long",     "short",     "signed",
                "unsigned", "void",  "wchar_t", "const",    "constexpr", "static",
                "struct",   "class", "enum",    "typename", "using"
            };
            for (const auto word : type_words)
            {
                if (Text(begin) == word)
                {
                    return GrammarKind::DeclarationStatement;
                }
            }

            if (m_tree.Tokens()[m_sig[begin]].kind == TokenKind::Identifier)
            {
                auto i = begin + 1;
                while (i + 1 < end && Is(i, "::") &&
                       m_tree.Tokens()[m_sig[i + 1]].kind == TokenKind::Identifier)
                {
                    i += kTwo;
                }

                if (Is(i, "<"))
                {
                    std::size_t depth = 0;
                    do
                    {
                        if (Is(i, "<"))
                        {
                            ++depth;
                        }
                        else if (Is(i, ">") && depth != 0)
                        {
                            --depth;
                        }
                        else if (Is(i, ">>") && depth != 0)
                        {
                            depth = depth > 1 ? depth - kTwo : 0;
                        }

                        ++i;
                    } while (i < end && depth != 0);
                }

                while (Is(i, "*") || Is(i, "&") || Is(i, "&&") || Is(i, "const"))
                {
                    ++i;
                }

                if (i < end && m_tree.Tokens()[m_sig[i]].kind == TokenKind::Identifier)
                {
                    // `a * b;` with `a` a variable in scope is a product, whatever it looks like.
                    if (!Is(begin + 1, "::") && ClassOf(Text(begin)) == kNameValue)
                    {
                        return GrammarKind::ExpressionStatement;
                    }

                    return GrammarKind::DeclarationStatement;
                }
            }

            return GrammarKind::ExpressionStatement;
        }

        static int Precedence(std::string_view op)
        {
            if (op == ",")
            {
                return 1;
            }

            if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" ||
                op == "&=" || op == "|=" || op == "^=" || op == "<<=" || op == ">>=")
            {
                return kTwo;
            }

            if (op == "?")
            {
                return kThree;
            }

            if (op == "||")
            {
                return kFour;
            }

            if (op == "&&")
            {
                return kFive;
            }

            if (op == "|")
            {
                return kSix;
            }

            if (op == "^")
            {
                return kSeven;
            }

            if (op == "&")
            {
                return kEight;
            }

            if (op == "==" || op == "!=")
            {
                return kNine;
            }

            if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "<=>")
            {
                return kTen;
            }

            if (op == "<<" || op == ">>")
            {
                return kEleven;
            }

            if (op == "+" || op == "-")
            {
                return kTwelve;
            }

            if (op == "*" || op == "/" || op == "%")
            {
                return kThirteen;
            }

            return 0;
        }

        std::size_t ParseExpression(
            std::size_t pos, std::size_t end, std::size_t parent, int minimum = 1)
        {
            if (pos >= end)
            {
                m_last_expression_node = Invalid;
                return pos;
            }

            const auto  begin       = pos;
            GrammarKind prefix_kind = GrammarKind::ErrorExpression;
            std::size_t root        = Invalid;
            const auto  first       = Text(pos);
            if (first == "+" || first == "-" || first == "!" || first == "~" || first == "*" ||
                first == "&" || first == "++" || first == "--" || first == "co_await" ||
                first == "^^")
            {
                root = Add(GrammarKind::UnaryExpression, begin, begin + 1, parent);
                ++pos;
                if (first == "^^" && pos < end &&
                    (IsBuiltinType(Text(pos)) || Is(pos, "const") || Is(pos, "volatile") ||
                     Is(pos, "signed") || Is(pos, "unsigned")))
                {
                    const auto type_begin = pos;
                    while (pos < end &&
                           (IsBuiltinType(Text(pos)) || Is(pos, "const") || Is(pos, "volatile") ||
                            Is(pos, "signed") || Is(pos, "unsigned") || Is(pos, "*") ||
                            Is(pos, "&") || Is(pos, "&&")))
                    {
                        ++pos;
                    }
                    Add(GrammarKind::TypeSpecifier, type_begin, pos, root);
                }
                else
                {
                    pos = ParseExpression(pos, end, root, kFourteen);
                }
                SetNodeRange(root, begin, pos);
                prefix_kind = GrammarKind::UnaryExpression;
            }
            else if (first == "(" && m_match[pos] != Invalid && m_match[pos] < end &&
                     IsCastTypeId(pos + 1, m_match[pos]) &&
                     StartsCastOperand(m_match[pos] + 1, end))
            {
                // `(T) operand`, with `T` known to be a type: a cast, not a group.
                const auto close = m_match[pos];
                root             = Add(GrammarKind::CastExpression, begin, close + 1, parent);
                Add(GrammarKind::TypeSpecifier, pos + 1, close, root);
                pos = ParseExpression(close + 1, end, root, kFourteen);
                SetNodeRange(root, begin, pos);
                prefix_kind = GrammarKind::CastExpression;
            }
            else if (first == "(" && m_match[pos] != Invalid && m_match[pos] < end)
            {
                const auto close = m_match[pos];
                const auto node = Add(GrammarKind::ParenthesizedExpression, pos, close + 1, parent);
                auto       inner = pos + 1;
                ParseExpression(inner, close, node);
                pos         = close + 1;
                root        = node;
                prefix_kind = GrammarKind::ParenthesizedExpression;
                (void) node;
            }
            else if (first == "[" && Is(pos + 1, ":") && m_match[pos] != Invalid &&
                     m_match[pos] < end && Is(m_match[pos] - 1, ":"))
            {
                const auto close = m_match[pos];
                root             = Add(GrammarKind::SpliceExpression, pos, close + 1, parent);
                ParseExpression(pos + 2, close - 1, root);
                pos         = close + 1;
                prefix_kind = GrammarKind::SpliceExpression;
            }
            else if (first == "[" && m_match[pos] != Invalid && m_match[pos] < end)
            {
                const std::size_t capture_close = m_match[pos];
                std::size_t       body          = capture_close + 1;
                if (Is(body, "("))
                {
                    body = SkipGroup(body, end);
                }

                while (body < end && !Is(body, "{") && !Is(body, ";"))
                {
                    ++body;
                }

                if (Is(body, "{") && m_match[body] != Invalid)
                {
                    const auto lambda =
                        Add(GrammarKind::LambdaExpression, pos, m_match[body] + 1, parent);
                    std::size_t body_pos = body;
                    ParseCompound(body_pos, m_match[body] + 1, lambda);
                    pos         = m_match[body] + 1;
                    root        = lambda;
                    prefix_kind = GrammarKind::LambdaExpression;
                }
                else
                {
                    root        = Add(GrammarKind::ErrorExpression, pos, capture_close + 1, parent);
                    pos         = capture_close + 1;
                    prefix_kind = GrammarKind::ErrorExpression;
                }
            }
            else if (first == "requires")
            {
                auto body = pos + 1;
                if (Is(body, "(") && m_match[body] != Invalid)
                {
                    body = SkipGroup(body, end);
                }

                while (body < end && !Is(body, "{") && !Is(body, ";"))
                {
                    ++body;
                }

                if (Is(body, "{") && m_match[body] != Invalid)
                {
                    const auto close = m_match[body];
                    const auto node  = Add(GrammarKind::RequiresExpression, pos, close + 1, parent);
                    auto       requirement = body + 1;
                    while (requirement < close)
                    {
                        const auto semi            = FindSemicolon(requirement, close);
                        const auto requirement_end = semi < close ? semi + 1 : close;
                        if (requirement_end > requirement)
                        {
                            const auto item =
                                Add(GrammarKind::Requirement, requirement, requirement_end, node);
                            if (semi < close && semi > requirement)
                            {
                                ParseExpression(requirement, semi, item);
                            }
                        }

                        requirement = requirement_end;
                    }

                    pos         = close + 1;
                    root        = node;
                    prefix_kind = GrammarKind::RequiresExpression;
                }
                else
                {
                    root        = Add(GrammarKind::ErrorExpression, pos, body, parent);
                    pos         = body;
                    prefix_kind = GrammarKind::ErrorExpression;
                }
            }
            else if (m_tree.Tokens()[m_sig[pos]].kind == TokenKind::Identifier)
            {
                root = Add(GrammarKind::IdentifierExpression, pos, pos + 1, parent);
                ++pos;
                prefix_kind = GrammarKind::IdentifierExpression;
            }
            else if (m_tree.Tokens()[m_sig[pos]].kind == TokenKind::Number ||
                     m_tree.Tokens()[m_sig[pos]].kind == TokenKind::StringLiteral ||
                     m_tree.Tokens()[m_sig[pos]].kind == TokenKind::CharacterLiteral ||
                     m_tree.Tokens()[m_sig[pos]].kind == TokenKind::RawStringLiteral ||
                     first == "true" || first == "false" || first == "nullptr")
            {
                root = Add(GrammarKind::LiteralExpression, pos, pos + 1, parent);
                ++pos;
                prefix_kind = GrammarKind::LiteralExpression;
            }
            else if (first == "{" && m_match[pos] != Invalid && m_match[pos] < end)
            {
                root        = Add(GrammarKind::LiteralExpression, pos, m_match[pos] + 1, parent);
                pos         = m_match[pos] + 1;
                prefix_kind = GrammarKind::LiteralExpression;
            }
            else
            {
                m_tree.m_diagnostics.push_back(
                    { m_tree.Tokens()[m_sig[pos]].offset, "expected expression" });
                root                   = Add(GrammarKind::ErrorExpression, pos, pos + 1, parent);
                m_last_expression_node = root;
                return pos + 1;
            }

            // `new`/`delete` expressions take a type/operand that must not be mistaken
            // for the start of the following declaration (e.g. `new int[5]` before `;`).
            bool operand_pending = first == "new" || first == "delete";
            while (pos < end)
            {
                const auto op = Text(pos);
                if ((op == "(" || op == "[") && m_match[pos] != Invalid && m_match[pos] < end)
                {
                    const auto close = m_match[pos];
                    const auto node_kind =
                        op == "(" ? GrammarKind::CallExpression : GrammarKind::SubscriptExpression;
                    const auto node = Add(node_kind, begin, close + 1, parent);
                    if (root != Invalid)
                    {
                        m_tree.m_nodes_soa.parent[root] = static_cast<std::uint32_t>(node);
                        m_tree.m_nodes_aos_dirty        = true;
                    }

                    if (op == "(")
                    {
                        auto arg = pos + 1;
                        while (arg < close)
                        {
                            const auto next = FindComma(arg, close);
                            if (arg < next)
                            {
                                ParseExpression(arg, next, node);
                            }

                            arg = next + 1;
                        }
                    }
                    else
                    {
                        ParseExpression(pos + 1, close, node);
                    }

                    pos  = close + 1;
                    root = node;
                    continue;
                }

                if (op == "." || op == "->" || op == "::")
                {
                    if (pos + 1 >= end)
                    {
                        break;
                    }

                    const auto node = Add(GrammarKind::MemberExpression, begin, pos + kTwo, parent);
                    if (root != Invalid)
                    {
                        m_tree.m_nodes_soa.parent[root] = static_cast<std::uint32_t>(node);
                        m_tree.m_nodes_aos_dirty        = true;
                    }

                    Add(GrammarKind::IdentifierExpression, pos + 1, pos + kTwo, node);
                    pos += kTwo;
                    root = node;
                    continue;
                }

                if (op == "<" && root != Invalid &&
                    (static_cast<GrammarKind>(m_tree.m_nodes_soa.kind[root]) ==
                         GrammarKind::IdentifierExpression ||
                     static_cast<GrammarKind>(m_tree.m_nodes_soa.kind[root]) ==
                         GrammarKind::MemberExpression ||
                     static_cast<GrammarKind>(m_tree.m_nodes_soa.kind[root]) ==
                         GrammarKind::TemplateIdExpression))
                {
                    const auto close          = FindTemplateClose(pos, end);
                    const auto after_template = close == Invalid ? end : close + 1;
                    const auto follows_template =
                        after_template == end || Is(after_template, "(") ||
                        Is(after_template, "{") || Is(after_template, "::") ||
                        Is(after_template, ",") || Is(after_template, ">") ||
                        Is(after_template, ">>") || Is(after_template, ";") ||
                        Is(after_template, ")") || Is(after_template, "]") ||
                        Is(after_template, ".") || Is(after_template, "->") ||
                        Is(after_template, "?") || Is(after_template, ":") ||
                        Is(after_template, "||") || Is(after_template, "&&") ||
                        Is(after_template, "|") || Is(after_template, "^") ||
                        Is(after_template, "&") || Is(after_template, "==") ||
                        Is(after_template, "!=") || Is(after_template, "<") ||
                        Is(after_template, ">") || Is(after_template, "<=") ||
                        Is(after_template, ">=") || Is(after_template, "<=>") ||
                        Is(after_template, "<<") || Is(after_template, ">>") ||
                        Is(after_template, "+") || Is(after_template, "-") ||
                        Is(after_template, "*") || Is(after_template, "/") ||
                        Is(after_template, "%") || Is(after_template, "++") ||
                        Is(after_template, "--") ||
                        m_tree.Tokens()[m_sig[after_template]].kind == TokenKind::Identifier;
                    if (close != Invalid && follows_template)
                    {
                        const auto node =
                            Add(GrammarKind::TemplateIdExpression, begin, close + 1, parent);
                        m_tree.m_nodes_soa.parent[root] = static_cast<std::uint32_t>(node);
                        m_tree.m_nodes_aos_dirty        = true;
                        auto arg                        = pos + 1;
                        while (arg < close)
                        {
                            const auto comma = FindComma(arg, close);
                            if (comma == arg)
                            {
                                m_tree.m_diagnostics.push_back({ m_tree.Tokens()[m_sig[arg]].offset,
                                                                 "expected template argument" });
                                ++arg;
                                continue;
                            }

                            const auto argument =
                                Add(GrammarKind::TemplateArgument, arg, comma, node);
                            if (arg < comma)
                            {
                                ParseExpression(arg, comma, argument);
                            }

                            if (comma == close)
                            {
                                break;
                            }

                            arg = comma + 1;
                        }

                        pos  = close + 1;
                        root = node;
                        continue;
                    }
                }

                if (op == "++" || op == "--")
                {
                    const auto node = Add(GrammarKind::UnaryExpression, begin, pos + 1, parent);
                    if (root != Invalid)
                    {
                        m_tree.m_nodes_soa.parent[root] = static_cast<std::uint32_t>(node);
                        m_tree.m_nodes_aos_dirty        = true;
                    }

                    ++pos;
                    root = node;
                    continue;
                }

                if (op == "?" && minimum <= kThree)
                {
                    const auto node = Add(GrammarKind::ConditionalExpression, begin, end, parent);
                    if (root != Invalid)
                    {
                        m_tree.m_nodes_soa.parent[root] = static_cast<std::uint32_t>(node);
                        m_tree.m_nodes_aos_dirty        = true;
                    }

                    ++pos;
                    pos = ParseExpression(pos, end, node);
                    if (Is(pos, ":"))
                    {
                        ++pos;
                    }

                    pos = ParseExpression(pos, end, node, kTwo);
                    SetNodeRange(node, begin, pos);
                    root = node;
                    continue;
                }

                // A user-defined literal suffix (`"abc"s`, `1_km`) only counts when it
                // directly touches the literal; otherwise an identifier here cannot
                // continue the expression and signals a missing ';'.
                if (m_tree.Tokens()[m_sig[pos]].kind == TokenKind::Identifier && pos > begin &&
                    (m_tree.Tokens()[m_sig[pos - 1]].kind == TokenKind::Number ||
                     m_tree.Tokens()[m_sig[pos - 1]].kind == TokenKind::StringLiteral ||
                     m_tree.Tokens()[m_sig[pos - 1]].kind == TokenKind::CharacterLiteral ||
                     m_tree.Tokens()[m_sig[pos - 1]].kind == TokenKind::RawStringLiteral))
                {
                    const auto& previous = m_tree.Tokens()[m_sig[pos - 1]];
                    const auto& current  = m_tree.Tokens()[m_sig[pos]];
                    if (current.offset == previous.offset + previous.length)
                    {
                        ++pos;
                        continue;
                    }
                }

                // C-style cast `(Type) operand` and `new`/`delete` operands: an
                // expression-start token after the type/keyword continues this
                // expression rather than starting a new declaration.
                const bool after_paren =
                    root != Invalid && static_cast<GrammarKind>(m_tree.m_nodes_soa.kind[root]) ==
                                           GrammarKind::ParenthesizedExpression;
                const auto token_kind_here = m_tree.Tokens()[m_sig[pos]].kind;
                const bool operand_start =
                    token_kind_here == TokenKind::Identifier ||
                    token_kind_here == TokenKind::Number ||
                    token_kind_here == TokenKind::StringLiteral ||
                    token_kind_here == TokenKind::CharacterLiteral ||
                    token_kind_here == TokenKind::RawStringLiteral || Is(pos, "::");
                if ((after_paren || operand_pending) && operand_start)
                {
                    const auto operand_end = ParseExpression(pos, end, parent, kFourteen);
                    if (operand_end > pos)
                    {
                        pos = operand_end;
                        if (m_last_expression_node != Invalid)
                        {
                            root = m_last_expression_node;
                        }

                        operand_pending = false;
                        continue;
                    }
                }

                const int precedence = Precedence(op);
                if (precedence < minimum || precedence == 0)
                {
                    break;
                }

                const auto node = Add(GrammarKind::BinaryExpression, begin, end, parent);
                if (root != Invalid)
                {
                    m_tree.m_nodes_soa.parent[root] = static_cast<std::uint32_t>(node);
                    m_tree.m_nodes_aos_dirty        = true;
                }

                ++pos;
                const bool right_associative = precedence == kTwo;
                pos = ParseExpression(pos, end, node, precedence + (right_associative ? 0 : 1));
                SetNodeRange(node, begin, pos);
                root = node;
            }

            m_last_expression_node = root;
            return pos;
        }

        std::size_t ParseStaticAssert(std::size_t pos, std::size_t end, std::size_t parent)
        {
            if (!Is(pos + 1, "(") || m_match[pos + 1] == Invalid ||
                m_match[pos + 1] >= end || !Is(m_match[pos + 1] + 1, ";"))
            {
                return pos;
            }

            const auto close = m_match[pos + 1];
            const auto node  = Add(GrammarKind::StaticAssertDeclaration, pos, close + kTwo, parent);
            const auto comma = FindComma(pos + kTwo, close);
            ParseExpression(pos + kTwo, comma, node);
            return close + kTwo;
        }

        void ParseStatement(std::size_t& pos, std::size_t end, std::size_t parent)
        {
            const auto start = pos;
            if (Is(pos, "}"))
            {
                return;
            }

            if (Is(pos, ";"))
            {
                Add(GrammarKind::EmptyStatement, pos, pos + 1, parent);
                ++pos;
                return;
            }

            if (Is(pos, "{"))
            {
                ParseCompound(pos, end, parent);
                return;
            }

            if (Is(pos, "static_assert"))
            {
                const auto next = ParseStaticAssert(pos, end, parent);
                if (next != pos)
                {
                    pos = next;
                    return;
                }
            }

            const auto keyword = Text(pos);
            if (keyword == "case" || keyword == "default")
            {
                auto colon = pos + 1;
                while (colon < end && !Is(colon, ":") && !Is(colon, "}"))
                {
                    if ((Is(colon, "(") || Is(colon, "[") || Is(colon, "{")) &&
                        m_match[colon] != Invalid)
                    {
                        colon = m_match[colon] + 1;
                    }
                    else
                    {
                        ++colon;
                    }
                }

                if (Is(colon, ":"))
                {
                    ++colon;
                }
                else
                {
                    m_tree.m_diagnostics.push_back(
                        { m_tree.Tokens()[m_sig[pos]].offset, "expected ':' after case label" });
                }

                Add(GrammarKind::CaseLabel, pos, colon, parent);
                pos = colon;
                return;
            }

            if (keyword == "do")
            {
                const auto node = Add(GrammarKind::DoStatement, pos, pos + 1, parent);
                ++pos;
                if (pos < end)
                {
                    ParseStatement(pos, end, node);
                }

                if (Is(pos, "while"))
                {
                    ++pos;
                    if (Is(pos, "(") && m_match[pos] != Invalid)
                    {
                        pos = SkipGroup(pos, end);
                    }

                    if (Is(pos, ";"))
                    {
                        ++pos;
                    }
                }
                else
                {
                    m_tree.m_diagnostics.push_back({ m_tree.Tokens()[m_sig[start]].offset,
                                                     "expected while after do statement" });
                }

                const auto past =
                    pos > start ? m_sig[pos - 1] + 1 : m_tree.m_nodes_soa.first_token[node];
                m_tree.m_nodes_soa.token_count[node] = past - m_tree.m_nodes_soa.first_token[node];
                m_tree.m_nodes_aos_dirty             = true;
                return;
            }

            if (keyword == "try")
            {
                const auto node = Add(GrammarKind::TryStatement, pos, pos + 1, parent);
                ++pos;
                if (pos < end && Is(pos, "{"))
                {
                    ParseStatement(pos, end, node);
                }

                while (Is(pos, "catch"))
                {
                    const auto catch_start = pos++;
                    if (Is(pos, "(") && m_match[pos] != Invalid)
                    {
                        pos = SkipGroup(pos, end);
                    }

                    if (pos < end && Is(pos, "{"))
                    {
                        ParseStatement(pos, end, node);
                    }
                    else
                    {
                        m_tree.m_diagnostics.push_back(
                            { m_tree.Tokens()[m_sig[catch_start]].offset,
                              "expected compound statement after catch clause" });
                    }
                }

                const auto past =
                    pos > start ? m_sig[pos - 1] + 1 : m_tree.m_nodes_soa.first_token[node];
                m_tree.m_nodes_soa.token_count[node] = past - m_tree.m_nodes_soa.first_token[node];
                m_tree.m_nodes_aos_dirty             = true;
                return;
            }

            const bool control =
                keyword == "if" || keyword == "while" || keyword == "for" || keyword == "switch";
            if (control)
            {
                const auto kind = keyword == "if"       ? GrammarKind::IfStatement
                                  : keyword == "switch" ? GrammarKind::SwitchStatement
                                                        : GrammarKind::LoopStatement;
                const auto node = Add(kind, start, start + 1, parent);
                m_block_starts.push_back(
                    m_block_names.size()); // `for (int i...)` is visible to the body only
                ++pos;
                if (keyword == "if")
                {
                    // `if constexpr (c)`, `if consteval {}` and `if !consteval {}`.
                    if (Is(pos, "constexpr") || Is(pos, "consteval"))
                    {
                        ++pos;
                    }
                    else if (Is(pos, "!") && Is(pos + 1, "consteval"))
                    {
                        pos += 2;
                    }
                }

                if (Is(pos, "(") && m_match[pos] != Invalid)
                {
                    const auto close = m_match[pos];
                    if (keyword == "for")
                    {
                        const auto first_sep = FindSemicolon(pos + 1, close);
                        if (first_sep < close)
                        {
                            const auto second_sep = FindSemicolon(first_sep + 1, close);
                            if (StatementKind(pos + 1, first_sep) ==
                                GrammarKind::DeclarationStatement)
                            {
                                const auto init = Add(GrammarKind::DeclarationStatement, pos + 1,
                                                      first_sep, node);
                                AddDeclarationDetails(pos + 1, first_sep, init);
                                RegisterNodes(init, m_tree.m_nodes_soa.size());
                            }
                            else if (first_sep > pos + 1)
                            {
                                ParseExpression(pos + 1, first_sep, node);
                            }

                            if (second_sep < close && second_sep > first_sep + 1)
                            {
                                ParseExpression(first_sep + 1, second_sep, node);
                            }

                            if (second_sep < close && second_sep + 1 < close)
                            {
                                ParseExpression(second_sep + 1, close, node);
                            }
                        }
                        else
                        {
                            auto colon = pos + 1;
                            while (colon < close && !Is(colon, ":"))
                            {
                                if ((Is(colon, "(") || Is(colon, "[") || Is(colon, "{")) &&
                                    m_match[colon] != Invalid)
                                {
                                    colon = m_match[colon] + 1;
                                }
                                else
                                {
                                    ++colon;
                                }
                            }

                            if (colon < close)
                            {
                                const auto init =
                                    Add(GrammarKind::DeclarationStatement, pos + 1, colon, node);
                                AddDeclarationDetails(pos + 1, colon, init);
                                RegisterNodes(init, m_tree.m_nodes_soa.size());
                                if (colon + 1 < close)
                                {
                                    ParseExpression(colon + 1, close, node);
                                }
                            }
                            else
                            {
                                ParseExpression(pos + 1, close, node);
                            }
                        }
                    }
                    else
                    {
                        ParseExpression(pos + 1, close, node);
                    }

                    pos = close + 1;
                }

                if (pos < end && !Is(pos, "}"))
                {
                    ParseStatement(pos, end, node);
                }

                if (keyword == "if" && Is(pos, "else"))
                {
                    ++pos;
                    if (pos < end && !Is(pos, "}"))
                    {
                        ParseStatement(pos, end, node);
                    }
                }

                const auto past =
                    pos > start ? m_sig[pos - 1] + 1 : m_tree.m_nodes_soa.first_token[node];
                m_tree.m_nodes_soa.token_count[node] = past - m_tree.m_nodes_soa.first_token[node];
                m_tree.m_nodes_aos_dirty             = true;
                PopBlock();
                return;
            }

            const auto semicolon = FindSemicolon(pos, end);
            if (semicolon < end)
            {
                if (StatementKind(start, semicolon + 1) == GrammarKind::DeclarationStatement)
                {
                    for (auto i = start + 1; i < semicolon; ++i)
                    {
                        if ((Is(i, "(") || Is(i, "[") || Is(i, "{")) && m_match[i] != Invalid &&
                            m_match[i] > i)
                        {
                            i = m_match[i];
                            continue;
                        }

                        if (Is(i, "return") || Is(i, "co_return"))
                        {
                            m_tree.m_diagnostics.push_back(
                                { m_tree.Tokens()[m_sig[i]].offset,
                                  "expected ';' before return statement" });
                            Add(GrammarKind::Error, start, i, parent);
                            pos = i;
                            return;
                        }
                    }
                }

                pos                         = semicolon + 1;
                const auto kind             = StatementKind(start, pos);
                const auto node             = Add(kind, start, pos, parent);
                auto       expression_begin = start;
                if (kind == GrammarKind::ReturnStatement)
                {
                    ++expression_begin;
                }

                if (expression_begin < semicolon && (kind == GrammarKind::ReturnStatement ||
                                                     kind == GrammarKind::ExpressionStatement))
                {
                    const auto expr_end = ParseExpression(expression_begin, semicolon, node);
                    // Tokens left over that look like the next statement mean the `;`
                    // is missing: report it, shrink this node, and resume parsing from
                    // the leftover so the following statement stands on its own.
                    if (expr_end > expression_begin && expr_end < semicolon &&
                        LooksLikeDeclarationStart(expr_end, semicolon))
                    {
                        m_tree.m_diagnostics.push_back({ m_tree.Tokens()[m_sig[expr_end]].offset,
                                                         std::string("expected ';' before '") +
                                                             std::string(Text(expr_end)) + "'" });
                        SetNodeRange(node, start, expr_end);
                        pos = expr_end;
                    }
                }

                if (kind == GrammarKind::DeclarationStatement)
                {
                    AddDeclarationDetails(start, semicolon, node);
                }

                return;
            }

            if (Is(pos, "}"))
            {
                return;
            }

            m_tree.m_diagnostics.push_back({ m_tree.Tokens()[m_sig[start]].offset,
                                             "expected ';' before end of compound statement" });
            while (pos < end && !Is(pos, "}") && !Is(pos, ";"))
            {
                ++pos;
            }

            if (Is(pos, ";"))
            {
                ++pos;
            }

            if (pos == start)
            {
                ++pos;
            }

            Add(GrammarKind::Error, start, pos, parent);
        }

        std::size_t ParseCompound(std::size_t& pos, std::size_t end, std::size_t parent)
        {
            const auto start = pos++;
            const auto node  = Add(GrammarKind::CompoundStatement, start, start + 1, parent);
            m_block_starts.push_back(m_block_names.size());
            if (parent < m_tree.m_nodes_soa.size() &&
                static_cast<GrammarKind>(m_tree.m_nodes_soa.kind[parent]) ==
                    GrammarKind::FunctionDefinition)
            {
                DeclareParameters(parent);
            }

            while (pos < end && !Is(pos, "}"))
            {
                const auto before       = pos;
                const auto nodes_before = m_tree.m_nodes_soa.size();
                if (IsDirective(pos))
                {
                    pos = SkipDirective(pos, end, node);
                }
                else
                {
                    ParseStatement(pos, end, node);
                    // Only statements that can declare something are worth scanning.
                    if (nodes_before < m_tree.m_nodes_soa.size())
                    {
                        const auto first_kind =
                            static_cast<GrammarKind>(m_tree.m_nodes_soa.kind[nodes_before]);
                        if (first_kind == GrammarKind::DeclarationStatement ||
                            first_kind == GrammarKind::RecordDefinition ||
                            first_kind == GrammarKind::UsingDeclaration ||
                            first_kind == GrammarKind::Declaration)
                        {
                            RegisterNodes(nodes_before, m_tree.m_nodes_soa.size());
                        }
                    }
                }

                if (pos == before)
                {
                    ++pos;
                }
            }

            if (pos < end && Is(pos, "}"))
            {
                ++pos;
            }
            else
            {
                m_tree.m_diagnostics.push_back({ m_tree.Tokens()[m_sig[start]].offset,
                                                 "expected '}' to close compound statement" });
            }

            PopBlock();

            auto&      record = m_tree.m_nodes_soa;
            const auto past   = pos > start && pos - 1 < m_sig.size() ? m_sig[pos - 1] + 1
                                                                      : record.first_token[node];
            record.token_count[node] = past - record.first_token[node];
            m_tree.m_nodes_aos_dirty = true;
            return node;
        }

        // Enum bodies hold comma-separated enumerators (`A, B = expr`), not the
        // semicolon-terminated declarations that ordinary record scopes expect.
        void ParseEnumerators(std::size_t begin, std::size_t end, std::size_t parent)
        {
            auto pos = begin;
            while (pos < end)
            {
                if (IsDirective(pos))
                {
                    pos = SkipDirective(pos, end, parent);
                    continue;
                }

                const auto comma = FindComma(pos, end);
                const auto stop  = comma < end ? comma : end;
                if (stop > pos)
                {
                    const auto enumerator = Add(GrammarKind::Enumerator, pos, stop, parent);
                    auto       name       = SkipAttributes(pos, stop, enumerator);
                    if (IsIdentifierToken(name) && name < stop)
                    {
                        Add(GrammarKind::DeclaredName, name, name + 1, enumerator);
                        ++name;
                        name = SkipAttributes(name, stop, enumerator);
                    }

                    // An explicit value ends the enumerator; track the consumed extent
                    // so a missing comma is caught below instead of swallowing the next
                    // enumerator into this one.
                    auto consumed = name;
                    for (auto i = name; i < stop; ++i)
                    {
                        if (Is(i, "="))
                        {
                            consumed = ParseExpression(i + 1, stop, enumerator);
                            break;
                        }
                    }

                    // Skip trailing attributes without emitting: only an identifier can
                    // start the next enumerator here.
                    auto rest = consumed;
                    while (IsAttributeStart(rest, stop))
                    {
                        const auto outer_close = m_match[rest];
                        if (outer_close == Invalid || outer_close >= stop)
                        {
                            break;
                        }

                        rest = outer_close + 1;
                    }

                    // Tokens left over that look like another enumerator mean the
                    // separating comma was forgotten: report it, shrink this node,
                    // and re-parse the remainder as its own enumerator.
                    if (rest > pos && rest < stop && IsIdentifierToken(rest))
                    {
                        m_tree.m_diagnostics.push_back({ m_tree.Tokens()[m_sig[rest]].offset,
                                                         std::string("expected ',' before '") +
                                                             std::string(Text(rest)) + "'" });
                        SetNodeRange(enumerator, pos, rest);
                        pos = rest;
                        continue;
                    }
                }

                if (comma >= end)
                {
                    break;
                }

                pos = comma + 1;
            }
        }

        // True when every bracket in sig range [begin, end) is matched inside it,
        // so the range's parse cannot depend on delimiters outside of it.
        bool BracketsClosedWithin(std::size_t begin, std::size_t end) const
        {
            for (auto i = begin; i < end; ++i)
            {
                const auto text = Text(i);
                if (text.size() == 1 &&
                    (text[0] == '(' || text[0] == '[' || text[0] == '{' || text[0] == ')' ||
                     text[0] == ']' || text[0] == '}') &&
                    (m_match[i] == Invalid || m_match[i] < begin || m_match[i] >= end))
                {
                    return false;
                }
            }

            return true;
        }

        // ---- names: what is a type, what is a value -----------------------------
        //
        // The grammar reads `a * b;` as a declaration and `(a)x` as a parenthesized
        // expression because, from token shapes alone, both are possible. A compiler
        // settles it by looking the name up while it parses; so does this parser. It
        // registers the names it has already parsed (types and values, in block
        // scopes and at file level) and asks the TypeNameOracle about the rest (the
        // included headers). A name nobody knows keeps the shape-based reading, so
        // missing information never changes what the parser did before.
        static constexpr std::uint8_t kNameType  = 1;
        static constexpr std::uint8_t kNameValue = 2;

        static std::uint64_t HashName(std::string_view name)
        {
            std::uint64_t hash = 14695981039346656037ull;
            for (const char c : name)
            {
                hash ^= static_cast<unsigned char>(c);
                hash *= 1099511628211ull;
            }

            return hash;
        }

        // What one entry contributes to the order-independent hash of the table.
        static std::uint64_t MixEntry(std::uint64_t hash, std::uint8_t flags)
        {
            hash = (hash ^ (static_cast<std::uint64_t>(flags) * 0x9E3779B97F4A7C15ull)) *
                   0xBF58476D1CE4E5B9ull;
            return hash ^ (hash >> 32);
        }

        NameSlot* FindSlot(std::string_view name, std::uint64_t hash)
        {
            if (m_slots.empty())
            {
                return nullptr;
            }

            const std::size_t mask = m_slots.size() - 1;
            for (std::size_t i = hash & mask;; i = (i + 1) & mask)
            {
                NameSlot& slot = m_slots[i];
                if (slot.flags == 0)
                {
                    return &slot; // empty: where it would go
                }

                if (slot.hash == hash && slot.name == name)
                {
                    return &slot;
                }
            }
        }

        void Declare(std::string_view name, std::uint8_t flags)
        {
            if (name.empty())
            {
                return;
            }

            if (!m_block_starts.empty())
            {
                m_block_names.push_back({ name, flags });
                return;
            }

            if (m_slot_count * 10 >= m_slots.size() * 7)
            {
                std::vector<NameSlot> old(m_slots.empty() ? 64 : m_slots.size() * 2);
                old.swap(m_slots);
                for (const NameSlot& slot : old)
                {
                    if (slot.flags != 0)
                    {
                        *FindSlot(slot.name, slot.hash) = slot;
                    }
                }
            }

            const std::uint64_t hash   = HashName(name);
            NameSlot*           slot   = FindSlot(name, hash);
            const std::uint8_t  before = slot->flags;
            const auto          merged = static_cast<std::uint8_t>(before | flags);
            if (merged == before)
            {
                return;
            }

            if (before == 0)
            {
                slot->name = name;
                slot->hash = hash;
                ++m_slot_count;
            }
            else
            {
                m_names_hash ^= MixEntry(hash, before);
            }

            slot->flags = merged;
            // Order-independent, so a replay of reused items reaches the same hash.
            m_names_hash ^= MixEntry(hash, merged);
        }

        // 0 when unknown; kNameType or kNameValue when the innermost declaration
        // says so; both bits when the file declares the name both ways.
        std::uint8_t ClassOf(std::string_view name) const
        {
            for (auto entry = m_block_names.rbegin(); entry != m_block_names.rend(); ++entry)
            {
                if (entry->name == name)
                {
                    return entry->flags;
                }
            }

            if (!m_slots.empty())
            {
                const NameSlot* slot =
                    const_cast<GrammarParser*>(this)->FindSlot(name, HashName(name));
                if (slot->flags != 0)
                {
                    return slot->flags;
                }
            }

            return m_oracle != nullptr && m_oracle->IsType(name) ? kNameType : std::uint8_t { 0 };
        }

        std::string_view RawText(std::uint32_t token) const
        {
            const auto& t = m_tree.Tokens()[token];
            return m_tree.m_source.substr(t.offset, t.length);
        }

        static bool StartsTag(std::string_view word)
        {
            return word == "class" || word == "struct" || word == "union" || word == "enum" ||
                   word == "typedef";
        }

        std::size_t SigOf(std::uint32_t token) const
        {
            return static_cast<std::size_t>(
                std::lower_bound(m_sig.begin(), m_sig.end(), token) - m_sig.begin());
        }

        // Name introduced by `class X`, `struct [[a]] X`, `enum class X`, `union X`
        // starting at sig index `begin` (the keyword may be preceded by `typedef`).
        std::string_view TagNameAt(std::size_t begin) const
        {
            std::size_t i = begin;
            if (Is(i, "typedef"))
            {
                ++i;
            }

            if (!(Is(i, "class") || Is(i, "struct") || Is(i, "union") || Is(i, "enum")))
            {
                return {};
            }

            const bool is_enum = Is(i, "enum");
            ++i;
            if (is_enum && (Is(i, "class") || Is(i, "struct")))
            {
                ++i;
            }

            while (i < m_sig.size() && (IsAttributeStart(i, m_sig.size()) || Is(i, "alignas")))
            {
                if (Is(i, "alignas"))
                {
                    i = Is(i + 1, "(") && m_match[i + 1] != Invalid ? m_match[i + 1] + 1 : i + 1;
                }
                else
                {
                    const auto close = m_match[i];
                    i                = close != Invalid && close > i ? close + 1 : i + 1;
                    // `[[` ... `]]`: skip the inner pair too
                    if (i < m_sig.size() && Is(i, "]"))
                    {
                        ++i;
                    }
                }
            }

            if (i < m_sig.size() && IsIdentifierToken(i) && m_sig_tok[i] == Tok::None &&
                !Is(i + 1, "::"))
            {
                return Text(i);
            }

            return {};
        }

        // Registers the names the nodes in [from, to) declare: types, variables,
        // functions, enumerators. Function bodies, lambdas and parameter lists are
        // skipped: their names belong to the block scopes opened by ParseCompound.
        void RegisterNodes(std::size_t from, std::size_t to)
        {
            const auto& nodes = m_tree.m_nodes_soa;
            to                = std::min(to, nodes.size());
            for (std::size_t n = from; n < to; ++n)
            {
                const std::uint32_t first = nodes.first_token[n];
                const auto          kind  = static_cast<GrammarKind>(nodes.kind[n]);
                switch (kind)
                {
                    case GrammarKind::CompoundStatement:
                    case GrammarKind::LambdaExpression:
                    case GrammarKind::ParameterDeclaration: {
                        // Everything inside starts before `limit`, and whatever follows starts at
                        // or after it: the end of the subtree is found by bisection instead of a
                        // walk.
                        const std::uint32_t limit = first + nodes.token_count[n];
                        std::size_t         low   = n + 1;
                        std::size_t         high  = to;
                        while (low < high)
                        {
                            const std::size_t mid = low + (high - low) / 2;
                            if (nodes.first_token[mid] < limit)
                            {
                                low = mid + 1;
                            }
                            else
                            {
                                high = mid;
                            }
                        }

                        n = low - 1;
                        break;
                    }
                    case GrammarKind::RecordDefinition:
                        Declare(TagNameAt(SigOf(first)), kNameType);
                        break;
                    case GrammarKind::UsingDeclaration: {
                        const auto head = SigOf(first);
                        if (Is(head, "using") && IsIdentifierToken(head + 1) && Is(head + 2, "="))
                        {
                            Declare(Text(head + 1), kNameType);
                        }

                        break;
                    }
                    case GrammarKind::Declaration:
                    case GrammarKind::DeclarationStatement:
                        // `struct Foo;` and `typedef struct Foo Bar;` name types too.
                        if (StartsTag(RawText(first)))
                        {
                            Declare(TagNameAt(SigOf(first)), kNameType);
                        }

                        break;
                    case GrammarKind::Enumerator:
                        Declare(RawText(first), kNameValue);
                        break;
                    case GrammarKind::DeclaredName: {
                        const auto declarator = nodes.parent[n];
                        if (declarator >= nodes.size() ||
                            static_cast<GrammarKind>(nodes.kind[declarator]) !=
                                GrammarKind::Declarator)
                        {
                            break;
                        }

                        // One token: unqualified. `A::f` is a member defined out of line, already
                        // known.
                        if (nodes.token_count[n] != 1 ||
                            m_tree.Tokens()[first].kind != TokenKind::Identifier)
                        {
                            break;
                        }

                        // Climb out of the declarator to the declaration that owns it. Only a
                        // plain variable declaration records a value: functions are not recorded
                        // (a constructor shares its class's name, and `f * x;` is never a
                        // declaration of something called f), nor are parameters.
                        std::size_t owner = nodes.parent[n];
                        while (owner < nodes.size())
                        {
                            const auto owner_kind = static_cast<GrammarKind>(nodes.kind[owner]);
                            if (owner_kind != GrammarKind::Declarator &&
                                owner_kind != GrammarKind::InitDeclarator)
                            {
                                break;
                            }

                            owner = nodes.parent[owner];
                        }

                        if (owner >= nodes.size() ||
                            (static_cast<GrammarKind>(nodes.kind[owner]) !=
                                 GrammarKind::Declaration &&
                             static_cast<GrammarKind>(nodes.kind[owner]) !=
                                 GrammarKind::DeclarationStatement))
                        {
                            break;
                        }

                        const bool typedef_declaration =
                            owner < nodes.size() && RawText(nodes.first_token[owner]) == "typedef";
                        Declare(RawText(first), typedef_declaration ? kNameType : kNameValue);
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        void PopBlock()
        {
            m_block_names.resize(m_block_starts.back());
            m_block_starts.pop_back();
        }

        void RegisterPending()
        {
            const std::size_t size = m_tree.m_nodes_soa.size();
            if (m_registered_upto < size)
            {
                RegisterNodes(m_registered_upto, size);
            }

            m_registered_upto = size;
        }

        // Parameters of the function whose body is about to be parsed.
        void DeclareParameters(std::size_t function)
        {
            const auto&       nodes = m_tree.m_nodes_soa;
            const std::size_t size  = nodes.size();
            for (std::size_t n = function + 1; n < size; ++n)
            {
                if (static_cast<GrammarKind>(nodes.kind[n]) != GrammarKind::ParameterDeclaration)
                {
                    continue;
                }

                const std::uint32_t limit = nodes.first_token[n] + nodes.token_count[n];
                for (std::size_t m = n + 1; m < size && nodes.first_token[m] < limit; ++m)
                {
                    if (static_cast<GrammarKind>(nodes.kind[m]) == GrammarKind::DeclaredName)
                    {
                        const auto head = SigOf(nodes.first_token[m]);
                        if (IsIdentifierToken(head) && !Is(head + 1, "::"))
                        {
                            Declare(Text(head), kNameValue);
                        }
                    }
                }
            }
        }

        // `(begin, end)` read as a type-id whose head is certainly a type: a builtin
        // (`unsigned long`) or a name the file or its headers declare as a type, then
        // any `*`, `&`, `const`.
        bool IsCastTypeId(std::size_t begin, std::size_t end) const
        {
            std::size_t i          = begin;
            const auto  qualifiers = [&]() {
                while (i < end && (Is(i, "const") || Is(i, "volatile")))
                {
                    ++i;
                }
            };
            qualifiers();
            if (i >= end)
            {
                return false;
            }

            const auto builtin = [&](std::size_t at) {
                const auto text = Text(at);
                return IsBuiltinType(text) && text != "auto" && text != "decltype";
            };
            if (builtin(i))
            {
                while (i < end && (builtin(i) || Is(i, "const") || Is(i, "volatile")))
                {
                    ++i;
                }
            }
            else
            {
                if (Is(i, "typename") || Is(i, "struct") || Is(i, "class") || Is(i, "union") ||
                    Is(i, "enum"))
                {
                    ++i;
                }

                if (Is(i, "::"))
                {
                    ++i;
                }

                std::string_view last;
                while (i < end && IsIdentifierToken(i) && m_sig_tok[i] == Tok::None)
                {
                    last = Text(i);
                    ++i;
                    if (i < end && Is(i, "<"))
                    {
                        const auto close = FindTemplateClose(i, end);
                        if (close == Invalid)
                        {
                            return false;
                        }

                        i = close + 1;
                    }

                    if (i + 1 < end && Is(i, "::"))
                    {
                        ++i;
                        continue;
                    }

                    break;
                }

                if (last.empty())
                {
                    return false;
                }

                // Most parenthesized expressions fail on shape alone: look the name up last.
                std::size_t tail = i;
                while (tail < end && (Is(tail, "*") || Is(tail, "&") || Is(tail, "&&") ||
                                      Is(tail, "const") || Is(tail, "volatile")))
                {
                    ++tail;
                }

                return tail == end && ClassOf(last) == kNameType;
            }

            qualifiers();
            while (i < end &&
                   (Is(i, "*") || Is(i, "&") || Is(i, "&&") || Is(i, "const") || Is(i, "volatile")))
            {
                ++i;
            }

            return i == end;
        }

        bool StartsCastOperand(std::size_t pos, std::size_t end) const
        {
            if (pos >= end)
            {
                return false;
            }

            const auto& token = m_tree.Tokens()[m_sig[pos]];
            switch (token.kind)
            {
                case TokenKind::Number:
                case TokenKind::StringLiteral:
                case TokenKind::CharacterLiteral:
                case TokenKind::RawStringLiteral:
                    return true;
                case TokenKind::Identifier: {
                    const auto text = Text(pos);
                    return text != "and" && text != "or" && text != "xor" && text != "and_eq" &&
                           text != "or_eq" && text != "xor_eq" && text != "not_eq" &&
                           text != "bitand" && text != "bitor" && text != "const" &&
                           text != "volatile" && text != "noexcept" && text != "override" &&
                           text != "final" && text != "requires";
                }
                default:
                    break;
            }

            const auto text = Text(pos);
            return text == "(" || text == "!" || text == "~" || text == "-" || text == "+" ||
                   text == "*" || text == "&" || text == "++" || text == "--" || text == "::";
        }

        void BeginItem(std::size_t pos)
        {
            m_item_names_hash = m_names_hash;
            m_item_open       = true;
            m_item_first_sig  = pos;
            m_item_node_begin = m_tree.m_nodes_soa.size();
            m_item_diag_begin = m_tree.m_diagnostics.size();
        }

        // Closes the item opened at BeginItem: it ends before sig index `pos`.
        // `complete` is false when the scope bailed out instead of consuming it.
        void FinishItem(std::size_t pos, bool complete)
        {
            if (!m_item_open)
            {
                return;
            }

            m_item_open = false;
            TopLevelItem item {};
            item.node_begin  = static_cast<std::uint32_t>(m_item_node_begin);
            item.node_end    = static_cast<std::uint32_t>(m_tree.m_nodes_soa.size());
            item.diag_begin  = static_cast<std::uint32_t>(m_item_diag_begin);
            item.diag_end    = static_cast<std::uint32_t>(m_tree.m_diagnostics.size());
            item.sig_count   = static_cast<std::uint32_t>(pos - m_item_first_sig);
            item.names_hash  = m_item_names_hash;
            item.first_token = m_sig[m_item_first_sig];
            item.token_end   = pos > m_item_first_sig ? m_sig[pos - 1] + 1 : item.first_token;
            bool reusable =
                complete && pos > m_item_first_sig && item.node_end > item.node_begin &&
                !IsDirective(m_item_first_sig) && BracketsClosedWithin(m_item_first_sig, pos);
            // Error recovery may scan arbitrarily far ahead, so only items that parsed
            // cleanly and ended on `;` or `}` are known to depend on nothing beyond them.
            reusable = reusable && item.diag_end == item.diag_begin && pos > m_item_first_sig &&
                       (Is(pos - 1, ";") || Is(pos - 1, "}"));
            for (auto n = item.node_begin; reusable && n < item.node_end; ++n)
            {
                const GrammarKind   kind = static_cast<GrammarKind>(m_tree.m_nodes_soa.kind[n]);
                const std::uint32_t first_token = m_tree.m_nodes_soa.first_token[n];
                const std::uint32_t token_count = m_tree.m_nodes_soa.token_count[n];
                const std::uint32_t parent      = m_tree.m_nodes_soa.parent[n];
                reusable = kind != GrammarKind::Error && kind != GrammarKind::ErrorExpression &&
                           first_token >= item.first_token && first_token < item.token_end &&
                           first_token + token_count <= item.token_end &&
                           (parent == ParseTree::RootNode ||
                            (parent >= item.node_begin && parent < item.node_end));
            }

            item.reusable = reusable;
            m_tree.m_items.push_back(item);
        }

        // Copies the matching item of the previous tree when the edit provably
        // cannot have changed how it parses; advances `pos` past it.
        bool TryReuseItem(std::size_t& pos, std::size_t end)
        {
            if (m_reuse == nullptr || m_reuse->previous == nullptr)
            {
                return false;
            }

            const ParseTree&     prev       = *m_reuse->previous;
            const auto&          items      = prev.m_items;
            const std::size_t    edit       = m_reuse->offset;
            const std::size_t    old_end    = edit + m_reuse->old_length;
            const std::size_t    new_end    = edit + m_reuse->new_length;
            const std::ptrdiff_t shift      = static_cast<std::ptrdiff_t>(m_reuse->new_length) -
                                              static_cast<std::ptrdiff_t>(m_reuse->old_length);
            const std::uint32_t  token      = m_sig[pos];
            const std::size_t    offset     = m_tree.Tokens()[token].offset;
            bool                 after      = false;
            std::size_t          old_offset = offset;
            if (offset >= new_end)
            {
                after      = true;
                old_offset = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(offset) - shift);
            }
            else if (offset >= edit)
            {
                return false;
            }

            const auto found =
                std::lower_bound(items.begin(), items.end(), old_offset,
                                 [&prev](const TopLevelItem& item, std::size_t value) {
                                     return prev.Tokens()[item.first_token].offset < value;
                                 });
            if (found == items.end() || prev.Tokens()[found->first_token].offset != old_offset ||
                !found->reusable)
            {
                return false;
            }

            const TopLevelItem& item = *found;
            // Read under other known names, the same tokens may parse differently.
            if (item.names_hash != m_names_hash)
            {
                return false;
            }

            if (after)
            {
                if (old_offset < old_end)
                {
                    return false;
                }
            }
            else
            {
                // The item must end before the edit, and so must the token after it:
                // the parser may peek one token past an item to decide where it ends.
                const auto& last = prev.Tokens()[item.token_end - 1];
                const auto  next = found + 1;
                if (static_cast<std::size_t>(last.offset) + last.length >= edit ||
                    next == items.end() || prev.Tokens()[next->first_token].offset >= edit)
                {
                    return false;
                }
            }

            const std::size_t token_count = item.token_end - item.first_token;
            const std::size_t after_sig   = pos + item.sig_count;
            if (after_sig > end || token + token_count > m_tree.Tokens().size() ||
                m_sig[after_sig - 1] != token + (item.token_end - 1 - item.first_token) ||
                (after_sig < m_sig.size() && m_sig[after_sig] < token + token_count) ||
                !BracketsClosedWithin(pos, after_sig))
            {
                return false;
            }

            const std::ptrdiff_t byte_shift = after ? shift : 0;
            const std::ptrdiff_t token_shift =
                static_cast<std::ptrdiff_t>(token) - static_cast<std::ptrdiff_t>(item.first_token);
            const auto& last_new = m_tree.Tokens()[token + token_count - 1];
            const auto& last_old = prev.Tokens()[item.token_end - 1];
            if (static_cast<std::ptrdiff_t>(last_new.offset) !=
                    static_cast<std::ptrdiff_t>(last_old.offset) + byte_shift ||
                last_new.length != last_old.length)
            {
                return false;
            }

            const std::uint32_t node_base  = static_cast<std::uint32_t>(m_tree.m_nodes_soa.size());
            auto&               nodes      = m_tree.m_nodes_soa;
            const auto&         old_nodes  = prev.m_nodes_soa;
            const std::size_t   node_count = item.node_end - item.node_begin;
            // Copy unchanged columns in contiguous batches, then remap only the
            // version-dependent indices. One resize replaces five push_backs per node.
            nodes.resize(node_base + node_count);
            std::copy_n(old_nodes.kind.begin() + item.node_begin, node_count,
                        nodes.kind.begin() + node_base);
            std::copy_n(old_nodes.token_count.begin() + item.node_begin, node_count,
                        nodes.token_count.begin() + node_base);
            for (auto n = item.node_begin; n < item.node_end; ++n)
            {
                const std::uint32_t first_token = static_cast<std::uint32_t>(
                    static_cast<std::ptrdiff_t>(prev.m_nodes_soa.first_token[n]) + token_shift);
                std::uint32_t parent = prev.m_nodes_soa.parent[n];
                if (parent != ParseTree::RootNode)
                {
                    parent = parent - item.node_begin + node_base;
                }

                const std::uint32_t subtree_end =
                    prev.m_nodes_soa.subtree_end[n] - item.node_begin + node_base;

                const auto target         = node_base + n - item.node_begin;
                nodes.first_token[target] = first_token;
                nodes.parent[target]      = parent;
                nodes.subtree_end[target] = subtree_end;
            }

            m_tree.m_nodes_aos_dirty = true;
            // The copied nodes declare what a fresh parse of them would have declared.
            RegisterNodes(node_base, m_tree.m_nodes_soa.size());
            m_registered_upto = m_tree.m_nodes_soa.size();

            const std::uint32_t diag_base = static_cast<std::uint32_t>(m_tree.m_diagnostics.size());
            for (auto d = item.diag_begin; d < item.diag_end; ++d)
            {
                GrammarDiagnostic diagnostic = prev.m_diagnostics[d];
                diagnostic.offset            = static_cast<std::size_t>(
                    static_cast<std::ptrdiff_t>(diagnostic.offset) + byte_shift);
                m_tree.m_diagnostics.push_back(std::move(diagnostic));
            }

            TopLevelItem copy = item;
            copy.first_token  = token;
            copy.token_end    = static_cast<std::uint32_t>(token + token_count);
            copy.node_begin   = node_base;
            copy.node_end     = static_cast<std::uint32_t>(m_tree.m_nodes_soa.size());
            copy.diag_begin   = diag_base;
            copy.diag_end     = static_cast<std::uint32_t>(m_tree.m_diagnostics.size());
            m_tree.m_items.push_back(copy);
            ++m_tree.m_reused_items;
            pos = after_sig;
            return true;
        }

        void ParseScope(std::size_t begin, std::size_t end, std::size_t parent, bool member_scope)
        {
            const bool top_level = parent == ParseTree::RootNode;
            auto       pos       = begin;
            while (pos < end)
            {
                if (m_tree.m_cancelled || m_stop.stop_requested())
                {
                    m_tree.m_cancelled = true;
                    return;
                }

                RegisterPending();
                if (top_level)
                {
                    FinishItem(pos, true);
                    if (TryReuseItem(pos, end))
                    {
                        continue;
                    }

                    BeginItem(pos);
                }

                const auto start = pos;
                if (IsDirective(pos))
                {
                    pos = SkipDirective(pos, end, parent);
                    continue;
                }

                // `public:` / `private:` / `protected:` is a label, not a
                // declaration: parsed as one it would swallow the next member
                // as a bit-field width and hide it from navigation/completion.
                if (member_scope && pos + 1 < end && Is(pos + 1, ":") &&
                    (Is(pos, "public") || Is(pos, "private") || Is(pos, "protected")))
                {
                    Add(GrammarKind::AccessSpecifier, pos, pos + kTwo, parent);
                    pos += kTwo;
                    continue;
                }

                auto exported_head = pos;
                if (Is(exported_head, "export"))
                {
                    ++exported_head;
                }

                if (Is(exported_head, "module") || Is(exported_head, "import"))
                {
                    const auto semi = FindSemicolon(pos, end);
                    if (semi < end)
                    {
                        const auto kind = Is(exported_head, "module")
                                              ? GrammarKind::ModuleDeclaration
                                              : GrammarKind::ImportDeclaration;
                        Add(kind, pos, semi + 1, parent);
                        pos = semi + 1;
                        continue;
                    }
                }

                const auto instantiation_head =
                    Is(pos, "extern") && Is(pos + 1, "template") ? pos + 1 : pos;
                if (Is(instantiation_head, "template") &&
                    !Is(instantiation_head + 1, "<"))
                {
                    const auto semi = FindSemicolon(instantiation_head, end);
                    if (semi < end)
                    {
                        Add(GrammarKind::Declaration, pos, semi + 1, parent);
                        pos = semi + 1;
                        continue;
                    }
                }

                auto declaration_start = pos;
                if (Is(declaration_start, "export") &&
                    Is(declaration_start + 1, "template"))
                {
                    ++declaration_start;
                }

                const bool is_template = Is(declaration_start, "template") &&
                                         Is(declaration_start + 1, "<");
                bool malformed_template_head = false;
                while (Is(declaration_start, "template") &&
                       Is(declaration_start + 1, "<"))
                {
                    const auto open = declaration_start + 1;
                    std::size_t depth = 1;
                    auto angle = open + 1;
                    for (; angle < end; ++angle)
                    {
                        if ((Is(angle, "(") || Is(angle, "[") || Is(angle, "{")) &&
                            m_match[angle] != Invalid && m_match[angle] > angle)
                        {
                            angle = m_match[angle];
                            continue;
                        }

                        if (Is(angle, "<"))
                        {
                            ++depth;
                        }
                        else if (Is(angle, ">"))
                        {
                            if (--depth == 0)
                            {
                                ++angle;
                                break;
                            }
                        }
                        else if (Is(angle, ">>"))
                        {
                            depth = depth > 1 ? depth - kTwo : 0;
                            if (depth == 0)
                            {
                                ++angle;
                                break;
                            }
                        }
                    }

                    if (depth != 0)
                    {
                        m_tree.m_diagnostics.push_back(
                            { m_tree.Tokens()[m_sig[open]].offset,
                              "expected '>' to close template parameter list" });
                        const auto semi = FindSemicolon(pos, end);
                        const auto recovery = semi < end ? semi + 1 : end;
                        Add(GrammarKind::Error, pos, recovery, parent);
                        pos = recovery;
                        malformed_template_head = true;
                        break;
                    }

                    declaration_start = angle;
                }

                if (malformed_template_head)
                {
                    if (pos >= end)
                    {
                        break;
                    }
                    continue;
                }

                if (Is(pos, "static_assert"))
                {
                    const auto next = ParseStaticAssert(pos, end, parent);
                    if (next != pos)
                    {
                        pos = next;
                        continue;
                    }
                }

                if (is_template && declaration_start >= end)
                {
                    m_tree.m_diagnostics.push_back(
                        { m_tree.Tokens()[m_sig[start]].offset,
                          "expected declaration after template parameter list" });
                    Add(GrammarKind::Error, start, end, parent);
                    break;
                }

                std::size_t leading_req     = Invalid;
                std::size_t leading_req_end = Invalid;
                if (Is(declaration_start, "requires") &&
                    !IsRequiresExpressionAt(declaration_start, end))
                {
                    const auto split = FindLeadingRequiresSplit(declaration_start, end);
                    if (split != Invalid && split > declaration_start)
                    {
                        leading_req       = declaration_start;
                        leading_req_end   = split;
                        declaration_start = split;
                    }
                }

                // Check for language linkage specification: extern "C" { ... } or extern "C++" {
                // ... }
                bool language_linkage = false;
                if (Is(declaration_start, "extern") && declaration_start + 1 < end)
                {
                    const auto next = declaration_start + 1;
                    if (m_tree.Tokens()[m_sig[next]].kind == TokenKind::StringLiteral)
                    {
                        // extern "C" or extern "C++" etc.
                        if (next + 1 < end && Is(next + 1, "{"))
                        {
                            language_linkage = true;
                        }
                    }
                }

                const bool namespace_decl =
                    Is(declaration_start, "namespace") ||
                    (Is(declaration_start, "inline") && declaration_start + 1 < end &&
                     Is(declaration_start + 1, "namespace"));
                const bool record_decl =
                    Is(declaration_start, "class") || Is(declaration_start, "struct") ||
                    Is(declaration_start, "union") || Is(declaration_start, "enum");
                std::size_t brace = end;
                for (auto i = declaration_start; i < end;)
                {
                    if (Is(i, ";"))
                    {
                        break;
                    }

                    if (Is(i, "{"))
                    {
                        brace = i;
                        break;
                    }

                    if ((Is(i, "(") || Is(i, "[")) && m_match[i] != Invalid && m_match[i] > i)
                    {
                        i = m_match[i] + 1;
                    }
                    else
                    {
                        ++i;
                    }
                }

                if (Is(declaration_start, "concept"))
                {
                    const auto semi = FindSemicolon(declaration_start, end);
                    if (semi < end)
                    {
                        const auto item_parent =
                            is_template
                                ? Add(GrammarKind::TemplateDeclaration, start, semi + 1, parent)
                                : parent;
                        if (leading_req != Invalid)
                        {
                            ParseRequiresConstraint(leading_req, leading_req_end, item_parent);
                        }

                        const auto concept_node = Add(GrammarKind::ConceptDefinition,
                                                      declaration_start, semi + 1, item_parent);
                        for (auto i = declaration_start + 1; i < semi; ++i)
                            if (Is(i, "="))
                            {
                                ParseExpression(i + 1, semi, concept_node);
                                break;
                            }

                        pos = semi + 1;
                        continue;
                    }
                }

                if (brace < end)
                {
                    const bool closed              = m_match[brace] != Invalid;
                    const auto close               = closed ? m_match[brace] : end;
                    bool       has_function_parens = false;
                    for (auto i = declaration_start; i < brace; ++i)
                    {
                        if (Is(i, "(") && m_match[i] != Invalid && m_match[i] < brace)
                        {
                            has_function_parens = true;
                        }
                    }

                    const bool function_body =
                        has_function_parens && !namespace_decl && !record_decl;
                    if (!namespace_decl && !record_decl && !function_body && !language_linkage)
                    {
                        const auto semi = FindSemicolon(brace + (closed ? 1 : 0), end);
                        if (semi < end)
                        {
                            const auto wrapper =
                                is_template
                                    ? Add(GrammarKind::TemplateDeclaration, start, semi + 1, parent)
                                    : parent;
                            if (leading_req != Invalid)
                            {
                                ParseRequiresConstraint(leading_req, leading_req_end, wrapper);
                            }

                            const auto declaration =
                                Add(GrammarKind::Declaration, declaration_start, semi + 1, wrapper);
                            AddDeclarationDetails(declaration_start, semi, declaration);
                            pos = semi + 1;
                            continue;
                        }
                    }

                    const auto node_kind =
                        language_linkage ? GrammarKind::LanguageLinkageSpec
                        : namespace_decl ? GrammarKind::NamespaceDefinition
                        : record_decl    ? GrammarKind::RecordDefinition
                        : function_body  ? GrammarKind::FunctionDefinition
                                         : GrammarKind::Error;
                    const auto item_end = closed ? close + 1 : close;
                    const auto item_parent =
                        is_template ? Add(GrammarKind::TemplateDeclaration, start, item_end, parent)
                                    : parent;
                    if (leading_req != Invalid && node_kind != GrammarKind::NamespaceDefinition &&
                        node_kind != GrammarKind::RecordDefinition)
                    {
                        ParseRequiresConstraint(leading_req, leading_req_end, item_parent);
                    }

                    const auto node = Add(node_kind, declaration_start, item_end, item_parent);
                    if (node_kind == GrammarKind::FunctionDefinition)
                    {
                        // Full declarator parsing: return type, declarator with FunctionSuffix
                        // (parameters, noexcept, trailing return, requires-clause).
                        AddTypeAndDeclarator(declaration_start, brace, node);
                    }

                    if (node_kind == GrammarKind::LanguageLinkageSpec)
                    {
                        // Parse the contents of the language linkage specification (like a
                        // namespace)
                        ParseScope(brace + 1, close, node, false);

                        if (!closed)
                        {
                            m_tree.m_diagnostics.push_back(
                                { m_tree.Tokens()[m_sig[brace]].offset,
                                  "expected '}' to close language linkage specification" });
                        }
                    }
                    else if (node_kind == GrammarKind::NamespaceDefinition ||
                             node_kind == GrammarKind::RecordDefinition)
                    {
                        if (node_kind == GrammarKind::RecordDefinition &&
                            Is(declaration_start, "enum"))
                        {
                            ParseEnumerators(brace + 1, close, node);
                        }
                        else
                        {
                            ParseScope(brace + 1, close, node,
                                       node_kind == GrammarKind::RecordDefinition);
                        }

                        if (!closed)
                        {
                            m_tree.m_diagnostics.push_back({ m_tree.Tokens()[m_sig[brace]].offset,
                                                             "expected '}' to close definition" });
                        }
                    }
                    else if (node_kind == GrammarKind::FunctionDefinition)
                    {
                        auto body = brace;
                        ParseCompound(body, closed ? close + 1 : close, node);
                    }

                    pos = closed ? close + 1 : end;
                    if (Is(pos, ";"))
                    {
                        ++pos;
                    }

                    continue;
                }

                const auto semi = FindSemicolon(pos, end);
                if (semi < end)
                {
                    const auto item_parent =
                        is_template ? Add(GrammarKind::TemplateDeclaration, start, semi + 1, parent)
                                    : parent;
                    if (leading_req != Invalid)
                    {
                        ParseRequiresConstraint(leading_req, leading_req_end, item_parent);
                    }

                    bool function_declaration = false;
                    for (auto i = declaration_start; i < semi; ++i)
                    {
                        if (Is(i, "(") && m_match[i] != Invalid && m_match[i] < semi)
                        {
                            function_declaration = true;
                        }
                    }

                    if (function_declaration)
                    {
                        const auto function = Add(GrammarKind::FunctionDeclaration,
                                                  declaration_start, semi + 1, item_parent);
                        const auto consumed =
                            AddTypeAndDeclarator(declaration_start, semi, function);
                        // The declarator parse stopped short of the `;` while a new
                        // declaration clearly begins: the function lost its `;`.
                        if (consumed > declaration_start && consumed < semi &&
                            LooksLikeDeclarationStart(consumed, semi))
                        {
                            m_tree.m_diagnostics.push_back(
                                { m_tree.Tokens()[m_sig[consumed]].offset,
                                  std::string("expected ';' before '") +
                                      std::string(Text(consumed)) + "'" });
                        }
                    }
                    else
                    {
                        const auto declaration_kind =
                            Is(declaration_start, "concept") ? GrammarKind::ConceptDefinition
                            : Is(declaration_start, "using") ? GrammarKind::UsingDeclaration
                                                             : GrammarKind::Declaration;
                        const auto declaration =
                            Add(declaration_kind, declaration_start, semi + 1, item_parent);
                        if (declaration_kind == GrammarKind::ConceptDefinition)
                        {
                            for (auto i = declaration_start + 1; i < semi; ++i)
                                if (Is(i, "="))
                                {
                                    ParseExpression(i + 1, semi, declaration);
                                    break;
                                }
                        }
                        else if (declaration_kind != GrammarKind::UsingDeclaration)
                        {
                            AddDeclarationDetails(declaration_start, semi, declaration);
                        }
                        else
                        {
                            for (auto i = declaration_start + 1; i < semi; ++i)
                            {
                                if (Is(i, "=") && Is(i + 1, "[") && Is(i + 2, ":"))
                                {
                                    ParseExpression(i + 1, semi, declaration);
                                    break;
                                }
                            }
                        }
                    }

                    pos = semi + 1;
                    continue;
                }

                m_tree.m_diagnostics.push_back(
                    { m_tree.Tokens()[m_sig[start]].offset,
                      "expected ';' or definition body before end of scope" });
                Add(GrammarKind::Error, start, end, parent);
                break;
            }

            RegisterPending();
            if (top_level)
            {
                FinishItem(pos, pos >= end);
            }
        }
    };

    namespace detail
    {

        void ParseWithGrammar(ParseTree&                    tree,
                              const PreprocessorResult&     preprocessing,
                              std::stop_token               stop,
                              const Preprocessor::MacroMap* macros,
                              const ParseReuse*             reuse,
                              const TypeNameOracle*         type_names)
        {
            GrammarParser parser(tree, preprocessing, std::move(stop), macros, reuse, type_names);
            parser.Run();
        }

    } // namespace detail

} // namespace heimdall
