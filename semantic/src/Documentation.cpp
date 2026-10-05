#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

#include "detail/TokenView.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heimdall
{

    namespace
    {

        using detail::TokenView;

        constexpr std::size_t kNpos = ~std::size_t {0};

        // Where a declaration stands, for the DocScope option: Skip for anything
        // local to a function, Private for private members and internal linkage.
        enum class Visibility : std::uint8_t
        {
            Skip,
            Public,
            Private
        };

        enum class Access : std::uint8_t
        {
            Public,
            Protected,
            Private
        };

        // Three-valued: the rules only act on a certain answer.
        enum class Tri : std::uint8_t
        {
            No,
            Yes,
            Unknown
        };

        bool IsSpace(char c)
        {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        }

        std::string_view Trim(std::string_view text)
        {
            while (!text.empty() && IsSpace(text.front()))
            {
                text.remove_prefix(1);
            }

            while (!text.empty() && IsSpace(text.back()))
            {
                text.remove_suffix(1);
            }

            return text;
        }

        // ---------------------------------------------------------------------
        // Comments

        // `///` and `//!` lines, but not `////` banners and not `///<`, which
        // documents what comes before it.
        bool IsLeadingDocLine(std::string_view text)
        {
            if (text.starts_with("//!"))
            {
                return !text.starts_with("//!<");
            }

            return text.starts_with("///") && !text.starts_with("////") && !text.starts_with("///<");
        }

        // `/** */` and `/*! */`, but not `/**/`, `/***` banners and `/**<`.
        bool IsLeadingDocBlock(std::string_view text)
        {
            if (text.starts_with("/*!"))
            {
                return !text.starts_with("/*!<");
            }

            return text.starts_with("/**") && !text.starts_with("/***") && !text.starts_with("/**/") &&
                !text.starts_with("/**<");
        }

        bool IsTrailingDoc(std::string_view text)
        {
            return text.starts_with("///<") || text.starts_with("//!<") || text.starts_with("/**<") ||
                text.starts_with("/*!<");
        }

        struct DocLine
        {
            std::string text;        // without the comment markers
            std::size_t offset = 0;  // of the first character of the line, in the source
            std::size_t length = 0;  // of the line, without surrounding blanks
        };

        struct Doc
        {
            bool present = false;
            bool qt_style = false; // `//!` or `/*! */`
            std::vector<DocLine> lines;
        };

        void AddLine(Doc &doc, std::string_view source, std::size_t begin, std::size_t end, std::string_view content)
        {
            while (begin < end && IsSpace(source[begin]))
            {
                ++begin;
            }

            while (end > begin && IsSpace(source[end - 1]))
            {
                --end;
            }

            if (content.starts_with(' '))
            {
                content.remove_prefix(1);
            }

            doc.lines.push_back({std::string(content), begin, end - begin});
        }

        void AddLineComment(Doc &doc, std::string_view source, const Token &token)
        {
            std::string_view text = source.substr(token.offset, token.length);
            const bool qt = text.starts_with("//!");
            if (doc.lines.empty())
            {
                doc.qt_style = qt;
            }

            text.remove_prefix(3);
            if (text.starts_with('<'))
            {
                text.remove_prefix(1);
            }

            AddLine(doc, source, token.offset, token.offset + token.length, text);
        }

        void AddBlockComment(Doc &doc, std::string_view source, const Token &token)
        {
            std::string_view text = source.substr(token.offset, token.length);
            doc.qt_style = text.starts_with("/*!");
            std::size_t cursor = 3;
            if (cursor < text.size() && text[cursor] == '<')
            {
                ++cursor;
            }

            std::size_t body_end = text.size();
            if (text.ends_with("*/") && body_end >= cursor + 2)
            {
                body_end -= 2;
            }

            bool first = true;
            while (cursor <= body_end)
            {
                std::size_t line_end = text.find('\n', cursor);
                if (line_end == std::string_view::npos || line_end > body_end)
                {
                    line_end = body_end;
                }

                std::size_t piece_end = line_end;
                if (piece_end > cursor && text[piece_end - 1] == '\r')
                {
                    --piece_end;
                }

                std::string_view content = text.substr(cursor, piece_end - cursor);
                if (!first)
                {
                    const auto indent = content.find_first_not_of(" \t");
                    content = indent == std::string_view::npos ? std::string_view {} : content.substr(indent);
                    if (content.starts_with('*'))
                    {
                        content.remove_prefix(1);
                    }
                }

                const auto begin = token.offset + (first ? 0 : cursor);
                AddLine(doc, source, begin, token.offset + piece_end, content);
                first = false;
                if (line_end >= body_end)
                {
                    break;
                }

                cursor = line_end + 1;
            }
        }

        // ---------------------------------------------------------------------
        // Doxygen commands

        struct Command
        {
            std::string name;
            bool at = true;                 // `@name`, as opposed to `\name`
            std::vector<std::string> parts; // text after the name, then continuation lines
            std::size_t line = 0;           // index in Doc::lines
        };

        constexpr std::array<std::string_view, 40> kCommands = {"brief", "short", "details", "param", "tparam",
            "return", "returns", "result", "retval", "throw", "throws", "exception", "copydoc", "copybrief",
            "copydetails", "inheritdoc", "overload", "file", "defgroup", "page", "mainpage", "dir", "see", "sa",
            "note", "warning", "remark", "remarks", "pre", "post", "since", "deprecated", "todo", "bug", "author",
            "version", "date", "attention", "par", "invariant"};

        bool IsKnownCommand(std::string_view name)
        {
            return std::find(kCommands.begin(), kCommands.end(), name) != kCommands.end();
        }

        std::string Joined(const Command &command)
        {
            std::string result;
            for (const auto &part: command.parts)
            {
                const auto trimmed = Trim(part);
                if (trimmed.empty())
                {
                    continue;
                }

                if (!result.empty())
                {
                    result += ' ';
                }

                result += trimmed;
            }

            return result;
        }

        // Commands of a comment, in order. A command's text runs to the next
        // command or the next blank line; only the commands listed above count, so
        // `\f$`, `\p` and friends stay plain text.
        std::vector<Command> ParseCommands(const Doc &doc)
        {
            std::vector<Command> commands;
            bool open = false; // the last command still takes continuation lines
            for (std::size_t line = 0; line < doc.lines.size(); ++line)
            {
                const std::string_view text = doc.lines[line].text;
                if (Trim(text).empty())
                {
                    open = false;
                    continue;
                }

                struct Span
                {
                    std::size_t begin;
                    std::size_t name_begin;
                    std::size_t name_end;
                    bool at;
                };
                std::vector<Span> spans;
                for (std::size_t i = 0; i < text.size(); ++i)
                {
                    const char c = text[i];
                    if (c != '@' && c != '\\')
                    {
                        continue;
                    }

                    if (i > 0 && !IsSpace(text[i - 1]) && text[i - 1] != '(' && text[i - 1] != '*')
                    {
                        continue;
                    }

                    std::size_t end = i + 1;
                    while (end < text.size() && std::isalpha(static_cast<unsigned char>(text[end])) != 0)
                    {
                        ++end;
                    }

                    if (end > i + 1 && IsKnownCommand(text.substr(i + 1, end - i - 1)))
                    {
                        spans.push_back({i, i + 1, end, c == '@'});
                        i = end - 1;
                    }
                }

                if (spans.empty())
                {
                    if (open)
                    {
                        commands.back().parts.emplace_back(text);
                    }

                    continue;
                }

                if (spans.front().begin > 0 && open)
                {
                    commands.back().parts.emplace_back(text.substr(0, spans.front().begin));
                }

                for (std::size_t k = 0; k < spans.size(); ++k)
                {
                    const auto stop = k + 1 < spans.size() ? spans[k + 1].begin : text.size();
                    Command command;
                    command.name = std::string(text.substr(spans[k].name_begin, spans[k].name_end - spans[k].name_begin));
                    command.at = spans[k].at;
                    command.line = line;
                    command.parts.emplace_back(text.substr(spans[k].name_end, stop - spans[k].name_end));
                    commands.push_back(std::move(command));
                }

                open = true;
            }

            return commands;
        }

        const Command *Find(const std::vector<Command> &commands, std::initializer_list<std::string_view> names)
        {
            for (const auto &command: commands)
            {
                if (std::find(names.begin(), names.end(), command.name) != names.end())
                {
                    return &command;
                }
            }

            return nullptr;
        }

        // A comment that describes a file, group or page, not the declaration under it.
        bool IsDetached(const std::vector<Command> &commands)
        {
            return Find(commands, {"file", "defgroup", "page", "mainpage", "dir"}) != nullptr;
        }

        // The text is somewhere else: nothing to check here.
        bool IsInherited(const std::vector<Command> &commands)
        {
            return Find(commands, {"copydoc", "copydetails", "inheritdoc", "overload"}) != nullptr;
        }

        // `.`, `!` or `?` followed by a capitalized word: a second sentence.
        bool HasSecondSentence(std::string_view text)
        {
            for (std::size_t i = 0; i + 2 < text.size(); ++i)
            {
                if ((text[i] != '.' && text[i] != '!' && text[i] != '?') || !IsSpace(text[i + 1]))
                {
                    continue;
                }

                std::size_t next = i + 1;
                while (next < text.size() && IsSpace(text[next]))
                {
                    ++next;
                }

                if (next >= text.size() || std::isupper(static_cast<unsigned char>(text[next])) == 0)
                {
                    continue;
                }

                if (text[i] == '.')
                {
                    std::size_t begin = i;
                    while (begin > 0 && std::isalpha(static_cast<unsigned char>(text[begin - 1])) != 0)
                    {
                        --begin;
                    }

                    const auto word = text.substr(begin, i - begin);
                    if (word.size() <= 1 || word == "etc" || word == "vs" || word == "cf")
                    {
                        continue; // `e.g. Foo`, `i.e. Foo`, initials
                    }
                }

                return true;
            }

            return false;
        }

        // ---------------------------------------------------------------------
        // Declarations

        struct Named
        {
            std::string_view name;
            std::size_t position = 0; // of the name token, in significant tokens
        };

        struct Entity
        {
            SymbolId symbol = kNone;
            bool function = false;
            std::size_t name = 0;  // position of the name token
            std::size_t start = 0; // position of the first token of the declaration
            std::size_t semi = kNpos;
            std::vector<Named> params;
            std::vector<Named> tparams;
            bool params_known = false;
            bool tparams_known = true;
            bool specialization = false; // `template<>`
            Tri returns = Tri::Unknown;
            bool throws = false;
        };

        struct ClassInfo
        {
            bool valid = false;
            bool is_class = false;
            std::size_t open = 0;
            std::size_t close = 0;
            std::vector<std::pair<std::size_t, Access>> markers;
        };

        class DocAnalysis
        {
        public:
            DocAnalysis(const SemanticModel &model, DocScope scope)
            : m_model(model), m_view(model), m_symbols(model.Symbols()), m_scopes(model.Scopes()),
              m_tree(model.Tree()), m_scope(scope)
            {
            }

            const SemanticModel &Model() const
            {
                return m_model;
            }
            const TokenView &View() const
            {
                return m_view;
            }

            // Fills `entity` when `symbol` is something the doc rules look at. With
            // `for_comment` (doc/require-comment) declarations documented elsewhere
            // are left out too; doc/doxygen-style checks any comment that exists.
            bool Describe(SymbolId symbol, Entity &entity, bool for_comment)
            {
                const auto kind = m_symbols.kind[symbol];
                if (kind != SymbolKind::Function && kind != SymbolKind::Class && kind != SymbolKind::Enum)
                {
                    return false;
                }

                if (m_symbols.name[symbol] == kNone || m_symbols.decl_token[symbol] == kNone)
                {
                    return false;
                }

                const auto vis = Classify(symbol);
                if (vis == Visibility::Skip || (m_scope == DocScope::Public && vis != Visibility::Public) ||
                    (m_scope == DocScope::Private && vis != Visibility::Private))
                {
                    return false;
                }

                entity = Entity {};
                entity.symbol = symbol;
                entity.name = m_view.PositionOf(m_symbols.decl_token[symbol]);
                entity.start = DeclarationStart(entity.name);
                if (kind == SymbolKind::Function)
                {
                    entity.function = true;
                    return DescribeFunction(symbol, entity, for_comment);
                }

                if (!HasBody(entity.name))
                {
                    return false;
                }

                DescribeTemplateHeader(entity, entity.name);
                return !(for_comment && IsSpecialization(entity));
            }

            Doc DocOf(const Entity &entity) const
            {
                Doc doc;
                const auto &tokens = m_tree.Tokens();
                const auto source = m_tree.Source();
                std::size_t newlines = 0;
                const auto previous = Previous(m_view.TokenAt(entity.start), newlines);
                if (previous != kNpos && newlines <= 1)
                {
                    const auto &token = tokens[previous];
                    const auto text = m_tree.Text(token);
                    if (token.kind == TokenKind::BlockComment && IsLeadingDocBlock(text))
                    {
                        AddBlockComment(doc, source, token);
                    }
                    else if (token.kind == TokenKind::LineComment && IsLeadingDocLine(text))
                    {
                        std::vector<std::size_t> run {previous};
                        for (;;)
                        {
                            std::size_t gap = 0;
                            const auto before = Previous(run.back(), gap);
                            if (before == kNpos || gap != 1 || tokens[before].kind != TokenKind::LineComment ||
                                !IsLeadingDocLine(m_tree.Text(tokens[before])))
                            {
                                break;
                            }

                            run.push_back(before);
                        }

                        for (auto it = run.rbegin(); it != run.rend(); ++it)
                        {
                            AddLineComment(doc, source, tokens[*it]);
                        }
                    }
                }

                if (!doc.lines.empty())
                {
                    doc.present = true;
                    return doc;
                }

                if (entity.semi != kNpos)
                {
                    for (auto i = static_cast<std::size_t>(m_view.TokenAt(entity.semi)) + 1; i < tokens.size(); ++i)
                    {
                        const auto &token = tokens[i];
                        if (token.kind == TokenKind::Whitespace)
                        {
                            if (m_tree.Text(token).find('\n') != std::string_view::npos)
                            {
                                break;
                            }

                            continue;
                        }

                        if (token.kind == TokenKind::LineComment && IsTrailingDoc(m_tree.Text(token)))
                        {
                            AddLineComment(doc, source, token);
                            doc.present = true;
                        }
                        else if (token.kind == TokenKind::BlockComment && IsTrailingDoc(m_tree.Text(token)))
                        {
                            AddBlockComment(doc, source, token);
                            doc.present = !doc.lines.empty();
                        }

                        break;
                    }
                }

                return doc;
            }

            Diagnostic Make(RuleId rule, std::string_view code, std::string message, std::size_t offset,
                std::size_t length)
            {
                if (!m_lines_built)
                {
                    m_lines.Build(m_tree.Source());
                    m_lines_built = true;
                }

                const auto position = m_lines.Lookup(offset);
                return Diagnostic {rule, Severity::Warning, std::string(code), std::move(message), offset, length,
                    position.line, position.column, false, TextEdit {}};
            }

            Diagnostic MakeAtName(RuleId rule, std::string_view code, std::string message, std::size_t name)
            {
                const auto offset = m_view.Offset(name);
                return Make(rule, code, std::move(message), offset, m_view.End(name) - offset);
            }

            std::string NameOf(const Entity &entity) const
            {
                return std::string(m_model.Names().Text(m_symbols.name[entity.symbol]));
            }

            const char *KindWord(const Entity &entity) const
            {
                if (entity.function)
                {
                    return "function";
                }

                return m_symbols.kind[entity.symbol] == SymbolKind::Enum ? "enum" : "class";
            }

        private:
            // Index of the nearest earlier token that is not blank or a decoration
            // macro, and how many line breaks lay between.
            std::size_t Previous(std::size_t index, std::size_t &newlines) const
            {
                const auto &tokens = m_tree.Tokens();
                newlines = 0;
                while (index > 0)
                {
                    --index;
                    if (tokens[index].kind == TokenKind::Whitespace)
                    {
                        for (const char c: m_tree.Text(tokens[index]))
                        {
                            newlines += c == '\n' ? 1 : 0;
                        }

                        continue;
                    }

                    if (m_tree.IsDecorationToken(index))
                    {
                        continue;
                    }

                    return index;
                }

                return kNpos;
            }

            // First token of the declaration whose name is at `name`, template
            // header included: where its comment is written.
            std::size_t DeclarationStart(std::size_t name) const
            {
                std::size_t start = name;
                while (start > 0)
                {
                    const Tok before = m_view.At(start - 1);
                    if (before == Tok::Semi || before == Tok::LBrace || before == Tok::RBrace ||
                        before == Tok::Colon)
                    {
                        break;
                    }

                    --start;
                }

                return start;
            }

            // A class or enum with a `{...}` body (not a forward declaration).
            bool HasBody(std::size_t name) const
            {
                for (auto i = name + 1; i < m_view.Size(); ++i)
                {
                    const Tok tok = m_view.At(i);
                    if (tok == Tok::LBrace)
                    {
                        return true;
                    }

                    if (tok == Tok::Semi)
                    {
                        return false;
                    }
                }

                return false;
            }

            const ClassInfo &InfoOf(SymbolId klass)
            {
                if (const auto found = m_classes.find(klass); found != m_classes.end())
                {
                    return found->second;
                }

                ClassInfo info;
                if (m_symbols.kind[klass] == SymbolKind::Class)
                {
                    const auto [begin, end] = m_view.Range(m_symbols.decl_node[klass]);
                    const auto name = m_view.PositionOf(m_symbols.decl_token[klass]);
                    bool keyword = false;
                    for (auto i = begin; i < name && i < end; ++i)
                    {
                        const Tok tok = m_view.At(i);
                        if (tok == Tok::KwClass || tok == Tok::KwStruct || tok == Tok::KwUnion)
                        {
                            info.is_class = tok == Tok::KwClass;
                            keyword = true;
                        }
                    }

                    std::size_t open = name;
                    while (keyword && open < m_view.Size() && m_view.At(open) != Tok::LBrace &&
                        m_view.At(open) != Tok::Semi)
                    {
                        ++open;
                    }

                    if (keyword && m_view.At(open) == Tok::LBrace)
                    {
                        info.open = open;
                        info.close = m_view.Match(open, m_view.Size());
                        info.valid = info.close < m_view.Size();
                    }

                    for (auto i = info.open + 1; info.valid && i < info.close; ++i)
                    {
                        const Tok tok = m_view.At(i);
                        if (tok == Tok::LBrace)
                        {
                            i = m_view.Match(i, m_view.Size());
                        }
                        else if (m_view.At(i + 1) == Tok::Colon &&
                            (tok == Tok::KwPublic || tok == Tok::KwProtected || tok == Tok::KwPrivate))
                        {
                            info.markers.emplace_back(i, tok == Tok::KwPublic ? Access::Public
                                    : tok == Tok::KwProtected             ? Access::Protected
                                                                          : Access::Private);
                        }
                    }
                }

                return m_classes.emplace(klass, std::move(info)).first->second;
            }

            // Access level of `member` among the members of `klass`.
            Access AccessOf(SymbolId klass, SymbolId member)
            {
                const auto &info = InfoOf(klass);
                if (!info.valid)
                {
                    return Access::Public;
                }

                const auto position = m_view.PositionOf(m_symbols.decl_token[member]);
                Access access = info.is_class ? Access::Private : Access::Public;
                for (const auto &[marker, level]: info.markers)
                {
                    if (marker >= position)
                    {
                        break;
                    }

                    access = level;
                }

                return access;
            }

            Visibility Classify(SymbolId symbol)
            {
                const auto flags = m_symbols.flags[symbol];
                bool internal = m_symbols.kind[symbol] == SymbolKind::Function && (flags & SymbolFlag::Static) != 0 &&
                    m_scopes.kind[m_symbols.scope[symbol]] != ScopeKind::Class;
                SymbolId item = symbol;
                for (ScopeId scope = m_symbols.scope[symbol]; scope != kNone && scope < m_scopes.Size();
                    scope = m_scopes.parent[scope])
                {
                    switch (m_scopes.kind[scope])
                    {
                    case ScopeKind::Function:
                    case ScopeKind::Block:
                        return Visibility::Skip;
                    case ScopeKind::Namespace:
                        internal = internal || m_scopes.owner[scope] == kNone;
                        break;
                    case ScopeKind::Class:
                        if (const auto owner = m_scopes.owner[scope]; owner != kNone)
                        {
                            internal = internal || AccessOf(owner, item) == Access::Private;
                            item = owner;
                        }

                        break;
                    default:
                        break;
                    }

                    if (scope == SemanticModel::TranslationUnitScope)
                    {
                        break;
                    }
                }

                return internal ? Visibility::Private : Visibility::Public;
            }

            // `template<>` with nothing in it: an explicit specialization, whose
            // comment is the primary template's.
            static bool IsSpecialization(const Entity &entity)
            {
                return entity.tparams_known && entity.tparams.empty() && entity.specialization;
            }

            // Names of the parameters of the template header(s) starting at `start`;
            // returns the position after them.
            std::size_t DescribeTemplateHeader(Entity &entity, std::size_t limit)
            {
                std::size_t p = entity.start;
                while (m_view.At(p) == Tok::KwTemplate && m_view.At(p + 1) == Tok::Lt)
                {
                    const auto close = m_view.MatchAngle(p + 1, limit);
                    if (close >= limit)
                    {
                        entity.tparams_known = false;
                        return kNpos;
                    }

                    if (close == p + 2)
                    {
                        entity.specialization = true;
                    }

                    SplitList(p + 1, close, entity.tparams, true);
                    p = close + 1;
                }

                return p;
            }

            static bool IsQualifierToken(Tok tok)
            {
                return tok == Tok::ColonColon || tok == Tok::Lt;
            }

            // Names in the comma-separated list between `open` and `close`: function
            // parameters or template parameters.
            void SplitList(std::size_t open, std::size_t close, std::vector<Named> &out, bool templates) const
            {
                std::size_t segment = open + 1;
                int depth = 0;
                for (std::size_t i = open + 1; i <= close; ++i)
                {
                    const Tok tok = i < close ? m_view.At(i) : Tok::Comma;
                    if (i < close)
                    {
                        if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace || tok == Tok::Lt)
                        {
                            if (tok == Tok::LParen && i > segment &&
                                (m_view.At(i - 1) == Tok::KwDecltype || m_view.At(i - 1) == Tok::KwAlignas ||
                                    m_view.At(i - 1) == Tok::KwNoexcept || m_view.At(i - 1) == Tok::KwSizeof))
                            {
                                i = m_view.Match(i, close);
                                continue;
                            }

                            ++depth;
                            continue;
                        }

                        if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace || tok == Tok::Gt)
                        {
                            depth = std::max(0, depth - 1);
                            continue;
                        }

                        if (tok == Tok::Shr)
                        {
                            depth = std::max(0, depth - 2);
                            continue;
                        }
                    }

                    if (tok != Tok::Comma || depth != 0)
                    {
                        continue;
                    }

                    NameOfSegment(segment, i, out, templates);
                    segment = i + 1;
                }
            }

            // The declared name of one parameter in [begin, end), when it has one.
            void NameOfSegment(std::size_t begin, std::size_t end, std::vector<Named> &out, bool templates) const
            {
                // Default argument or default template argument.
                int depth = 0;
                std::size_t stop = end;
                for (auto i = begin; i < end; ++i)
                {
                    const Tok tok = m_view.At(i);
                    if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace || tok == Tok::Lt)
                    {
                        ++depth;
                    }
                    else if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace || tok == Tok::Gt)
                    {
                        depth = std::max(0, depth - 1);
                    }
                    else if (tok == Tok::Eq && depth == 0)
                    {
                        stop = i;
                        break;
                    }
                }

                if (stop <= begin || (stop == begin + 1 && m_view.At(begin) == Tok::KwVoid))
                {
                    return;
                }

                if (!templates)
                {
                    // `void (*callback)(int)`: the name is inside the first group.
                    for (auto i = begin; i < stop; ++i)
                    {
                        const Tok tok = m_view.At(i);
                        if (tok == Tok::LParen && i > begin && (m_view.At(i - 1) == Tok::KwDecltype ||
                                m_view.At(i - 1) == Tok::KwAlignas || m_view.At(i - 1) == Tok::KwNoexcept))
                        {
                            i = m_view.Match(i, stop);
                            continue;
                        }

                        if (tok == Tok::Lt)
                        {
                            // `std::function<void(int)> callback`: the parentheses are a type.
                            const auto angle = m_view.MatchAngle(i, stop);
                            if (angle < stop)
                            {
                                i = angle;
                            }

                            continue;
                        }

                        if (tok == Tok::LParen)
                        {
                            const auto group = m_view.Match(i, stop);
                            if (group < stop && group > i + 1 && m_view.IsWord(group - 1) && m_view.At(group - 2) != Tok::LParen)
                            {
                                out.push_back({m_view.Text(group - 1), group - 1});
                            }

                            return;
                        }

                        if (tok == Tok::LBracket)
                        {
                            stop = i; // `int values[4]`
                            break;
                        }
                    }
                }
                else
                {
                    // A `template<...>` parameter of a template template parameter has its own list.
                    for (auto i = begin; i < stop; ++i)
                    {
                        if (m_view.At(i) == Tok::KwTemplate && m_view.At(i + 1) == Tok::Lt)
                        {
                            const auto angle = m_view.MatchAngle(i + 1, stop);
                            if (angle >= stop)
                            {
                                return;
                            }

                            i = angle;
                        }
                    }
                }

                if (stop < begin + 2 || !m_view.IsWord(stop - 1))
                {
                    return;
                }

                const Tok before = m_view.At(stop - 2);
                const bool type_only = IsQualifierToken(before) ||
                    (!templates && (before == Tok::KwConst || before == Tok::KwVolatile || before == Tok::KwStruct ||
                        before == Tok::KwClass || before == Tok::KwEnum || before == Tok::KwUnion ||
                        before == Tok::KwTypename));
                if (!type_only)
                {
                    out.push_back({m_view.Text(stop - 1), stop - 1});
                }
            }

            bool DescribeFunction(SymbolId symbol, Entity &entity, bool for_comment)
            {
                const auto flags = m_symbols.flags[symbol];
                if (for_comment)
                {
                    if ((flags & (SymbolFlag::Qualified | SymbolFlag::Friend | SymbolFlag::Defaulted |
                                     SymbolFlag::Override)) != 0)
                    {
                        return false;
                    }

                    if (m_scopes.kind[m_symbols.scope[symbol]] == ScopeKind::TranslationUnit &&
                        m_model.Names().Text(m_symbols.name[symbol]) == "main")
                    {
                        return false;
                    }

                    // The first declaration carries the comment.
                    for (auto other = m_model.LookupLocal(m_symbols.scope[symbol], m_symbols.name[symbol]);
                        other != kNone; other = m_symbols.next_same_name[other])
                    {
                        if (other != symbol && m_symbols.kind[other] == SymbolKind::Function &&
                            m_symbols.signature[other] != 0 && m_symbols.signature[other] == m_symbols.signature[symbol] &&
                            m_symbols.decl_token[other] < m_symbols.decl_token[symbol])
                        {
                            return false;
                        }
                    }
                }

                if (m_view.At(entity.name + 1) != Tok::LParen)
                {
                    return false;
                }

                const auto open = entity.name + 1;
                const auto close = m_view.Match(open, m_view.Size());
                if (close >= m_view.Size())
                {
                    return false;
                }

                std::size_t name_start = entity.name;
                if ((flags & SymbolFlag::Operator) != 0)
                {
                    for (std::size_t back = 1; back <= 5 && back <= entity.name; ++back)
                    {
                        if (m_view.At(entity.name - back) == Tok::KwOperator)
                        {
                            name_start = entity.name - back;
                            break;
                        }
                    }
                }
                else if ((flags & SymbolFlag::Destructor) != 0 && entity.name > 0)
                {
                    name_start = entity.name - 1;
                }

                const bool qualified_names = name_start >= 2 && m_view.At(name_start - 1) == Tok::ColonColon;
                while (name_start >= 2 && m_view.At(name_start - 1) == Tok::ColonColon && m_view.IsWord(name_start - 2))
                {
                    name_start -= 2;
                }

                entity.params_known = true;
                SplitList(open, close, entity.params, false);

                const auto after_header = DescribeTemplateHeader(entity, name_start);
                if (for_comment && IsSpecialization(entity))
                {
                    return false;
                }

                entity.returns = ReturnKind(flags, after_header, name_start, close, qualified_names);
                for (auto i = close + 1; i < m_view.Size(); ++i)
                {
                    const Tok tok = m_view.At(i);
                    if (tok == Tok::LParen)
                    {
                        i = m_view.Match(i, m_view.Size());
                        continue;
                    }

                    if (tok == Tok::Semi)
                    {
                        entity.semi = i;
                        break;
                    }

                    if (tok == Tok::LBrace)
                    {
                        break;
                    }
                }

                if ((flags & SymbolFlag::Definition) != 0)
                {
                    entity.throws = BodyThrows(m_symbols.decl_node[symbol], close);
                }

                return true;
            }

            // Does the function the node at `node` defines throw out of its body?
            bool BodyThrows(std::uint32_t node, std::size_t close) const
            {
                const auto end = m_view.Range(node).second;
                for (auto i = close + 1; i < end; ++i)
                {
                    const Tok tok = m_view.At(i);
                    if (tok == Tok::KwNoexcept && m_view.At(i + 1) != Tok::LParen)
                    {
                        return false;
                    }

                    if (tok == Tok::KwTry && m_view.At(i + 1) == Tok::LBrace)
                    {
                        i = m_view.Match(i + 1, end); // thrown inside is probably handled there
                        continue;
                    }

                    if (tok == Tok::KwThrow)
                    {
                        return true;
                    }
                }

                return false;
            }

            Tri ReturnKind(std::uint32_t flags, std::size_t after_header, std::size_t name_start, std::size_t close,
                bool qualified) const
            {
                if ((flags & (SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0)
                {
                    return Tri::No;
                }

                if (after_header == kNpos)
                {
                    return Tri::Unknown;
                }

                std::size_t p = after_header;
                for (;;)
                {
                    const Tok tok = m_view.At(p);
                    if (p >= name_start)
                    {
                        break;
                    }

                    if (tok == Tok::LBracket && m_view.At(p + 1) == Tok::LBracket)
                    {
                        p = m_view.Match(p, name_start) + 1;
                    }
                    else if (tok == Tok::KwStatic || tok == Tok::KwInline || tok == Tok::KwVirtual ||
                        tok == Tok::KwConstexpr || tok == Tok::KwConsteval || tok == Tok::KwExplicit ||
                        tok == Tok::KwFriend || tok == Tok::KwThreadLocal)
                    {
                        ++p;
                    }
                    else if (tok == Tok::KwExtern)
                    {
                        ++p;
                        if (m_view.KindAt(p) == TokenKind::StringLiteral)
                        {
                            ++p;
                        }
                    }
                    else
                    {
                        break;
                    }
                }

                if (p >= name_start)
                {
                    return Tri::No; // constructors, conversion operators, deduction guides
                }

                bool deduced = false;
                bool angle = false;
                std::size_t significant = 0;
                bool is_void = false;
                for (auto i = p; i < name_start; ++i)
                {
                    const Tok tok = m_view.At(i);
                    deduced = deduced || tok == Tok::KwAuto || tok == Tok::KwDecltype;
                    angle = angle || tok == Tok::Gt || tok == Tok::Shr;
                    if (tok != Tok::KwConst && tok != Tok::KwVolatile)
                    {
                        ++significant;
                        is_void = tok == Tok::KwVoid;
                    }
                }

                if (qualified && angle)
                {
                    return Tri::Unknown; // `A<T>::f`: the qualifier is part of what was read
                }

                if (deduced)
                {
                    for (auto i = close + 1; i < m_view.Size(); ++i)
                    {
                        const Tok tok = m_view.At(i);
                        if (tok == Tok::LParen)
                        {
                            i = m_view.Match(i, m_view.Size());
                        }
                        else if (tok == Tok::Semi || tok == Tok::LBrace || tok == Tok::Eq)
                        {
                            break;
                        }
                        else if (tok == Tok::Arrow)
                        {
                            return m_view.At(i + 1) == Tok::KwVoid &&
                                    (m_view.At(i + 2) == Tok::LBrace || m_view.At(i + 2) == Tok::Semi ||
                                        m_view.At(i + 2) == Tok::KwNoexcept || m_view.At(i + 2) == Tok::KwRequires ||
                                        m_view.At(i + 2) == Tok::Eq || m_view.At(i + 2) == Tok::KwConst ||
                                        m_view.At(i + 2) == Tok::KwOverride || m_view.At(i + 2) == Tok::KwFinal)
                                ? Tri::No
                                : Tri::Yes;
                        }
                    }

                    return Tri::Unknown;
                }

                return significant == 1 && is_void ? Tri::No : Tri::Yes;
            }

            const SemanticModel &m_model;
            TokenView m_view;
            const SymbolTable &m_symbols;
            const ScopeTable &m_scopes;
            const ParseTree &m_tree;
            DocScope m_scope;
            LineTable m_lines;
            bool m_lines_built = false;
            std::unordered_map<SymbolId, ClassInfo> m_classes;
        };

        void Append(std::vector<Diagnostic> &all, std::vector<Diagnostic> part)
        {
            all.insert(all.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
        }

        void SortByOffset(std::vector<Diagnostic> &diagnostics)
        {
            std::stable_sort(diagnostics.begin(), diagnostics.end(),
                [](const Diagnostic &a, const Diagnostic &b)
                {
                    return a.offset < b.offset;
                });
        }

        // The `@param`/`@tparam` commands against what the signature declares.
        void CheckNamed(DocAnalysis &analysis, std::vector<Diagnostic> &out, const Doc &doc,
            const std::vector<Command> &commands, std::string_view tag, const std::vector<Named> &declared,
            bool known, std::string_view what)
        {
            std::vector<std::string> seen;
            for (const auto &command: commands)
            {
                if (command.name != tag)
                {
                    continue;
                }

                std::string text = Joined(command);
                std::string_view rest = text;
                if (rest.starts_with('['))
                {
                    const auto close = rest.find(']');
                    rest = close == std::string_view::npos ? std::string_view {} : rest.substr(close + 1);
                }

                rest = Trim(rest);
                std::size_t end = 0;
                while (end < rest.size() && !IsSpace(rest[end]))
                {
                    ++end;
                }

                const auto names = rest.substr(0, end);
                const auto description = Trim(rest.substr(end));
                const auto &line = doc.lines[command.line];
                if (names.empty())
                {
                    out.push_back(analysis.Make(RuleId::DocDoxygenStyle, "doc/doxygen-style",
                        "@" + std::string(tag) + " without a name", line.offset, line.length));
                    continue;
                }

                std::size_t begin = 0;
                while (begin <= names.size())
                {
                    auto comma = names.find(',', begin);
                    if (comma == std::string_view::npos)
                    {
                        comma = names.size();
                    }

                    const std::string name(names.substr(begin, comma - begin));
                    begin = comma + 1;
                    if (name.empty())
                    {
                        continue;
                    }

                    const bool repeated = std::find(seen.begin(), seen.end(), name) != seen.end();
                    seen.push_back(name);
                    const bool exists = std::any_of(declared.begin(), declared.end(),
                        [&](const Named &item)
                        {
                            return item.name == name;
                        });
                    if (repeated)
                    {
                        out.push_back(analysis.Make(RuleId::DocDoxygenStyle, "doc/doxygen-style",
                            "@" + std::string(tag) + " '" + name + "' is documented more than once", line.offset,
                            line.length));
                    }
                    else if (known && !exists)
                    {
                        out.push_back(analysis.Make(RuleId::DocDoxygenStyle, "doc/doxygen-style",
                            "@" + std::string(tag) + " '" + name + "' does not match any " + std::string(what),
                            line.offset, line.length));
                    }
                    else if (description.empty())
                    {
                        out.push_back(analysis.Make(RuleId::DocDoxygenStyle, "doc/doxygen-style",
                            "@" + std::string(tag) + " '" + name +
                                "' has no description; say what it is, its unit and its valid range",
                            line.offset, line.length));
                    }
                }
            }

            if (!known)
            {
                return;
            }

            for (const auto &item: declared)
            {
                const bool documented = std::any_of(commands.begin(), commands.end(),
                    [&](const Command &command)
                    {
                        if (command.name != tag)
                        {
                            return false;
                        }

                        // The name can be anywhere in a `@param[in] a, b` list.
                        const auto text = Joined(command);
                        for (std::size_t at = text.find(item.name); at != std::string::npos;
                            at = text.find(item.name, at + 1))
                        {
                            const bool left = at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) != 0 ||
                                text[at - 1] == '_');
                            const auto stop = at + item.name.size();
                            const bool right = stop >= text.size() ||
                                !(std::isalnum(static_cast<unsigned char>(text[stop])) != 0 || text[stop] == '_');
                            if (left && right)
                            {
                                return true;
                            }
                        }

                        return false;
                    });
                if (!documented)
                {
                    out.push_back(analysis.MakeAtName(RuleId::DocDoxygenStyle, "doc/doxygen-style",
                        std::string(what) + " '" + std::string(item.name) + "' is not documented; add @" +
                            std::string(tag) + " " + std::string(item.name),
                        item.position));
                }
            }
        }

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeRequireDocComment(const SemanticModel &model, DocScope scope)
    {
        DocAnalysis analysis(model, scope);
        std::vector<Diagnostic> diagnostics;
        const auto &symbols = model.Symbols();
        Entity entity;
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (!analysis.Describe(symbol, entity, true))
            {
                continue;
            }

            const auto doc = analysis.DocOf(entity);
            if (doc.present && !IsDetached(ParseCommands(doc)))
            {
                continue;
            }

            diagnostics.push_back(analysis.MakeAtName(RuleId::DocRequireComment, "doc/require-comment",
                std::string(analysis.KindWord(entity)) + " '" + analysis.NameOf(entity) +
                    "' has no documentation comment; add a /** */ or /// block with a @brief",
                entity.name));
        }

        SortByOffset(diagnostics);
        return diagnostics;
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeDoxygenStyle(const SemanticModel &model, DocScope scope)
    {
        DocAnalysis analysis(model, scope);
        std::vector<Diagnostic> diagnostics;
        const auto &symbols = model.Symbols();
        Entity entity;
        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (!analysis.Describe(symbol, entity, false))
            {
                continue;
            }

            const auto doc = analysis.DocOf(entity);
            if (!doc.present)
            {
                continue;
            }

            const auto commands = ParseCommands(doc);
            if (IsDetached(commands) || IsInherited(commands))
            {
                continue;
            }

            const auto report = [&](std::string message, std::size_t line = 0)
            {
                const auto &where = doc.lines[std::min(line, doc.lines.size() - 1)];
                diagnostics.push_back(analysis.Make(RuleId::DocDoxygenStyle, "doc/doxygen-style", std::move(message),
                    where.offset, where.length));
            };

            if (doc.qt_style)
            {
                report("use Javadoc style (/** */ or ///) instead of /*! */ and //!");
            }

            const bool at = std::any_of(commands.begin(), commands.end(),
                [](const Command &command)
                {
                    return command.at;
                });
            const bool slash = std::any_of(commands.begin(), commands.end(),
                [](const Command &command)
                {
                    return !command.at;
                });
            if (at && slash)
            {
                report("comment mixes @command and \\command spellings; pick one");
            }

            // Brief: one sentence, then a blank line.
            const auto *brief = Find(commands, {"brief", "short"});
            if (brief == nullptr)
            {
                if (Find(commands, {"copybrief"}) == nullptr)
                {
                    report("no @brief; start with a one-sentence summary");
                }
            }
            else
            {
                if (Joined(*brief).empty())
                {
                    report("@brief has no text", brief->line);
                }

                bool broke = false;
                for (std::size_t k = 0; k + 1 < brief->parts.size() && !broke; ++k)
                {
                    const auto part = Trim(brief->parts[k]);
                    broke = !part.empty() && (part.back() == '.' || part.back() == '!' || part.back() == '?');
                }

                if (broke)
                {
                    report("separate the @brief from the detailed description with a blank line", brief->line);
                }
                else if (std::any_of(brief->parts.begin(), brief->parts.end(),
                             [](const std::string &part)
                             {
                                 return HasSecondSentence(part);
                             }))
                {
                    report("@brief should be a single sentence; move the rest to the detailed description",
                        brief->line);
                }
            }

            CheckNamed(analysis, diagnostics, doc, commands, "tparam", entity.tparams, entity.tparams_known,
                "template parameter");
            if (!entity.function)
            {
                continue;
            }

            CheckNamed(analysis, diagnostics, doc, commands, "param", entity.params, entity.params_known, "parameter");

            const auto *returns = Find(commands, {"return", "returns", "result", "retval"});
            if (entity.returns == Tri::Yes && returns == nullptr)
            {
                diagnostics.push_back(analysis.MakeAtName(RuleId::DocDoxygenStyle, "doc/doxygen-style",
                    "'" + analysis.NameOf(entity) + "' returns a value but has no @return", entity.name));
            }
            else if (entity.returns == Tri::No && returns != nullptr)
            {
                report("'" + analysis.NameOf(entity) + "' returns nothing; remove @" + returns->name, returns->line);
            }
            else if (returns != nullptr && returns->name != "retval" && Joined(*returns).empty())
            {
                report("@" + returns->name + " has no description", returns->line);
            }

            const auto *throws = Find(commands, {"throw", "throws", "exception"});
            if (throws == nullptr && entity.throws)
            {
                diagnostics.push_back(analysis.MakeAtName(RuleId::DocDoxygenStyle, "doc/doxygen-style",
                    "'" + analysis.NameOf(entity) + "' can throw but has no @throws", entity.name));
            }
            else if (throws != nullptr)
            {
                const auto text = Joined(*throws);
                const auto space = text.find(' ');
                if (space == std::string::npos || Trim(std::string_view(text).substr(space)).empty())
                {
                    report("@" + throws->name + " needs the exception type and when it is thrown", throws->line);
                }
            }
        }

        SortByOffset(diagnostics);
        return diagnostics;
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeDocumentation(const SemanticModel &model, const RuleEngine &engine)
    {
        std::vector<Diagnostic> all;
        const auto scope = engine.DocumentationScope();
        if (engine.OptInEnabled("doc/require-comment"))
        {
            Append(all, AnalyzeRequireDocComment(model, scope));
        }

        if (engine.OptInEnabled("doc/doxygen-style"))
        {
            Append(all, AnalyzeDoxygenStyle(model, scope));
        }

        SortByOffset(all);
        return all;
    }

} // namespace heimdall
