#include <Heimdall/Preprocessor.hpp>

#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>

namespace heimdall
{

    namespace
    {

        struct ConditionalFrame
        {
            bool parent_active;
            bool branch_taken;
            bool active;
            bool saw_else;
        };

        bool IsIdentStart(char c)
        {
            return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') || c == '_';
        }

        bool IsIdentContinue(char c)
        {
            return IsIdentStart(c) ||(c >= '0' && c <= '9');
        }

        std::string_view Trim(std::string_view text)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            {
                text.remove_prefix(1);
            }

            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            {
                text.remove_suffix(1);
            }

            return text;
        }

        std::string_view ReadWord(std::string_view & text)
        {
            text = Trim(text);
            std::size_t n = 0;
            while (n < text.size() && IsIdentContinue(text[n]))
            {
                ++n;
            }

            const auto word = text.substr(0, n);
            text.remove_prefix(n);
            return word;
        }

        // Overlay of file-local #defines over the shared predefined map: lookups
        // check the small local map first, then a tombstone set for #undef, then the
        // predefined map by pointer. The predefined map is never copied.
        struct MacroScope
        {
            const Preprocessor::MacroMap * local = nullptr;
            const Preprocessor::ErasedSet * erased = nullptr;
            const Preprocessor::MacroMap * predefined = nullptr;

            const std::string * Find(std::string_view name) const
            {
                if (local != nullptr)
                {
                    if (const auto it = local->find(name); it != local->end())
                    {
                        return &it->second;
                    }
                }

                if (erased != nullptr && erased->find(name) != erased->end())
                {
                    return nullptr;
                }

                if (predefined != nullptr)
                {
                    if (const auto it = predefined->find(name); it != predefined->end())
                    {
                        return &it->second;
                    }
                }

                return nullptr;
            }

            bool Contains(std::string_view name) const
            {
                return Find(name) != nullptr;
            }
        };

        class IfExpression
        {
        public:
            IfExpression(std::string_view input, const MacroScope &macros) : m_input(input), m_macros(macros) {}

            bool Evaluate()
            {
                const long long value = ParseOr();
                SkipSpace();
                return value != 0 && m_pos == m_input.size();
            }

        private:
            void SkipSpace()
            {
                while (m_pos < m_input.size() && std::isspace(static_cast<unsigned char>(m_input[m_pos])))
                {
                    ++m_pos;
                }
            }

            bool Consume(std::string_view token)
            {
                SkipSpace();
                if (m_input.substr(m_pos, token.size()) != token)
                {
                    return false;
                }

                m_pos += token.size();
                return true;
            }

            long long ParseOr()
            {
                long long value = ParseAnd();
                while (Consume("||"))
                {
                    const long long rhs = ParseAnd();
                    value = value != 0 || rhs != 0;
                }

                return value;
            }

            long long ParseAnd()
            {
                long long value = ParseCompare();
                while (Consume("&&"))
                {
                    const long long rhs = ParseCompare();
                    value = value != 0 && rhs != 0;
                }

                return value;
            }

            long long ParseCompare()
            {
                const long long lhs = ParseUnary();
                if (Consume("=="))
                {
                    return lhs == ParseUnary();
                }

                if (Consume("!="))
                {
                    return lhs != ParseUnary();
                }

                if (Consume("<="))
                {
                    return lhs <= ParseUnary();
                }

                if (Consume(">="))
                {
                    return lhs >= ParseUnary();
                }

                if (Consume("<"))
                {
                    return lhs < ParseUnary();
                }

                if (Consume(">"))
                {
                    return lhs > ParseUnary();
                }

                return lhs;
            }

            long long ParseUnary()
            {
                if (Consume("!"))
                {
                    return ParseUnary() == 0;
                }

                if (Consume("("))
                {
                    const long long value = ParseOr();
                    Consume(")");
                    return value;
                }

                SkipSpace();
                if (m_input.substr(m_pos, 7) == "defined")
                {
                    m_pos += 7;
                    const bool paren = Consume("(");
                    SkipSpace();
                    const std::size_t start = m_pos;
                    while (m_pos < m_input.size() && IsIdentContinue(m_input[m_pos]))
                    {
                        ++m_pos;
                    }

                    const std::string_view name = m_input.substr(start, m_pos - start);
                    if (paren)
                    {
                        Consume(")");
                    }

                    return m_macros.Contains(name) ? 1 : 0;
                }

                const std::size_t start = m_pos;
                if (m_pos < m_input.size() && (IsIdentStart(m_input[m_pos]) || std::isdigit(static_cast<unsigned char>(m_input[m_pos]))))
                {
                    while (m_pos < m_input.size() && (IsIdentContinue(m_input[m_pos]) || m_input[m_pos] == 'x' ||
                        m_input[m_pos] == 'X'))
                    {
                        ++m_pos;
                    }
                }

                if (start == m_pos)
                {
                    return 0;
                }

                std::string_view atom = m_input.substr(start, m_pos - start);
                if (IsIdentStart(atom.front()))
                {
                    const std::string * replacement = m_macros.Find(atom);
                    if (replacement == nullptr)
                    {
                        return 0;
                    }

                    atom = Trim(*replacement);
                }

                while (!atom.empty() && (atom.back() == 'u' || atom.back() == 'U' || atom.back() == 'l' || atom.back() == 'L'))
                {
                    atom.remove_suffix(1);
                }

                long long number = 0;
                int base = 10;
                if (atom.size() > 2 && atom[0] == '0' && (atom[1] == 'x' || atom[1] == 'X'))
                {
                    base = 16;
                }
                else if (atom.size() > 1 && atom[0] == '0')
                {
                    base = 8;
                }

                if (base == 16)
                {
                    atom.remove_prefix(2);
                }
                else if (base == 8)
                {
                    atom.remove_prefix(1);
                    if (atom.empty())
                    {
                        return 0;
                    }
                }

                const auto parsed = std::from_chars(atom.data(), atom.data() + atom.size(), number, base);
                return parsed.ec == std::errc{}
                && parsed.ptr == atom.data() + atom.size() ? number : 0;
            }

            std::string_view m_input;
            const MacroScope &m_macros;
            std::size_t m_pos = 0;
        };

        DirectiveKind KindOf(std::string_view name)
        {
            if (name == "include" || name == "include_next")
            {
                return DirectiveKind::Include;
            }

            if (name == "define")
            {
                return DirectiveKind::Define;
            }

            if (name == "undef")
            {
                return DirectiveKind::Undef;
            }

            if (name == "if")
            {
                return DirectiveKind::If;
            }

            if (name == "ifdef")
            {
                return DirectiveKind::Ifdef;
            }

            if (name == "ifndef")
            {
                return DirectiveKind::Ifndef;
            }

            if (name == "elif")
            {
                return DirectiveKind::Elif;
            }

            if (name == "else")
            {
                return DirectiveKind::Else;
            }

            if (name == "endif")
            {
                return DirectiveKind::Endif;
            }

            if (name == "pragma")
            {
                return DirectiveKind::Pragma;
            }

            return DirectiveKind::Other;
        }

        std::string ExpandObjectMacros(std::string_view line, const MacroScope &macros, unsigned depth = 0)
        {
            if (depth >= 16)
            {
                return std::string(line);
            }

            std::string out;
            out.reserve(line.size());
            std::size_t i = 0;
            char quote = '\0';
            while (i < line.size())
            {
                const char c = line[i];
                if (quote != '\0')
                {
                    out += c;
                    ++i;
                    if (c == '\\' && i < line.size())
                    {
                        out += line[i++];
                    }
                    else if (c == quote)
                    {
                        quote = '\0';
                    }

                    continue;
                }

                if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
                {
                    out.append(line.substr(i));
                    break;
                }

                if (c == '/' && i + 1 < line.size() && line[i + 1] == '*')
                {
                    const auto close = line.find("*/", i + 2);
                    const std::size_t end = close == std::string_view::npos ? line.size() : close + 2;
                    out.append(line.substr(i, end - i));
                    i = end;
                    continue;
                }

                if (c == '"' || c == '\'')
                {
                    quote = c;
                    out += c;
                    ++i;
                    continue;
                }

                if (IsIdentStart(c))
                {
                    const std::size_t start = i++;
                    while (i < line.size() && IsIdentContinue(line[i]))
                    {
                        ++i;
                    }

                    // Heterogeneous lookup: no std::string temporary per identifier.
                    const std::string * replacement = macros.Find(line.substr(start, i - start));
                    if (replacement == nullptr || replacement->empty())
                    {
                        out.append(line.substr(start, i - start));
                    }
                    else
                    {
                        out += ExpandObjectMacros(*replacement, macros, depth + 1);
                    }

                    continue;
                }

                out += c;
                ++i;
            }

            return out;
        }

        bool EndsWithBackslash(std::string_view body)
        {
            return !body.empty() && body.back() == '\\';
        }

    } // namespace

    PreprocessorResult Preprocessor::Process(std::string_view source, bool build_active_source) const
    {
        PreprocessorResult result;
        // Local overlay: file #defines stay small; predefined macros are read
        // through the scope pointer without copying the whole map per file.
        MacroMap local;
        local.reserve(16);
        ErasedSet erased;
        const MacroScope scope
        {
            &local, &erased, m_predefined
        };
        if (build_active_source)
        {
            result.active_source.reserve(source.size() / 2);
        }

        std::vector<ConditionalFrame> stack;
        bool active = true;
        std::size_t offset = 0;

        while (offset < source.size())
        {
            const std::size_t line_start = offset;
            std::size_t end = source.find('\n', offset);
            if (end == std::string_view::npos)
            {
                end = source.size();
            }
            else
            {
                ++end;
            }

            std::string_view line = source.substr(line_start, end - line_start);
            std::string_view body = line;
            if (!body.empty() && body.back() == '\n')
            {
                body.remove_suffix(1);
            }

            if (!body.empty() && body.back() == '\r')
            {
                body.remove_suffix(1);
            }

            auto trimmed = Trim(body);

            // A directive continues over lines ending in a backslash: the whole
            // span is one directive, and its logical text joins the pieces.
            std::string joined;
            if (!trimmed.empty() && trimmed.front() == '#' && EndsWithBackslash(body))
            {
                joined.assign(body.substr(0, body.size() - 1));
                while (end < source.size())
                {
                    const std::size_t next_end = source.find('\n', end);
                    const std::size_t stop = next_end == std::string_view::npos ? source.size() : next_end + 1;
                    std::string_view piece = source.substr(end, stop - end);
                    end = stop;
                    if (!piece.empty() && piece.back() == '\n')
                    {
                        piece.remove_suffix(1);
                    }

                    if (!piece.empty() && piece.back() == '\r')
                    {
                        piece.remove_suffix(1);
                    }

                    const bool more = EndsWithBackslash(piece);
                    joined += ' ';
                    joined.append(piece.substr(0, more ? piece.size() - 1 : piece.size()));
                    if (!more)
                    {
                        break;
                    }
                }

                line = source.substr(line_start, end - line_start);
                trimmed = Trim(joined);
            }

            if (!trimmed.empty() && trimmed.front() == '#')
            {
                trimmed.remove_prefix(1);
                trimmed = Trim(trimmed);
                const auto directive_name = ReadWord(trimmed);
                const auto kind = KindOf(directive_name);
                result.directives.push_back({kind, line_start, line.size()});

                if (kind == DirectiveKind::If || kind == DirectiveKind::Ifdef || kind == DirectiveKind::Ifndef)
                {
                    bool condition = false;
                    if (kind == DirectiveKind::If)
                    {
                        condition = IfExpression(trimmed, scope).Evaluate();
                    }
                    else
                    {
                        const std::string_view name(ReadWord(trimmed));
                        condition = scope.Contains(name);
                        if (kind == DirectiveKind::Ifndef)
                        {
                            condition =!condition;
                        }
                    }

                    stack.push_back({active, active &&condition, active &&condition, false});
                    active = stack.back().active;
                }
                else if (kind == DirectiveKind::Elif)
                {
                    if (stack.empty())
                    {
                        result.diagnostics.push_back({line_start, "#elif without matching #if"});
                    }
                    else
                    {
                        auto &frame = stack.back();
                        if (frame.saw_else)
                        {
                            result.diagnostics.push_back({line_start, "#elif after #else"});
                        }

                        const bool condition = frame.parent_active && !frame.branch_taken && IfExpression(trimmed,
                            scope).Evaluate();
                        frame.active = condition;
                        frame.branch_taken |= condition;
                        active = frame.active;
                    }
                }
                else if (kind == DirectiveKind::Else)
                {
                    if (stack.empty())
                    {
                        result.diagnostics.push_back({line_start, "#else without matching #if"});
                    }
                    else
                    {
                        auto &frame = stack.back();
                        if (frame.saw_else)
                        {
                            result.diagnostics.push_back({line_start, "duplicate #else"});
                        }

                        frame.saw_else = true;
                        frame.active = frame.parent_active && !frame.branch_taken;
                        frame.branch_taken = true;
                        active = frame.active;
                    }
                }
                else if (kind == DirectiveKind::Endif)
                {
                    if (stack.empty())
                    {
                        result.diagnostics.push_back({line_start, "#endif without matching #if"});
                    }
                    else
                    {
                        active = stack.back().parent_active;
                        stack.pop_back();
                    }
                }
                else if (active && kind == DirectiveKind::Define)
                {
                    trimmed = Trim(trimmed);
                    std::size_t name_len = 0;
                    while (name_len < trimmed.size() && IsIdentContinue(trimmed[name_len]))
                    {
                        ++name_len;
                    }

                    if (name_len == 0)
                    {
                        result.diagnostics.push_back({line_start, "#define requires a macro name"});
                    }
                    else
                    {
                        std::string name(trimmed.substr(0, name_len));
                        // Function-like macro: retain as an opaque definition; do not expand it.
                        if (name_len < trimmed.size() && trimmed[name_len] == '(')
                        {
                            local.erase(name);
                            erased.insert(std::move(name));
                        }
                        else
                        {
                            erased.erase(name);
                            local[std::move(name)] = std::string(Trim(trimmed.substr(name_len)));
                            // A re-#define revives the name even if previously #undef'd.
                        }
                    }
                }
                else if (active && kind == DirectiveKind::Undef)
                {
                    std::string name(ReadWord(trimmed));
                    local.erase(name);
                    erased.insert(std::move(name));
                }
            }
            else if (active)
            {
                result.active_ranges.push_back({line_start, line.size()});
                if (build_active_source)
                {
                    result.active_source += ExpandObjectMacros(line, scope);
                }
            }

            offset = end;
        }

        if (!stack.empty())
        {
            result.diagnostics.push_back({source.size(), "unterminated conditional directive"});
        }

        return result;
    }

} // namespace heimdall
