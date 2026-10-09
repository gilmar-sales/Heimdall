#include <Heimdall/Lexer.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace heimdall
{

    namespace
    {

        namespace CharBits
        {
            constexpr std::uint8_t kSpace      = 1 << 0;
            constexpr std::uint8_t kIdentStart = 2;
            constexpr std::uint8_t kIdentCont  = 4;
            constexpr std::uint8_t kDigit      = 8;
            constexpr std::uint8_t kHex        = 16;
            constexpr std::uint8_t kPunct      = 32;
        } // namespace CharBits

        constexpr std::size_t kByteValueCount      = 256;
        constexpr std::size_t kNonAsciiThreshold   = 0x80;
        constexpr std::size_t kTwoCharPunctLen     = 2;
        constexpr std::size_t kThreeCharPunctLen   = 3;
        constexpr std::size_t kFourCharPunctLen    = 4;
        constexpr std::size_t kMaxDelimLen         = 16;
        constexpr std::size_t kEscapedPairLen      = 2;
        constexpr std::size_t kRawStringSuffixLen  = 2;
        constexpr std::size_t kCommentDelimLen     = 2;
        constexpr std::size_t kPrefixOffset2       = 2;
        constexpr std::size_t kPrefixOffset3       = 3;
        constexpr std::size_t kApproxBytesPerToken = 4;
        constexpr std::size_t kHalfDivisor         = 2;

        constexpr std::array<std::uint8_t, kByteValueCount> BuildCharClass()
        {
            std::array<std::uint8_t, kByteValueCount> table {};
            for (std::size_t i = 0; i < kByteValueCount; ++i)
            {
                const char   c    = static_cast<char>(i);
                std::uint8_t bits = 0;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
                {
                    bits |= CharBits::kSpace;
                }

                const bool ident_start = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                         c == '_' || i >= kNonAsciiThreshold;
                if (ident_start)
                {
                    bits |= CharBits::kIdentStart | CharBits::kIdentCont;
                }

                if (c >= '0' && c <= '9')
                {
                    bits |= CharBits::kIdentCont | CharBits::kDigit | CharBits::kHex;
                }

                if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                {
                    bits |= CharBits::kHex;
                }

                constexpr std::string_view punct = "{}[]#()<>%:;.?*+-/^&|~!=,\\\"'";
                if (punct.find(c) != std::string_view::npos)
                {
                    bits |= CharBits::kPunct;
                }

                table[i] = bits;
            }

            return table;
        }

        constexpr std::array<std::uint8_t, kByteValueCount> kCharClass = BuildCharClass();

        constexpr bool IsSpace(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kSpace) != 0;
        }

        constexpr bool IsDigit(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kDigit) != 0;
        }

        constexpr bool IsIdentStart(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kIdentStart) != 0;
        }

        constexpr bool IsIdentContinue(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kIdentCont) != 0;
        }

        constexpr bool IsHexDigit(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kHex) != 0;
        }

        std::size_t ScanQuoted(std::string_view s, std::size_t i, char quote)
        {
            ++i;
            while (i < s.size())
            {
                if (s[i] == '\n' || s[i] == '\r')
                {
                    break;
                }

                if (s[i] == '\\')
                {
                    i += i + 1 < s.size() ? kEscapedPairLen : 1;
                }
                else if (s[i++] == quote)
                {
                    break;
                }
            }

            return i;
        }

        std::size_t ScanRawString(std::string_view s, std::size_t quote)
        {
            const std::size_t delim_start = quote + 1;
            const std::size_t open        = s.find('(', delim_start);
            if (open == std::string_view::npos || open - delim_start > kMaxDelimLen)
            {
                return ScanQuoted(s, quote, '"');
            }

            const std::string_view delimiter = s.substr(delim_start, open - delim_start);
            std::size_t            pos       = open + 1;
            while ((pos = s.find(')', pos)) != std::string_view::npos)
            {
                if (pos + delimiter.size() + 1 < s.size() && s[pos + 1 + delimiter.size()] == '"' &&
                    s.substr(pos + 1, delimiter.size()) == delimiter)
                {
                    return pos + delimiter.size() + kRawStringSuffixLen;
                }

                ++pos;
            }

            return s.size();
        }

        bool IsPunctuatorStart(char c)
        {
            return (kCharClass[static_cast<unsigned char>(c)] & CharBits::kPunct) != 0;
        }

        std::size_t PunctuatorLength(std::string_view source, std::size_t offset)
        {
            const std::string_view tail = source.substr(offset);
            auto is = [tail](std::string_view punctuator) { return tail.starts_with(punctuator); };
            switch (source[offset])
            {
                case '%':
                    if (is("%:%:"))
                    {
                        return kFourCharPunctLen;
                    }

                    if (is("%=") || is("%>") || is("%:"))
                    {
                        return kTwoCharPunctLen;
                    }

                    break;
                case '#':
                    if (is("##"))
                    {
                        return kTwoCharPunctLen;
                    }
                    break;
                case ':':
                    if (is("::") || is(":>"))
                    {
                        return kTwoCharPunctLen;
                    }
                    break;
                case '.':
                    if (is("...") || is(".*"))
                    {
                        return is("...") ? kThreeCharPunctLen : kTwoCharPunctLen;
                    }
                    break;
                case '-':
                    if (is("->*"))
                    {
                        return kThreeCharPunctLen;
                    }

                    if (is("->") || is("--") || is("-="))
                    {
                        return kTwoCharPunctLen;
                    }

                    break;
                case '<':
                    if (is("<=>"))
                    {
                        return kThreeCharPunctLen;
                    }

                    if (is("<<="))
                    {
                        return kThreeCharPunctLen;
                    }

                    if (is("<<") || is("<=") || is("<:") || is("<%"))
                    {
                        return kTwoCharPunctLen;
                    }

                    break;
                case '>':
                    if (is(">>="))
                    {
                        return kThreeCharPunctLen;
                    }

                    if (is(">>") || is(">="))
                    {
                        return kTwoCharPunctLen;
                    }

                    break;
                case '+':
                    if (is("++") || is("+="))
                    {
                        return kTwoCharPunctLen;
                    }
                    break;
                case '*':
                    if (is("*="))
                    {
                        return kTwoCharPunctLen;
                    }
                    break;
                case '/':
                    if (is("/="))
                    {
                        return kTwoCharPunctLen;
                    }
                    break;
                case '&':
                    if (is("&&") || is("&="))
                    {
                        return kTwoCharPunctLen;
                    }
                    break;
                case '|':
                    if (is("||") || is("|="))
                    {
                        return kTwoCharPunctLen;
                    }
                    break;
                case '^':
                    if (is("^="))
                    {
                        return kTwoCharPunctLen;
                    }
                    break;
                case '=':
                    if (is("=="))
                    {
                        return kTwoCharPunctLen;
                    }
                    break;
                case '!':
                    if (is("!="))
                    {
                        return kTwoCharPunctLen;
                    }
                    break;
                default:
                    break;
            }

            return 1;
        }

    } // namespace

    std::vector<Token> Lexer::Lex() const
    {
        std::vector<Token> tokens;
        // ~1 token per 4 source bytes on typical C++ (was size/3: over-reserved
        // ~8 B of Token storage per source byte up front).
        tokens.reserve(m_source.size() / kApproxBytesPerToken + 1);

        std::size_t i = 0;
        while (i < m_source.size())
        {
            const Token token = ScanToken(i);
            tokens.push_back(token);
            i += token.length;
        }

        return tokens;
    }

    Token Lexer::ScanToken(std::size_t start) const
    {
        std::size_t i = start;
        {
            const char c    = m_source[i];
            TokenKind  kind = TokenKind::Unknown;

            if (IsSpace(c))
            {
                kind = TokenKind::Whitespace;
                do
                {
                    ++i;
                } while (i < m_source.size() && IsSpace(m_source[i]));
            }
            else if (c == '/' && i + 1 < m_source.size() && m_source[i + 1] == '/')
            {
                kind = TokenKind::LineComment;
                i += kCommentDelimLen;
                while (i < m_source.size() && m_source[i] != '\n' && m_source[i] != '\r')
                {
                    ++i;
                }
            }
            else if (c == '/' && i + 1 < m_source.size() && m_source[i + 1] == '*')
            {
                kind = TokenKind::BlockComment;
                i += kCommentDelimLen;
                while (i + 1 < m_source.size() && !(m_source[i] == '*' && m_source[i + 1] == '/'))
                {
                    ++i;
                }

                i = i + 1 < m_source.size() ? i + kCommentDelimLen : m_source.size();
            }
            else
            {
                std::size_t quote     = i;
                std::size_t raw_quote = std::string_view::npos;
                if (c == 'R' && i + 1 < m_source.size() && m_source[i + 1] == '"')
                {
                    raw_quote = i + 1;
                }
                else if ((c == 'u' || c == 'U' || c == 'L') &&
                         i + kPrefixOffset2 < m_source.size() && m_source[i + 1] == 'R' &&
                         m_source[i + kPrefixOffset2] == '"')
                {
                    raw_quote = i + kPrefixOffset2;
                }
                else if (c == 'u' && i + kPrefixOffset3 < m_source.size() &&
                         m_source[i + 1] == '8' && m_source[i + kPrefixOffset2] == 'R' &&
                         m_source[i + kPrefixOffset3] == '"')
                {
                    raw_quote = i + kPrefixOffset3;
                }

                if (raw_quote != std::string_view::npos)
                {
                    kind = TokenKind::RawStringLiteral;
                    i    = ScanRawString(m_source, raw_quote);
                }
                else if ((c == 'u' || c == 'U' || c == 'L') && i + 1 < m_source.size() &&
                         (m_source[i + 1] == '"' || m_source[i + 1] == '\''))
                {
                    quote = i + 1;
                    kind  = m_source[quote] == '"' ? TokenKind::StringLiteral
                                                   : TokenKind::CharacterLiteral;
                    i     = ScanQuoted(m_source, quote, m_source[quote]);
                }
                else if (c == 'u' && i + kPrefixOffset2 < m_source.size() &&
                         m_source[i + 1] == '8' &&
                         (m_source[i + kPrefixOffset2] == '"' ||
                          m_source[i + kPrefixOffset2] == '\''))
                {
                    quote = i + kPrefixOffset2;
                    kind  = m_source[quote] == '"' ? TokenKind::StringLiteral
                                                   : TokenKind::CharacterLiteral;
                    i     = ScanQuoted(m_source, quote, m_source[quote]);
                }
                else if (c == '"' || c == '\'')
                {
                    kind = c == '"' ? TokenKind::StringLiteral : TokenKind::CharacterLiteral;
                    i    = ScanQuoted(m_source, i, c);
                }
                else if (IsIdentStart(c))
                {
                    kind = TokenKind::Identifier;
                    do
                    {
                        ++i;
                    } while (i < m_source.size() && IsIdentContinue(m_source[i]));
                }
                else if (IsDigit(c) ||
                         (c == '.' && i + 1 < m_source.size() && IsDigit(m_source[i + 1])))
                {
                    kind = TokenKind::Number;
                    ++i;
                    while (i < m_source.size())
                    {
                        const char n = m_source[i];
                        if (IsIdentContinue(n) || n == '\'' || n == '.')
                        {
                            ++i;
                        }
                        else if ((n == '+' || n == '-') && i > start &&
                                 (m_source[i - 1] == 'e' || m_source[i - 1] == 'E' ||
                                  m_source[i - 1] == 'p' || m_source[i - 1] == 'P'))
                        {
                            ++i;
                        }
                        else if (n == 'x' || n == 'X')
                        {
                            ++i;
                        }
                        else if (IsHexDigit(n) &&
                                 (m_source[start] == '0' && start + 1 < m_source.size() &&
                                  (m_source[start + 1] == 'x' || m_source[start + 1] == 'X')))
                        {
                            ++i;
                        }
                        else
                        {
                            break;
                        }
                    }
                }
                else if (IsPunctuatorStart(c))
                {
                    kind = TokenKind::Punctuation;
                    i += PunctuatorLength(m_source, i);
                }
                else
                {
                    ++i;
                }
            }

            Tok tok = Tok::None;
            if (kind == TokenKind::Punctuation)
            {
                tok = i - start == 1 ? SingleCharTok(c)
                                     : LookupTok(m_source.substr(start, i - start));
            }
            else if (kind == TokenKind::Identifier &&
                     MayBeKeyword(m_source.substr(start, i - start)))
            {
                tok = LookupTok(m_source.substr(start, i - start));
            }

            return { kind, tok, static_cast<std::uint32_t>(start),
                     static_cast<std::uint32_t>(i - start) };
        }
    }

    void Lexer::Relex(std::vector<Token>& tokens, const TextEdit& edit) const
    {
        constexpr std::size_t kLookbehind = 32;
        const std::size_t     old_end     = edit.offset + edit.old_length;
        const std::size_t     new_end     = edit.offset + edit.new_length;
        const std::ptrdiff_t  delta       = static_cast<std::ptrdiff_t>(edit.new_length) -
                                            static_cast<std::ptrdiff_t>(edit.old_length);

        // First token that ends at or after the window start, minus one token of
        // lookbehind: a token can only be influenced by text that follows it, so
        // anything ending well before the edit keeps its old lexing.
        const std::size_t window = edit.offset > kLookbehind ? edit.offset - kLookbehind : 0;
        std::size_t       lo     = 0;
        std::size_t       hi     = tokens.size();
        while (lo < hi)
        {
            const std::size_t mid = (lo + hi) / kHalfDivisor;
            if (static_cast<std::size_t>(tokens[mid].offset) + tokens[mid].length < window)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }

        const std::size_t first = lo > 0 ? lo - 1 : 0;
        std::size_t       scan  = first < tokens.size() ? tokens[first].offset : 0;
        // Old tokens whose start lies at or after the replaced range, in old coordinates.
        std::size_t        resume = first;
        std::vector<Token> middle;
        std::size_t        suffix = tokens.size();
        while (scan < m_source.size())
        {
            if (scan >= new_end)
            {
                const std::size_t old_scan = scan - delta;
                while (resume < tokens.size() && tokens[resume].offset < old_scan)
                {
                    ++resume;
                }

                if (resume < tokens.size() && tokens[resume].offset == old_scan)
                {
                    const Token fresh = ScanToken(scan);
                    if (fresh.kind == tokens[resume].kind && fresh.length == tokens[resume].length)
                    {
                        suffix = resume;
                        break;
                    }

                    middle.push_back(fresh);
                    scan += fresh.length;
                    continue;
                }
            }

            const Token fresh = ScanToken(scan);
            middle.push_back(fresh);
            scan += fresh.length;
        }

        if (suffix == tokens.size())
        {
            tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(first), tokens.end());
            tokens.insert(tokens.end(), middle.begin(), middle.end());
            return;
        }

        const std::size_t removed    = suffix - first;
        const std::size_t tail_start = first + middle.size();
        if (middle.size() < removed)
        {
            std::copy(middle.begin(), middle.end(),
                      tokens.begin() + static_cast<std::ptrdiff_t>(first));
            tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(tail_start),
                         tokens.begin() + static_cast<std::ptrdiff_t>(suffix));
        }
        else
        {
            std::copy(middle.begin(), middle.begin() + static_cast<std::ptrdiff_t>(removed),
                      tokens.begin() + static_cast<std::ptrdiff_t>(first));
            tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(suffix),
                          middle.begin() + static_cast<std::ptrdiff_t>(removed), middle.end());
        }

        for (std::size_t t = tail_start; t < tokens.size(); ++t)
        {
            tokens[t].offset =
                static_cast<std::uint32_t>(static_cast<std::ptrdiff_t>(tokens[t].offset) + delta);
        }
    }

    std::string_view Lexer::Text(const Token& token) const noexcept
    {
        return m_source.substr(token.offset, token.length);
    }

} // namespace heimdall
