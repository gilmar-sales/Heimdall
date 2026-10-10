#include <Heimdall/SymbolOutline.hpp>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace heimdall
{

    namespace
    {

        constexpr std::size_t kNone           = static_cast<std::size_t>(-1);
        constexpr std::size_t kMaxDetailBytes = 160;
        constexpr std::string_view kEllipsis  = "...";
        constexpr std::string_view kScopeSeparator = "::";
        constexpr int kShiftRightDepth             = 2;

        struct DeclaratorSite
        {
            std::size_t node      = kNone;
            std::size_t rangeNode = kNone;
            std::size_t nameToken = kNone;
            bool detachedInit     = false;
        };

        class Extractor
        {
        public:
            explicit Extractor(const ParseTree& tree) :
                mTree(tree), mNodes(tree.NodesSoA()), mTokens(tree.Tokens())
            {
            }

            std::vector<OutlineSymbol> Run()
            {
                struct Frame
                {
                    std::size_t end;
                    std::uint32_t symbol;
                };

                std::vector<Frame> scopes;
                const std::size_t count = mNodes.size();
                std::size_t node        = 1;
                while (node < count)
                {
                    while (!scopes.empty() && node >= scopes.back().end)
                    {
                        scopes.pop_back();
                    }

                    const std::uint32_t parent =
                        scopes.empty() ? kNoOutlineParent : scopes.back().symbol;
                    std::size_t next = node + 1;
                    switch (mNodes.Kind(node))
                    {
                    case GrammarKind::NamespaceDefinition:
                    case GrammarKind::RecordDefinition:
                        if (const auto symbol = EmitScope(node, parent))
                        {
                            scopes.emplace_back(SubtreeEnd(node), *symbol);
                        }
                        break;
                    case GrammarKind::TemplateDeclaration:
                    case GrammarKind::LanguageLinkageSpec:
                        break;
                    case GrammarKind::Declaration:
                    case GrammarKind::FunctionDeclaration:
                    case GrammarKind::FunctionDefinition:
                        EmitDeclaration(node, parent);
                        next = SubtreeEnd(node);
                        break;
                    case GrammarKind::UsingDeclaration:
                        EmitAlias(node, parent);
                        next = SubtreeEnd(node);
                        break;
                    case GrammarKind::ConceptDefinition:
                        EmitConcept(node, parent);
                        next = SubtreeEnd(node);
                        break;
                    case GrammarKind::Enumerator:
                        EmitEnumerator(node, parent);
                        next = SubtreeEnd(node);
                        break;
                    default:
                        next = SubtreeEnd(node);
                        break;
                    }

                    node = std::max(next, node + 1);
                }

                return std::move(mSymbols);
            }

        private:
            [[nodiscard]] std::size_t SubtreeEnd(std::size_t node) const noexcept
            {
                return mNodes.SubtreeEnd(node);
            }

            [[nodiscard]] static bool IsTrivia(TokenKind kind) noexcept
            {
                return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
                       kind == TokenKind::BlockComment;
            }

            [[nodiscard]] std::string_view Text(std::size_t token) const noexcept
            {
                return mTree.Text(mTokens[token]);
            }

            [[nodiscard]] bool Is(std::size_t token, std::string_view text) const noexcept
            {
                return token < mTokens.size() && !IsTrivia(mTokens[token].kind) &&
                       Text(token) == text;
            }

            [[nodiscard]] bool IsIdentifier(std::size_t token) const noexcept
            {
                return token < mTokens.size() && mTokens[token].kind == TokenKind::Identifier;
            }

            [[nodiscard]] std::size_t NextSig(std::size_t token, std::size_t limit) const noexcept
            {
                for (++token; token < limit && token < mTokens.size(); ++token)
                {
                    if (!IsTrivia(mTokens[token].kind))
                    {
                        return token;
                    }
                }

                return kNone;
            }

            [[nodiscard]] std::size_t PrevSig(std::size_t token, std::size_t floor) const noexcept
            {
                while (token > floor)
                {
                    --token;
                    if (!IsTrivia(mTokens[token].kind))
                    {
                        return token;
                    }
                }

                return kNone;
            }

            [[nodiscard]] std::size_t NodeEndToken(std::size_t node) const noexcept
            {
                return std::min<std::size_t>(
                    static_cast<std::size_t>(mNodes.FirstToken(node)) + mNodes.TokenCount(node),
                    mTokens.size());
            }

            [[nodiscard]] std::size_t FirstSig(std::size_t node) const noexcept
            {
                const std::size_t begin = mNodes.FirstToken(node);
                if (begin >= mTokens.size())
                {
                    return kNone;
                }

                if (!IsTrivia(mTokens[begin].kind))
                {
                    return begin;
                }

                return NextSig(begin, NodeEndToken(node));
            }

            [[nodiscard]] std::size_t LastSig(std::size_t node) const noexcept
            {
                return PrevSig(NodeEndToken(node), mNodes.FirstToken(node));
            }

            [[nodiscard]] std::size_t TokenEnd(std::size_t token) const noexcept
            {
                return static_cast<std::size_t>(mTokens[token].offset) + mTokens[token].length;
            }

            // Source text of [first, last] with comments dropped and each run of whitespace
            // folded to one space; stops with an ellipsis at a token boundary past the cap.
            [[nodiscard]] std::string Collapsed(std::size_t first, std::size_t last) const
            {
                std::string text;
                if (first == kNone || last == kNone)
                {
                    return text;
                }

                bool space = false;
                for (std::size_t token = first; token <= last && token < mTokens.size(); ++token)
                {
                    const TokenKind kind = mTokens[token].kind;
                    if (kind == TokenKind::LineComment || kind == TokenKind::BlockComment)
                    {
                        continue;
                    }

                    if (kind == TokenKind::Whitespace)
                    {
                        space = !text.empty();
                        continue;
                    }

                    if (text.size() >= kMaxDetailBytes)
                    {
                        text += space ? " " : "";
                        text += kEllipsis;
                        break;
                    }

                    if (space)
                    {
                        text += ' ';
                        space = false;
                    }

                    text += Text(token);
                }

                return text;
            }

            [[nodiscard]] std::size_t LiftTemplate(std::size_t node) const noexcept
            {
                while (node != 0)
                {
                    const std::size_t parent = mNodes.Parent(node);
                    if (parent >= node || mNodes.Kind(parent) != GrammarKind::TemplateDeclaration)
                    {
                        break;
                    }

                    node = parent;
                }

                return node;
            }

            void SetRange(OutlineSymbol& symbol, std::size_t node) const
            {
                const std::size_t first = FirstSig(node);
                const std::size_t last  = LastSig(node);
                if (first == kNone || last == kNone || last < first)
                {
                    symbol.rangeOffset = symbol.nameOffset;
                    symbol.rangeLength = symbol.nameLength;
                    return;
                }

                symbol.rangeOffset = mTokens[first].offset;
                symbol.rangeLength = TokenEnd(last) - symbol.rangeOffset;
            }

            std::uint32_t Add(OutlineSymbol symbol, std::uint32_t parent)
            {
                symbol.parent = parent;
                if (parent != kNoOutlineParent)
                {
                    const OutlineSymbol& owner = mSymbols[parent];
                    symbol.container =
                        owner.container.empty() ? owner.name : owner.container + "::" + owner.name;
                }

                mSymbols.emplace_back(std::move(symbol));
                return static_cast<std::uint32_t>(mSymbols.size() - 1);
            }

            [[nodiscard]] bool InRecord(std::uint32_t parent) const noexcept
            {
                if (parent == kNoOutlineParent)
                {
                    return false;
                }

                const OutlineKind kind = mSymbols[parent].kind;
                return kind == OutlineKind::Class || kind == OutlineKind::Struct ||
                       kind == OutlineKind::Union;
            }

            // Skips `[[...]]` starting at the `[` token; returns the token after it.
            [[nodiscard]] std::size_t SkipBrackets(std::size_t open, std::size_t limit) const
            {
                int depth = 0;
                for (std::size_t token = open; token < limit && token < mTokens.size(); ++token)
                {
                    if (Is(token, "["))
                    {
                        ++depth;
                    }
                    else if (Is(token, "]") && --depth == 0)
                    {
                        return token + 1;
                    }
                }

                return limit;
            }

            [[nodiscard]] std::size_t SkipParens(std::size_t open, std::size_t limit) const
            {
                int depth = 0;
                for (std::size_t token = open; token < limit && token < mTokens.size(); ++token)
                {
                    if (Is(token, "("))
                    {
                        ++depth;
                    }
                    else if (Is(token, ")") && --depth == 0)
                    {
                        return token + 1;
                    }
                }

                return limit;
            }

            // Closing `>` of the template-argument list opened at `open`, or kNone.
            [[nodiscard]] std::size_t MatchAngle(std::size_t open, std::size_t limit) const
            {
                int depth = 0;
                for (std::size_t token = open; token < limit && token < mTokens.size(); ++token)
                {
                    if (IsTrivia(mTokens[token].kind))
                    {
                        continue;
                    }

                    const std::string_view text = Text(token);
                    if (text == "<")
                    {
                        ++depth;
                    }
                    else if (text == ">")
                    {
                        if (--depth == 0)
                        {
                            return token;
                        }
                    }
                    else if (text == ">>")
                    {
                        depth -= kShiftRightDepth;
                        if (depth <= 0)
                        {
                            return depth == 0 ? token : kNone;
                        }
                    }
                    else if (text == "{" || text == ";")
                    {
                        break;
                    }
                }

                return kNone;
            }

            [[nodiscard]] static std::optional<OutlineKind> ScopeKeyword(std::string_view word)
            {
                if (word == "namespace")
                {
                    return OutlineKind::Namespace;
                }

                if (word == "class")
                {
                    return OutlineKind::Class;
                }

                if (word == "struct")
                {
                    return OutlineKind::Struct;
                }

                if (word == "union")
                {
                    return OutlineKind::Union;
                }

                if (word == "enum")
                {
                    return OutlineKind::Enum;
                }

                return std::nullopt;
            }

            [[nodiscard]] static std::string_view AnonymousName(OutlineKind kind) noexcept
            {
                switch (kind)
                {
                case OutlineKind::Namespace:
                    return "(anonymous namespace)";
                case OutlineKind::Class:
                    return "(anonymous class)";
                case OutlineKind::Union:
                    return "(anonymous union)";
                case OutlineKind::Enum:
                    return "(anonymous enum)";
                default:
                    return "(anonymous struct)";
                }
            }

            std::optional<std::uint32_t> EmitScope(std::size_t node, std::uint32_t parent)
            {
                const std::size_t end = NodeEndToken(node);
                std::size_t body      = end;
                for (std::size_t token = mNodes.FirstToken(node); token < end; ++token)
                {
                    if (Is(token, "{"))
                    {
                        body = token;
                        break;
                    }
                }

                std::size_t keyword = kNone;
                OutlineKind kind    = OutlineKind::Struct;
                for (std::size_t token = FirstSig(node); token != kNone && token < body;
                     token          = NextSig(token, body))
                {
                    if (!IsIdentifier(token))
                    {
                        continue;
                    }

                    if (const auto found = ScopeKeyword(Text(token)))
                    {
                        keyword = token;
                        kind    = *found;
                        break;
                    }
                }

                if (keyword == kNone)
                {
                    return std::nullopt;
                }

                std::size_t cursor = NextSig(keyword, body);
                if (kind == OutlineKind::Enum && cursor != kNone &&
                    (Is(cursor, "class") || Is(cursor, "struct")))
                {
                    cursor = NextSig(cursor, body);
                }

                std::size_t nameFirst = kNone;
                std::size_t nameLast  = kNone;
                while (cursor != kNone && cursor < body)
                {
                    if (Is(cursor, "["))
                    {
                        cursor = NextSigOrSelf(SkipBrackets(cursor, body), body);
                        continue;
                    }

                    if (!IsIdentifier(cursor))
                    {
                        break;
                    }

                    if (Is(cursor, "alignas"))
                    {
                        const std::size_t open = NextSig(cursor, body);
                        cursor = open != kNone && Is(open, "(")
                                     ? NextSigOrSelf(SkipParens(open, body), body)
                                     : open;
                        continue;
                    }

                    const std::size_t after = NextSig(cursor, body);
                    if (mTree.IsDecorationToken(cursor) ||
                        (after != kNone && IsIdentifier(after) && !Is(after, "final")))
                    {
                        cursor = after;
                        continue;
                    }

                    nameFirst = cursor;
                    nameLast  = cursor;
                    while (true)
                    {
                        const std::size_t separator = NextSig(nameLast, body);
                        if (separator == kNone || !Is(separator, "::"))
                        {
                            break;
                        }

                        const std::size_t member = NextSig(separator, body);
                        if (member == kNone || !IsIdentifier(member))
                        {
                            break;
                        }

                        nameLast = member;
                    }

                    const std::size_t angle = NextSig(nameLast, body);
                    if (angle != kNone && Is(angle, "<"))
                    {
                        if (const std::size_t close = MatchAngle(angle, body); close != kNone)
                        {
                            nameLast = close;
                        }
                    }

                    break;
                }

                OutlineSymbol symbol;
                symbol.kind = kind;
                if (nameFirst != kNone)
                {
                    symbol.name       = Collapsed(nameFirst, nameLast);
                    symbol.nameOffset = mTokens[nameFirst].offset;
                    symbol.nameLength = TokenEnd(nameLast) - symbol.nameOffset;
                }
                else
                {
                    symbol.name       = std::string(AnonymousName(kind));
                    symbol.nameOffset = mTokens[keyword].offset;
                    symbol.nameLength = mTokens[keyword].length;
                }

                SetRange(symbol, LiftTemplate(node));
                return Add(std::move(symbol), parent);
            }

            // First significant token at or after `token`.
            [[nodiscard]] std::size_t NextSigOrSelf(std::size_t token, std::size_t limit) const noexcept
            {
                while (token < limit && token < mTokens.size())
                {
                    if (!IsTrivia(mTokens[token].kind))
                    {
                        return token;
                    }

                    ++token;
                }

                return kNone;
            }

            [[nodiscard]] bool HasKeywordBefore(std::size_t node,
                                                std::size_t stop,
                                                std::string_view keyword) const noexcept
            {
                for (std::size_t token = FirstSig(node); token != kNone && token < stop;
                     token             = NextSig(token, stop))
                {
                    if (Is(token, keyword))
                    {
                        return true;
                    }
                }

                return false;
            }

            [[nodiscard]] std::size_t FindChild(std::size_t node, GrammarKind kind) const noexcept
            {
                for (const std::size_t child : mTree.DirectChildren(node))
                {
                    if (mNodes.Kind(child) == kind)
                    {
                        return child;
                    }
                }

                return kNone;
            }

            // First DeclaredName of a declarator, ignoring those inside parameter lists,
            // array bounds and bit-field widths.
            [[nodiscard]] std::size_t FindDeclaredName(std::size_t declarator) const noexcept
            {
                const std::size_t end = SubtreeEnd(declarator);
                for (std::size_t node = declarator + 1; node < end;)
                {
                    switch (mNodes.Kind(node))
                    {
                    case GrammarKind::DeclaredName:
                        return node;
                    case GrammarKind::FunctionSuffix:
                    case GrammarKind::ArraySuffix:
                    case GrammarKind::BitfieldSuffix:
                    case GrammarKind::TrailingReturnType:
                    case GrammarKind::NoexceptSpecifier:
                        node = std::max(SubtreeEnd(node), node + 1);
                        break;
                    default:
                        ++node;
                        break;
                    }
                }

                return kNone;
            }

            [[nodiscard]] static std::string_view BaseName(std::string_view name) noexcept
            {
                if (const auto angle = name.find('<'); angle != std::string_view::npos)
                {
                    name = name.substr(0, angle);
                }

                if (const auto scope = name.rfind("::"); scope != std::string_view::npos)
                {
                    name.remove_prefix(scope + kScopeSeparator.size());
                }

                return name;
            }

            void EmitDeclaration(std::size_t node, std::uint32_t parent)
            {
                std::vector<DeclaratorSite> declarators;
                std::size_t initializers = 0;
                for (const std::size_t child : mTree.DirectChildren(node))
                {
                    const GrammarKind kind = mNodes.Kind(child);
                    if (kind == GrammarKind::Declarator)
                    {
                        declarators.emplace_back(child, node, kNone, false);
                    }
                    else if (kind == GrammarKind::InitDeclarator)
                    {
                        const std::size_t declarator = FindChild(child, GrammarKind::Declarator);
                        if (declarator != kNone)
                        {
                            declarators.emplace_back(declarator, child, kNone, false);
                        }
                        else if (initializers > 0)
                        {
                            // `int a, b;` leaves `b` without a Declarator node.
                            if (const std::size_t first = FirstSig(child);
                                first != kNone && IsIdentifier(first))
                            {
                                declarators.emplace_back(kNone, child, first, true);
                            }
                        }

                        ++initializers;
                    }
                }

                if (declarators.empty())
                {
                    return;
                }

                const std::size_t firstDeclared = declarators.front().node != kNone
                                                      ? mNodes.FirstToken(declarators.front().node)
                                                      : declarators.front().nameToken;
                if (HasKeywordBefore(node, firstDeclared, "friend"))
                {
                    return;
                }

                const bool isTypedef = HasKeywordBefore(node, firstDeclared, "typedef");
                const bool single    = declarators.size() == 1;
                for (const DeclaratorSite& declarator : declarators)
                {
                    EmitDeclarator(node, declarator, single, isTypedef, parent);
                }
            }

            [[nodiscard]] std::string TypeDetail(std::size_t declaration, bool isTypedef) const
            {
                const std::size_t type = FindChild(declaration, GrammarKind::TypeSpecifier);
                if (type == kNone)
                {
                    return {};
                }

                std::size_t first = FirstSig(type);
                if (isTypedef && first != kNone && Is(first, "typedef"))
                {
                    first = NextSig(first, NodeEndToken(type));
                }

                return Collapsed(first, LastSig(type));
            }

            void EmitDeclarator(std::size_t declaration,
                                const DeclaratorSite& declarator,
                                bool single,
                                bool isTypedef,
                                std::uint32_t parent)
            {
                OutlineSymbol symbol;
                const std::size_t rangeNode = single ? declaration : declarator.rangeNode;
                if (declarator.detachedInit)
                {
                    symbol.name       = std::string(Text(declarator.nameToken));
                    symbol.nameOffset = mTokens[declarator.nameToken].offset;
                    symbol.nameLength = mTokens[declarator.nameToken].length;
                    symbol.kind       = isTypedef ? OutlineKind::TypeAlias
                                        : InRecord(parent) ? OutlineKind::Field
                                                           : OutlineKind::Variable;
                    symbol.detail     = TypeDetail(declaration, isTypedef);
                    SetRange(symbol, LiftTemplate(rangeNode));
                    Add(std::move(symbol), parent);
                    return;
                }

                const std::size_t declaredName = FindDeclaredName(declarator.node);
                if (declaredName == kNone)
                {
                    return;
                }

                const std::size_t nameToken = mNodes.FirstToken(declaredName);
                if (nameToken >= mTokens.size() || !IsIdentifier(nameToken))
                {
                    return;
                }

                const std::size_t owner  = mNodes.Parent(declaredName);
                const std::size_t suffix = FindChild(owner, GrammarKind::FunctionSuffix);
                const bool isFunction    = suffix != kNone && !isTypedef;
                const bool isOperator    = Text(nameToken) == "operator";

                std::size_t nameFirst = nameToken;
                std::size_t nameLast  = nameToken;
                if (isFunction && isOperator)
                {
                    const std::size_t before = PrevSig(mNodes.FirstToken(suffix), nameToken);
                    if (before != kNone && before > nameToken)
                    {
                        nameLast = before;
                    }
                }

                bool destructor = false;
                if (const std::size_t tilde = PrevSig(nameToken, 0);
                    tilde != kNone && Is(tilde, "~"))
                {
                    nameFirst  = tilde;
                    destructor = true;
                }

                std::size_t displayFirst = nameFirst;
                bool qualified           = false;
                if (const std::size_t qualifier = FindChild(owner, GrammarKind::NestedNameSpecifier);
                    qualifier != kNone)
                {
                    if (const std::size_t first = FirstSig(qualifier);
                        first != kNone && first < displayFirst)
                    {
                        displayFirst = first;
                        qualified    = true;
                    }
                }

                symbol.name       = Collapsed(displayFirst, nameLast);
                symbol.nameOffset = mTokens[nameFirst].offset;
                symbol.nameLength = TokenEnd(nameLast) - symbol.nameOffset;

                if (isTypedef)
                {
                    symbol.kind   = OutlineKind::TypeAlias;
                    symbol.detail = TypeDetail(declaration, true);
                }
                else if (isFunction)
                {
                    const std::size_t suffixFirst = FirstSig(suffix);
                    symbol.detail                 = Collapsed(suffixFirst, LastSig(suffix));
                    symbol.kind =
                        ClassifyFunction(nameToken, nameFirst, qualified, destructor, isOperator, parent);
                }
                else
                {
                    symbol.kind   = InRecord(parent) ? OutlineKind::Field : OutlineKind::Variable;
                    symbol.detail = TypeDetail(declaration, false);
                }

                SetRange(symbol, LiftTemplate(single ? declaration : declarator.rangeNode));
                Add(std::move(symbol), parent);
            }

            [[nodiscard]] OutlineKind ClassifyFunction(std::size_t nameToken,
                                                      std::size_t nameFirst,
                                                      bool qualified,
                                                      bool destructor,
                                                      bool isOperator,
                                                      std::uint32_t parent) const
            {
                if (isOperator)
                {
                    return OutlineKind::Operator;
                }

                const bool member = qualified || InRecord(parent);
                if (!member)
                {
                    return OutlineKind::Function;
                }

                if (destructor)
                {
                    return OutlineKind::Destructor;
                }

                std::string_view owner;
                if (qualified)
                {
                    const std::size_t separator = PrevSig(nameFirst, 0);
                    const std::size_t previous =
                        separator != kNone && Is(separator, "::") ? PrevSig(separator, 0) : kNone;
                    if (previous != kNone && IsIdentifier(previous))
                    {
                        owner = Text(previous);
                    }
                }
                else
                {
                    owner = BaseName(mSymbols[parent].name);
                }

                return !owner.empty() && owner == Text(nameToken) ? OutlineKind::Constructor
                                                                  : OutlineKind::Method;
            }

            void EmitAlias(std::size_t node, std::uint32_t parent)
            {
                const std::size_t end   = NodeEndToken(node);
                const std::size_t first = FirstSig(node);
                if (first == kNone || !Is(first, "using"))
                {
                    return;
                }

                const std::size_t name = NextSig(first, end);
                if (name == kNone || !IsIdentifier(name) || Is(name, "namespace"))
                {
                    return;
                }

                const std::size_t assign = NextSig(name, end);
                if (assign == kNone || !Is(assign, "="))
                {
                    return;
                }

                OutlineSymbol symbol;
                symbol.kind       = OutlineKind::TypeAlias;
                symbol.name       = std::string(Text(name));
                symbol.nameOffset = mTokens[name].offset;
                symbol.nameLength = mTokens[name].length;
                std::size_t last  = LastSig(node);
                if (last != kNone && Is(last, ";"))
                {
                    last = PrevSig(last, assign);
                }

                if (last != kNone && last > assign)
                {
                    symbol.detail = Collapsed(NextSig(assign, end), last);
                }

                SetRange(symbol, LiftTemplate(node));
                Add(std::move(symbol), parent);
            }

            void EmitConcept(std::size_t node, std::uint32_t parent)
            {
                const std::size_t end   = NodeEndToken(node);
                const std::size_t first = FirstSig(node);
                if (first == kNone || !Is(first, "concept"))
                {
                    return;
                }

                const std::size_t name = NextSig(first, end);
                if (name == kNone || !IsIdentifier(name))
                {
                    return;
                }

                OutlineSymbol symbol;
                symbol.kind       = OutlineKind::Concept;
                symbol.name       = std::string(Text(name));
                symbol.nameOffset = mTokens[name].offset;
                symbol.nameLength = mTokens[name].length;
                SetRange(symbol, LiftTemplate(node));
                Add(std::move(symbol), parent);
            }

            void EmitEnumerator(std::size_t node, std::uint32_t parent)
            {
                if (parent == kNoOutlineParent || mSymbols[parent].kind != OutlineKind::Enum)
                {
                    return;
                }

                const std::size_t declaredName = FindChild(node, GrammarKind::DeclaredName);
                if (declaredName == kNone)
                {
                    return;
                }

                const std::size_t name = mNodes.FirstToken(declaredName);
                if (name >= mTokens.size() || !IsIdentifier(name))
                {
                    return;
                }

                OutlineSymbol symbol;
                symbol.kind       = OutlineKind::EnumMember;
                symbol.name       = std::string(Text(name));
                symbol.nameOffset = mTokens[name].offset;
                symbol.nameLength = mTokens[name].length;
                SetRange(symbol, node);
                Add(std::move(symbol), parent);
            }

            const ParseTree& mTree;
            const GrammarNodeSoA& mNodes;
            const std::vector<Token>& mTokens;
            std::vector<OutlineSymbol> mSymbols;
        };

    } // namespace

    std::vector<OutlineSymbol> SymbolOutline::Extract(const ParseTree& tree)
    {
        return Extractor(tree).Run();
    }

} // namespace heimdall
