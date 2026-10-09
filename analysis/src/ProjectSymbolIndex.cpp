#include <Heimdall/ProjectSymbolIndex.hpp>
#include <Heimdall/Workspace.hpp>

#include <algorithm>
#include <unordered_set>

namespace heimdall
{
    namespace
    {
        std::uint64_t LocalKey(DocumentId document, SymbolId symbol)
        {
            return (static_cast<std::uint64_t>(document) << 32) | symbol;
        }

        void Part(std::string& key, std::string_view text)
        {
            key += std::to_string(text.size()) + ":";
            key += text;
        }

        std::string PathKey(const std::filesystem::path& path)
        {
            std::error_code error;
            auto            absolute = std::filesystem::absolute(path, error);
            auto            text = (error ? path : absolute).lexically_normal().generic_string();
#ifdef _WIN32
            for (auto& c : text)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c += 'a' - 'A';
                }
            }
#endif
            return text;
        }

        std::string Configuration(const AnalysisSnapshot& snapshot, DocumentId document)
        {
            const auto& options = snapshot.Options(document);
            std::string result;
            Part(result, std::to_string(static_cast<unsigned>(options.standard)));
            std::vector<std::pair<std::string, std::string>> macros;
            for (const auto& [name, value] : options.Macros())
            {
                macros.emplace_back(name, value);
            }

            std::ranges::sort(macros);
            Part(result, std::to_string(macros.size()));
            for (const auto& [name, value] : macros)
            {
                Part(result, name);
                Part(result, value);
            }

            if (const auto command = snapshot.Command(document))
            {
                Part(result, "quote");
                Part(result, std::to_string(command->quote_directories.size()));
                for (const auto& path : command->quote_directories)
                {
                    Part(result, PathKey(path));
                }

                Part(result, "include");
                Part(result, std::to_string(command->include_directories.size()));
                for (const auto& path : command->include_directories)
                {
                    Part(result, PathKey(path));
                }

                Part(result, PathKey(command->directory));
                for (std::size_t i = 0; i < command->arguments.size(); ++i)
                {
                    const auto& argument = command->arguments[i];
                    if (argument == "-o" || argument == "-MF" || argument == "-MT" ||
                        argument == "-MQ")
                    {
                        if (i + 1 < command->arguments.size())
                        {
                            ++i;
                        }

                        continue;
                    }

                    if (argument == "-c" || argument == "-MD" || argument == "-MMD" ||
                        argument == "-MP" || argument.starts_with("-o") ||
                        argument.starts_with("-MF") || argument.starts_with("-MT") ||
                        argument.starts_with("-MQ") || argument.starts_with("-fdeps-"))
                    {
                        continue;
                    }

                    const auto path = std::filesystem::path(argument);
                    if (i > 0 && !argument.starts_with('-') &&
                        PathKey(path.is_absolute() ? path : command->directory / path) ==
                            PathKey(command->file))
                    {
                        continue;
                    }

                    Part(result, argument);
                }
            }

            return result;
        }

        bool Trivia(const Token& token)
        {
            return token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
                   token.kind == TokenKind::BlockComment;
        }

        struct ScopeName
        {
            std::string text;
            bool        local = false, internal = false, member = false;
        };

        ScopeName QualifiedScope(const SemanticModel& model, ScopeId scope,
                                 bool lexical_only = false)
        {
            ScopeName                     result;
            std::vector<std::string_view> components;
            const auto&                   scopes = model.Scopes();
            while (scope < scopes.Size() && scope != SemanticModel::TranslationUnitScope)
            {
                const auto kind = scopes.kind[scope];
                result.local |= kind == ScopeKind::Block || kind == ScopeKind::Function;
                result.member |= kind == ScopeKind::Class;
                const auto owner = scopes.owner[scope];
                if (kind == ScopeKind::Namespace &&
                    (owner == kNone || model.Names().Text(model.Symbols().name[owner]).empty()))
                {
                    result.internal = true;
                }

                if (owner != kNone &&
                    (!lexical_only || kind == ScopeKind::Namespace || kind == ScopeKind::Class))
                {
                    components.push_back(model.Names().Text(model.Symbols().name[owner]));
                }

                const auto parent = scopes.parent[scope];
                if (parent == scope)
                {
                    break;
                }

                scope = parent;
            }

            for (auto it = components.rbegin(); it != components.rend(); ++it)
                if (!it->empty())
                {
                    result.text += *it;
                    result.text += "::";
                }

            return result;
        }

        std::optional<std::string> ScalarParameter(const SemanticModel& model, NodeId parameter)
        {
            const auto&                       tree = model.Tree();
            const auto&                       soa  = tree.NodesSoA();
            std::unordered_set<std::uint32_t> names;
            for (auto i = parameter; i < soa.SubtreeEnd(parameter); ++i)
            {
                if (soa.Kind(i) == GrammarKind::DeclaredName)
                {
                    names.insert(soa.FirstToken(i));
                }
            }

            std::string type;
            for (auto i = soa.FirstToken(parameter);
                 i < soa.FirstToken(parameter) + soa.TokenCount(parameter); ++i)
            {
                const auto& token = tree.Tokens()[i];
                if (token.tok == Tok::Eq)
                {
                    break;
                }

                if (Trivia(token) || names.contains(i))
                {
                    continue;
                }

                switch (token.tok)
                {
                    case Tok::KwVoid:
                    case Tok::KwBool:
                    case Tok::KwChar:
                    case Tok::KwWchar:
                    case Tok::KwChar8:
                    case Tok::KwChar16:
                    case Tok::KwChar32:
                    case Tok::KwShort:
                    case Tok::KwInt:
                    case Tok::KwLong:
                    case Tok::KwSigned:
                    case Tok::KwUnsigned:
                    case Tok::KwFloat:
                    case Tok::KwDouble:
                        break;
                    default:
                        return std::nullopt;
                }

                if (!type.empty())
                {
                    type += ' ';
                }

                type += tree.Text(token);
            }

            if (type == "void")
            {
                return names.empty() ? std::optional(type) : std::nullopt;
            }

            // Canonical builtin synonyms. Aliases and cv/ref adjustment need a
            // richer canonical type model, so unknown spellings are blocked.
            if (type == "signed" || type == "signed int")
            {
                type = "int";
            }
            else if (type == "unsigned")
            {
                type = "unsigned int";
            }
            else if (type == "short int" || type == "signed short" || type == "signed short int")
            {
                type = "short";
            }
            else if (type == "unsigned short int")
            {
                type = "unsigned short";
            }
            else if (type == "long int" || type == "signed long" || type == "signed long int")
            {
                type = "long";
            }
            else if (type == "unsigned long int")
            {
                type = "unsigned long";
            }
            else if (type == "long long int" || type == "signed long long" ||
                     type == "signed long long int")
            {
                type = "long long";
            }
            else if (type == "unsigned long long int")
            {
                type = "unsigned long long";
            }

            constexpr std::string_view builtin[] = {
                "bool",    "char",           "signed char", "unsigned char",
                "wchar_t", "char8_t",        "char16_t",    "char32_t",
                "short",   "unsigned short", "int",         "unsigned int",
                "long",    "unsigned long",  "long long",   "unsigned long long",
                "float",   "double",         "long double"
            };
            return std::ranges::find(builtin, type) == std::end(builtin)
                       ? std::nullopt
                       : std::optional(type);
        }

        bool Signature(const SemanticModel& model, SymbolId symbol, std::string& result)
        {
            const auto& tree   = model.Tree();
            const auto& soa    = tree.NodesSoA();
            const auto  node   = model.Symbols().decl_node[symbol];
            NodeId      suffix = InvalidNode;
            for (auto i = node; i < soa.SubtreeEnd(node); ++i)
            {
                if (soa.Kind(i) == GrammarKind::CompoundStatement)
                {
                    break;
                }

                if (soa.Kind(i) == GrammarKind::FunctionSuffix)
                {
                    suffix = i;
                    break;
                }
            }

            if (suffix == InvalidNode)
            {
                return false;
            }

            for (auto token = soa.FirstToken(suffix);
                 token < soa.FirstToken(suffix) + soa.TokenCount(suffix); ++token)
            {
                if (tree.Tokens()[token].tok == Tok::Ellipsis)
                {
                    return false;
                }
            }

            result = "(";
            for (auto parameter : model.ChildrenOf(suffix))
            {
                if (soa.Kind(parameter) != GrammarKind::ParameterDeclaration)
                {
                    continue;
                }

                const auto type = ScalarParameter(model, parameter);
                if (!type)
                {
                    return false;
                }

                if (*type != "void")
                {
                    Part(result, *type);
                }
            }

            result += ")";
            return true;
        }

        struct UnitData
        {
            std::shared_ptr<const SemanticModel>            model;
            std::shared_ptr<const TypeModel>                types;
            std::vector<NodeId>                             nodes;
            std::vector<SymbolId>                           declarations;
            std::vector<EntityId>                           prototype_parameters;
            std::vector<std::pair<DocumentId, std::size_t>> includes;
        };

        bool Definition(const SemanticModel& model, SymbolId symbol)
        {
            const auto& symbols = model.Symbols();
            if (symbols.kind[symbol] == SymbolKind::Function)
            {
                return symbols.flags[symbol] & SymbolFlag::Definition;
            }

            if (symbols.kind[symbol] == SymbolKind::Namespace ||
                symbols.kind[symbol] == SymbolKind::Class)
            {
                return true;
            }

            if (symbols.kind[symbol] != SymbolKind::Variable)
            {
                return false;
            }

            const auto& tree     = model.Tree();
            const auto& soa      = tree.NodesSoA();
            const auto  node     = symbols.decl_node[symbol];
            bool        external = false, initialized = false;
            for (auto token = soa.FirstToken(node);
                 token < soa.FirstToken(node) + soa.TokenCount(node); ++token)
            {
                external |= tree.Tokens()[token].tok == Tok::KwExtern;
                initialized |= tree.Tokens()[token].tok == Tok::Eq;
            }

            return !external || initialized;
        }
    } // namespace

    std::expected<ProjectSymbolIndex, SymbolIndexError> ProjectSymbolIndex::Build(
        const AnalysisSnapshot& snapshot, SymbolIndexLimits limits, std::stop_token stop)
    {
        if (stop.stop_requested())
        {
            return std::unexpected(SymbolIndexError::Cancelled);
        }

        limits.symbols     = std::min(limits.symbols, static_cast<std::size_t>(InvalidEntity));
        limits.occurrences = std::min(limits.occurrences, static_cast<std::size_t>(kNone));
        auto documents     = snapshot.Documents();
        if (documents.size() > limits.documents)
        {
            return std::unexpected(SymbolIndexError::LimitReached);
        }

        std::ranges::sort(documents, [&](auto a, auto b) {
            return PathKey(snapshot.Path(a)) < PathKey(snapshot.Path(b));
        });
        ProjectSymbolIndex result;
        result.m_revision = snapshot.Revision();
        std::vector<UnitData>                                      data;
        std::unordered_map<DocumentId, std::size_t>                unit_for;
        std::unordered_map<std::string, EntityId>                  identities;
        std::vector<std::vector<std::pair<std::size_t, SymbolId>>> declarations;
        const auto                                                 issue =
            [&](std::size_t unit, SymbolIndexIssue kind, std::filesystem::path path = {}) {
                auto&      entry = result.m_units[unit];
                const bool first = std::ranges::find(entry.issues, kind) == entry.issues.end();
                if (first)
                {
                    entry.issues.push_back(kind);
                }

                if (!first && path.empty())
                {
                    return;
                }

                result.m_issues.push_back(
                    { kind, entry.document, path.empty() ? entry.path : std::move(path) });
            };
        std::size_t symbol_count = 0;
        for (auto document : documents)
        {
            if (stop.stop_requested())
            {
                return std::unexpected(SymbolIndexError::Cancelled);
            }

            const auto unit = result.m_units.size();
            unit_for.emplace(document, unit);
            IndexedUnit entry;
            entry.document      = document;
            entry.path          = snapshot.Path(document);
            entry.version       = snapshot.Version(document);
            entry.source        = snapshot.Source(document);
            entry.configuration = Configuration(snapshot, document);
            entry.dependencies.assign(snapshot.Dependencies(document).begin(),
                                      snapshot.Dependencies(document).end());
            if (const auto command = snapshot.Command(document))
            {
                entry.command = command->arguments;
            }

            result.m_units.push_back(std::move(entry));
            UnitData current;
            current.model       = snapshot.Semantic(document);
            current.types       = snapshot.Types(document);
            const auto& model   = *current.model;
            const auto& tree    = model.Tree();
            const auto& soa     = tree.NodesSoA();
            const auto& symbols = model.Symbols();
            if (symbols.Size() > limits.symbols - symbol_count)
            {
                return std::unexpected(SymbolIndexError::LimitReached);
            }

            symbol_count += symbols.Size();
            current.nodes.assign(tree.Tokens().size(), 0);
            current.declarations.assign(tree.Tokens().size(), kNone);
            current.prototype_parameters.assign(tree.Tokens().size(), InvalidEntity);
            if (!tree.Diagnostics().empty())
            {
                issue(unit, SymbolIndexIssue::ParseErrors);
            }

            if (tree.Cancelled() || !result.m_units[unit].source)
            {
                issue(unit, SymbolIndexIssue::UnsupportedSyntax);
            }

            if (!snapshot.Options(document).Macros().empty())
            {
                issue(unit, SymbolIndexIssue::Preprocessing);
            }

            if (snapshot.Options(document).type_names)
            {
                issue(unit, SymbolIndexIssue::UnsupportedSyntax);
            }

            for (NodeId node = 0; node < soa.size(); ++node)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(SymbolIndexError::Cancelled);
                }

                const auto kind = soa.Kind(node);
                if (kind == GrammarKind::TemplateDeclaration ||
                    kind == GrammarKind::LambdaExpression ||
                    kind == GrammarKind::RequiresExpression ||
                    kind == GrammarKind::RequiresClause || kind == GrammarKind::RecordDefinition ||
                    kind == GrammarKind::UsingDeclaration ||
                    kind == GrammarKind::ModuleDeclaration ||
                    kind == GrammarKind::ImportDeclaration ||
                    kind == GrammarKind::LanguageLinkageSpec)
                {
                    issue(unit, SymbolIndexIssue::UnsupportedSyntax);
                }

                for (auto token = soa.FirstToken(node);
                     token < soa.FirstToken(node) + soa.TokenCount(node); ++token)
                {
                    if (token < current.nodes.size())
                    {
                        current.nodes[token] = node;
                    }
                }
            }

            for (const auto& directive : tree.Directives())
            {
                if (directive.kind != DirectiveKind::Include)
                {
                    issue(unit, SymbolIndexIssue::Preprocessing);
                    continue;
                }

                for (NodeId node = 0; node < soa.size(); ++node)
                {
                    if (soa.Kind(node) != GrammarKind::NamespaceDefinition &&
                        soa.Kind(node) != GrammarKind::FunctionDefinition &&
                        soa.Kind(node) != GrammarKind::RecordDefinition)
                    {
                        continue;
                    }

                    const auto first = soa.FirstToken(node);
                    const auto count = soa.TokenCount(node);
                    if (count && directive.offset >= tree.Tokens()[first].offset &&
                        directive.offset < tree.Tokens()[first + count - 1].offset +
                                               tree.Tokens()[first + count - 1].length)
                    {
                        issue(unit, SymbolIndexIssue::UnsupportedSyntax);
                    }
                }

                const auto         body = tree.Source().substr(directive.offset, directive.length);
                std::vector<Token> tokens;
                for (const auto& token : Lexer(body).Lex())
                {
                    if (!Trivia(token))
                    {
                        tokens.push_back(token);
                    }
                }

                std::string header;
                bool        quoted = false;
                if (tokens.size() >= 3)
                {
                    const auto text = body.substr(tokens[2].offset, tokens[2].length);
                    if (tokens[2].kind == TokenKind::StringLiteral && text.size() >= 2)
                    {
                        quoted = true;
                        header = text.substr(1, text.size() - 2);
                    }
                    else if (text == "<")
                    {
                        for (std::size_t i = 3; i < tokens.size(); ++i)
                        {
                            const auto part = body.substr(tokens[i].offset, tokens[i].length);
                            if (part == ">")
                            {
                                break;
                            }

                            header += part;
                        }
                    }
                }

                std::vector<std::filesystem::path> directories;
                if (quoted)
                {
                    directories.push_back(snapshot.Path(document).parent_path());
                }

                if (const auto command = snapshot.Command(document))
                {
                    if (quoted)
                    {
                        directories.insert(directories.end(), command->quote_directories.begin(),
                                           command->quote_directories.end());
                    }

                    directories.insert(directories.end(), command->include_directories.begin(),
                                       command->include_directories.end());
                }

                DocumentId dependency = InvalidDocument;
                for (const auto& directory : directories)
                {
                    const auto candidate = directory / header;
                    dependency           = snapshot.Find(candidate);
                    std::error_code error;
                    if (dependency != InvalidDocument ||
                        std::filesystem::is_regular_file(candidate, error))
                    {
                        break;
                    }
                }

                if (header.empty() || dependency == InvalidDocument)
                {
                    issue(unit, SymbolIndexIssue::MissingInclude, header);
                }
                else
                {
                    current.includes.emplace_back(dependency, directive.offset);
                }
            }

            for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(SymbolIndexError::Cancelled);
                }

                const auto token = symbols.decl_token[symbol];
                if (token >= tree.Tokens().size())
                {
                    issue(unit, SymbolIndexIssue::UnsupportedSyntax);
                    continue;
                }

                const auto    scope = QualifiedScope(model, symbols.scope[symbol]);
                IndexedEntity entity;
                entity.qualified_name =
                    scope.text + std::string(model.Names().Text(symbols.name[symbol]));
                entity.kind          = symbols.kind[symbol];
                entity.configuration = result.m_units[unit].configuration;
                entity.type          = current.types->Spell(current.types->SymbolType(symbol));
                entity.linkage =
                    scope.local ? EntityLinkage::Local
                    : scope.internal ||
                            (!scope.member && (symbols.flags[symbol] & SymbolFlag::Static))
                        ? EntityLinkage::Internal
                        : EntityLinkage::External;
                const auto type = current.types->SymbolType(symbol);
                entity.supported =
                    !scope.member &&
                    !(symbols.flags[symbol] &
                      (SymbolFlag::Template | SymbolFlag::Qualified | SymbolFlag::Constructor |
                       SymbolFlag::Destructor | SymbolFlag::Operator));
                if (entity.kind == SymbolKind::Function)
                {
                    entity.supported &= Signature(model, symbol, entity.signature) &&
                                        current.types->Types().Kind(type) == TypeKind::Builtin;
                    const auto node = symbols.decl_node[symbol];
                    for (auto i = soa.FirstToken(node);
                         i < soa.FirstToken(node) + soa.TokenCount(node); ++i)
                    {
                        const auto tok = tree.Tokens()[i].tok;
                        if (tok == Tok::LBrace)
                        {
                            break;
                        }

                        if (tok == Tok::KwAuto || tok == Tok::KwDecltype || tok == Tok::KwNoexcept)
                        {
                            entity.supported = false;
                        }
                    }
                }
                else if (entity.kind == SymbolKind::Variable ||
                         entity.kind == SymbolKind::Parameter)
                {
                    entity.supported &=
                        current.types->Types().IsKnown(type) &&
                        current.types->Types().Kind(current.types->Types().Strip(type)) ==
                            TypeKind::Builtin;
                    bool       external = false;
                    const auto node     = symbols.decl_node[symbol];
                    for (auto i = soa.FirstToken(node); i < token; ++i)
                    {
                        external |= tree.Tokens()[i].tok == Tok::KwExtern;
                    }

                    if (!scope.local && current.types->Types().Kind(type) == TypeKind::Const &&
                        !external)
                    {
                        entity.linkage = EntityLinkage::Internal;
                        // A prior extern declaration in an included header can
                        // change const linkage; this binder does not prove that.
                        entity.supported = false;
                    }
                }
                else
                {
                    entity.supported &= entity.kind == SymbolKind::Namespace;
                }

                if (entity.linkage == EntityLinkage::Internal)
                {
                    auto extension = snapshot.Path(document).extension().string();
                    for (auto& c : extension)
                    {
                        if (c >= 'A' && c <= 'Z')
                        {
                            c += 'a' - 'A';
                        }
                    }

                    // Header-internal entities have a distinct identity in each
                    // including TU, which a file-local binder cannot certify.
                    if (extension == ".h" || extension == ".hpp" || extension == ".hh" ||
                        extension == ".hxx")
                    {
                        entity.supported = false;
                    }
                }

                Part(entity.identity, std::to_string(static_cast<unsigned>(entity.kind)));
                Part(entity.identity, entity.qualified_name);
                Part(entity.identity, entity.signature);
                Part(entity.identity, entity.configuration);
                if (entity.linkage != EntityLinkage::External || !entity.supported)
                {
                    Part(entity.identity, PathKey(snapshot.Path(document)));
                    if (entity.linkage == EntityLinkage::Local || !entity.supported)
                    {
                        Part(entity.identity, std::to_string(tree.Tokens()[token].offset));
                    }
                }

                auto [found, inserted] =
                    identities.try_emplace(entity.identity,
                                           static_cast<EntityId>(result.m_entities.size()));
                const auto id = found->second;
                if (inserted)
                {
                    result.m_names[entity.qualified_name].push_back(id);
                    result.m_entities.push_back(std::move(entity));
                    declarations.emplace_back();
                }
                else if (result.m_entities[id].type != entity.type)
                {
                    result.m_entities[id].supported = false;
                    issue(unit, SymbolIndexIssue::ConflictingDeclarations);
                }

                result.m_local.emplace(LocalKey(document, symbol), id);
                declarations[id].emplace_back(unit, symbol);
                if (current.declarations[token] != kNone)
                {
                    issue(unit, SymbolIndexIssue::UnsupportedSyntax);
                }

                current.declarations[token] = symbol;
            }

            // The binder deliberately omits names in prototype parameter lists.
            // They are declarations, not unresolved references, and do not denote
            // the parameter of an eventual definition in another declaration.
            for (NodeId node = 0; node < soa.size(); ++node)
            {
                if (soa.Kind(node) != GrammarKind::ParameterDeclaration)
                {
                    continue;
                }

                auto function = soa.Parent(node);
                while (function != 0 && function < soa.size() &&
                       soa.Kind(function) != GrammarKind::FunctionDeclaration &&
                       soa.Kind(function) != GrammarKind::FunctionDefinition)
                {
                    function = soa.Parent(function);
                }

                if (function >= soa.size() ||
                    soa.Kind(function) != GrammarKind::FunctionDeclaration)
                {
                    continue;
                }

                EntityId owner = InvalidEntity;
                for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
                {
                    if (symbols.kind[symbol] == SymbolKind::Function &&
                        symbols.decl_node[symbol] == function)
                    {
                        owner = result.EntityFor(document, symbol);
                    }
                }

                for (auto child = node; child < soa.SubtreeEnd(node); ++child)
                {
                    if (soa.Kind(child) != GrammarKind::DeclaredName)
                    {
                        continue;
                    }

                    const auto token = soa.FirstToken(child);
                    if (token >= tree.Tokens().size())
                    {
                        continue;
                    }

                    if (current.declarations[token] != kNone ||
                        current.prototype_parameters[token] != InvalidEntity)
                    {
                        continue;
                    }

                    if (symbol_count >= limits.symbols)
                    {
                        return std::unexpected(SymbolIndexError::LimitReached);
                    }

                    ++symbol_count;
                    IndexedEntity parameter;
                    parameter.kind          = SymbolKind::Parameter;
                    parameter.linkage       = EntityLinkage::Local;
                    parameter.configuration = result.m_units[unit].configuration;
                    parameter.qualified_name =
                        owner == InvalidEntity
                            ? std::string(tree.Text(tree.Tokens()[token]))
                            : result.m_entities[owner].qualified_name +
                                  "::" + std::string(tree.Text(tree.Tokens()[token]));
                    parameter.supported =
                        owner != InvalidEntity && result.m_entities[owner].supported;
                    const auto type = ScalarParameter(model, node);
                    parameter.type  = type ? *type : "?";
                    parameter.supported &= type.has_value();
                    Part(parameter.identity, "prototype-parameter");
                    Part(parameter.identity, parameter.qualified_name);
                    Part(parameter.identity, parameter.configuration);
                    Part(parameter.identity, PathKey(snapshot.Path(document)));
                    Part(parameter.identity, std::to_string(tree.Tokens()[token].offset));
                    const auto id = static_cast<EntityId>(result.m_entities.size());
                    result.m_names[parameter.qualified_name].push_back(id);
                    result.m_entities.push_back(std::move(parameter));
                    declarations.emplace_back();
                    current.prototype_parameters[token] = id;
                }
            }

            data.push_back(std::move(current));
        }

        for (std::size_t unit = 0; unit < data.size(); ++unit)
            for (const auto& [dependency, at] : data[unit].includes)
            {
                const auto target = unit_for.find(dependency);
                if (target != unit_for.end() && result.m_units[unit].configuration !=
                                                    result.m_units[target->second].configuration)
                {
                    // One header model cannot stand in for two distinct TU
                    // contexts, even when a name happens to be unique.
                    issue(unit, SymbolIndexIssue::ConfigurationVariants,
                          result.m_units[target->second].path);
                    issue(target->second, SymbolIndexIssue::ConfigurationVariants);
                }
            }

        // Compilation database entries outside the pinned set are explicit gaps,
        // not evidence that their references do not exist.
        std::unordered_set<std::string> commands;
        for (auto document : documents)
            if (const auto command = snapshot.Command(document))
            {
                std::string key = PathKey(command->file);
                for (const auto& argument : command->arguments)
                {
                    Part(key, argument);
                }

                commands.insert(std::move(key));
            }

        for (const auto& command : snapshot.CompilationCommands())
        {
            if (stop.stop_requested())
            {
                return std::unexpected(SymbolIndexError::Cancelled);
            }

            std::string key = PathKey(command.file);
            for (const auto& argument : command.arguments)
            {
                Part(key, argument);
            }

            const auto document = snapshot.Find(command.file);
            if (document == InvalidDocument)
            {
                result.m_issues.push_back(
                    { SymbolIndexIssue::MissingTranslationUnit, InvalidDocument, command.file });
            }
            else if (!commands.contains(key))
            {
                issue(unit_for.at(document), SymbolIndexIssue::ConfigurationVariants);
            }
        }

        for (EntityId id = 0; id < result.m_entities.size(); ++id)
        {
            const auto& entity = result.m_entities[id];
            if (entity.linkage == EntityLinkage::Local ||
                (entity.kind != SymbolKind::Variable && entity.kind != SymbolKind::Function))
            {
                continue;
            }

            bool defined = false;
            for (const auto& [unit, symbol] : declarations[id])
                if (Definition(*data[unit].model, symbol))
                {
                    if (defined)
                    {
                        result.m_entities[id].supported = false;
                        issue(unit, SymbolIndexIssue::ConflictingDeclarations);
                    }

                    defined = true;
                }
        }

        // Compare exact keys in insertion order: no quadratic all-pairs scan
        // for common names, and deterministic coverage reports across builds.
        std::unordered_map<std::string, EntityId> variants;
        for (EntityId id = 0; id < result.m_entities.size(); ++id)
        {
            auto& entity = result.m_entities[id];
            if (entity.linkage != EntityLinkage::External)
            {
                continue;
            }

            std::string key;
            Part(key, entity.qualified_name);
            Part(key, std::to_string(static_cast<unsigned>(entity.kind)));
            Part(key, entity.signature);
            const auto [found, inserted] = variants.try_emplace(std::move(key), id);
            if (!inserted && result.m_entities[found->second].configuration != entity.configuration)
            {
                entity.supported = result.m_entities[found->second].supported = false;
                result.m_issues.push_back(
                    { SymbolIndexIssue::ConfigurationVariants, InvalidDocument, {} });
            }
        }

        for (std::size_t unit = 0; unit < data.size(); ++unit)
        {
            auto&       coverage         = result.m_units[unit];
            const auto& current          = data[unit];
            const auto& model            = *current.model;
            const auto& tree             = model.Tree();
            const auto& tokens           = tree.Tokens();
            const auto& soa              = tree.NodesSoA();
            const auto& sig              = model.Significant();
            const auto  occurrence_begin = result.m_occurrences.size();
            for (std::size_t position = 0; position < sig.size(); ++position)
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(SymbolIndexError::Cancelled);
                }

                const auto token = sig[position];
                if (tokens[token].kind != TokenKind::Identifier || tokens[token].tok != Tok::None)
                {
                    continue;
                }

                if (result.m_occurrences.size() >= limits.occurrences)
                {
                    return std::unexpected(SymbolIndexError::LimitReached);
                }

                IndexedOccurrence occurrence;
                occurrence.document    = coverage.document;
                occurrence.token       = token;
                occurrence.offset      = tokens[token].offset;
                occurrence.length      = tokens[token].length;
                occurrence.name        = tree.Text(tokens[token]);
                const auto declaration = current.declarations[token];
                bool       dependent = false, member = false;
                NodeId     call = InvalidNode;
                for (auto node = current.nodes[token]; node < soa.size(); node = soa.Parent(node))
                {
                    dependent |= soa.Kind(node) == GrammarKind::TemplateDeclaration ||
                                 soa.Kind(node) == GrammarKind::TemplateIdExpression ||
                                 soa.Kind(node) == GrammarKind::RequiresExpression;
                    member |= soa.Kind(node) == GrammarKind::MemberExpression ||
                              soa.Kind(node) == GrammarKind::LambdaExpression;
                    if (soa.Kind(node) == GrammarKind::CallExpression)
                    {
                        const auto children = model.ChildrenOf(node);
                        if (!children.empty() && token >= soa.FirstToken(children.front()) &&
                            token <
                                soa.FirstToken(children.front()) + soa.TokenCount(children.front()))
                        {
                            call = node;
                        }
                    }

                    if (node == 0 || soa.Parent(node) == node)
                    {
                        break;
                    }
                }

                if (declaration != kNone || current.prototype_parameters[token] != InvalidEntity)
                {
                    occurrence.entity =
                        declaration == kNone ? current.prototype_parameters[token]
                                             : result.EntityFor(coverage.document, declaration);
                    const auto& entity    = result.m_entities[occurrence.entity];
                    occurrence.resolution = dependent          ? OccurrenceResolution::Dependent
                                            : entity.supported ? OccurrenceResolution::Resolved
                                                               : OccurrenceResolution::Unsupported;
                    occurrence.role       = declaration != kNone && Definition(model, declaration)
                                                ? OccurrenceRole::Definition
                                                : OccurrenceRole::Declaration;
                }
                else
                {
                    occurrence.role =
                        call == InvalidNode ? OccurrenceRole::Reference : OccurrenceRole::Call;
                    std::unordered_set<DocumentId> visible { coverage.document };
                    std::vector<DocumentId>        queue;
                    for (const auto& [dependency, at] : current.includes)
                    {
                        if (at < occurrence.offset && visible.insert(dependency).second)
                        {
                            queue.push_back(dependency);
                        }
                    }

                    for (std::size_t next = 0; next < queue.size(); ++next)
                    {
                        const auto found = unit_for.find(queue[next]);
                        if (found == unit_for.end())
                        {
                            continue;
                        }

                        for (const auto& [dependency, at] : data[found->second].includes)
                        {
                            if (visible.insert(dependency).second)
                            {
                                queue.push_back(dependency);
                            }
                        }
                    }

                    const auto bound = model.ResolveToken(token);
                    const auto bound_entity =
                        bound == kNone ? InvalidEntity : result.EntityFor(coverage.document, bound);
                    const auto available = [&](EntityId id) {
                        for (const auto& [declared_unit, symbol] : declarations[id])
                        {
                            const auto doc = result.m_units[declared_unit].document;
                            if (!visible.contains(doc))
                            {
                                continue;
                            }

                            const auto& declared = *data[declared_unit].model;
                            if (doc != coverage.document ||
                                declared.Symbols().decl_token[symbol] < token)
                            {
                                return true;
                            }
                        }

                        return false;
                    };
                    if (bound_entity != InvalidEntity &&
                        result.m_entities[bound_entity].linkage == EntityLinkage::Local &&
                        (position == 0 || tokens[sig[position - 1]].tok != Tok::ColonColon))
                    {
                        occurrence.candidates.push_back(bound_entity);
                    }
                    else
                    {
                        auto scope =
                            QualifiedScope(model, model.ScopeOfNode(current.nodes[token]), true)
                                .text;
                        std::string qualified = occurrence.name;
                        std::size_t first     = position;
                        while (first >= 2 && tokens[sig[first - 1]].tok == Tok::ColonColon &&
                               tokens[sig[first - 2]].kind == TokenKind::Identifier)
                        {
                            qualified =
                                std::string(tree.Text(tokens[sig[first - 2]])) + "::" + qualified;
                            first -= 2;
                        }

                        if (first > 0 && tokens[sig[first - 1]].tok == Tok::ColonColon)
                        {
                            scope.clear();
                        }

                        for (;;)
                        {
                            // Qualified lookup selects the nearest owner first;
                            // a missing member must not fall back to an outer
                            // namespace with the same qualifier spelling.
                            bool owner_found = false;
                            if (first != position)
                                for (auto owner : result.Named(
                                         scope + std::string(tree.Text(tokens[sig[first]]))))
                                    if (result.m_entities[owner].linkage != EntityLinkage::Local &&
                                        available(owner))
                                    {
                                        owner_found = true;
                                        member |=
                                            result.m_entities[owner].kind != SymbolKind::Namespace;
                                    }

                            for (auto id : result.Named(scope + qualified))
                            {
                                const auto& entity = result.m_entities[id];
                                if (entity.linkage == EntityLinkage::Local)
                                {
                                    continue;
                                }

                                if (available(id))
                                {
                                    occurrence.candidates.push_back(id);
                                }
                            }

                            if (owner_found || !occurrence.candidates.empty() || scope.empty())
                            {
                                break;
                            }

                            scope.resize(scope.size() - 2);
                            const auto parent = scope.rfind("::");
                            scope = parent == std::string::npos ? "" : scope.substr(0, parent + 2);
                        }
                    }

                    bool unsupported_call = false;
                    if (call != InvalidNode)
                    {
                        const auto children = model.ChildrenOf(call);
                        for (std::size_t argument = 1; argument < children.size(); ++argument)
                        {
                            const auto type = current.types->NodeType(children[argument]);
                            unsupported_call |=
                                current.types->Types().Kind(current.types->Types().Strip(type)) !=
                                TypeKind::Builtin;
                        }
                    }

                    occurrence.resolution =
                        dependent                          ? OccurrenceResolution::Dependent
                        : member || unsupported_call       ? OccurrenceResolution::Unsupported
                        : occurrence.candidates.empty()    ? OccurrenceResolution::Unresolved
                        : occurrence.candidates.size() > 1 ? OccurrenceResolution::Ambiguous
                        : result.m_entities[occurrence.candidates.front()].supported
                            ? OccurrenceResolution::Resolved
                            : OccurrenceResolution::Unsupported;
                    if (occurrence.resolution == OccurrenceResolution::Resolved)
                    {
                        occurrence.entity = occurrence.candidates.front();
                    }

                    if (occurrence.entity != InvalidEntity &&
                        result.m_entities[occurrence.entity].kind == SymbolKind::Namespace)
                    {
                        occurrence.role = OccurrenceRole::Reference;
                    }
                }

                switch (occurrence.resolution)
                {
                    case OccurrenceResolution::Resolved:
                        ++coverage.resolved;
                        break;
                    case OccurrenceResolution::Unresolved:
                        ++coverage.unresolved;
                        break;
                    case OccurrenceResolution::Ambiguous:
                        ++coverage.ambiguous;
                        break;
                    case OccurrenceResolution::Dependent:
                        ++coverage.dependent;
                        break;
                    case OccurrenceResolution::Unsupported:
                        ++coverage.unsupported;
                        break;
                }

                if (occurrence.entity != InvalidEntity)
                {
                    result.m_entities[occurrence.entity].occurrences.push_back(
                        static_cast<std::uint32_t>(result.m_occurrences.size()));
                }

                result.m_occurrences.push_back(std::move(occurrence));
            }

            result.m_document_ranges.emplace(
                coverage.document,
                std::pair { occurrence_begin, result.m_occurrences.size() - occurrence_begin });
        }

        if (stop.stop_requested())
        {
            return std::unexpected(SymbolIndexError::Cancelled);
        }

        return result;
    }

    std::span<const EntityId> ProjectSymbolIndex::Named(std::string_view name) const
    {
        const auto found = m_names.find(std::string(name));
        return found == m_names.end() ? std::span<const EntityId> {} : found->second;
    }

    const IndexedOccurrence* ProjectSymbolIndex::At(DocumentId document, std::size_t offset) const
    {
        const auto range = m_document_ranges.find(document);
        if (range == m_document_ranges.end() || range->second.second == 0)
        {
            return nullptr;
        }

        const auto begin = m_occurrences.begin() + range->second.first;
        const auto end   = begin + range->second.second;
        auto found = std::upper_bound(begin, end, offset, [](auto at, const auto& occurrence) {
            return at < occurrence.offset;
        });
        if (found == begin)
        {
            return nullptr;
        }

        --found;
        return offset - found->offset < found->length ? &*found : nullptr;
    }

    EntityId ProjectSymbolIndex::EntityFor(DocumentId document, SymbolId symbol) const
    {
        const auto found = m_local.find(LocalKey(document, symbol));
        return found == m_local.end() ? InvalidEntity : found->second;
    }

    bool ProjectSymbolIndex::Complete() const noexcept
    {
        return m_issues.empty() && std::ranges::all_of(m_units, [](const auto& unit) {
                   return unit.unresolved == 0 && unit.ambiguous == 0 && unit.dependent == 0 &&
                          unit.unsupported == 0;
               });
    }

    bool ProjectSymbolIndex::HasCompleteCoverageFor(EntityId entity) const noexcept
    {
        return entity < m_entities.size() && m_entities[entity].supported && Complete();
    }

    std::size_t ProjectSymbolIndex::StorageBytes() const noexcept
    {
        std::size_t bytes = m_entities.capacity() * sizeof(IndexedEntity) +
                            m_occurrences.capacity() * sizeof(IndexedOccurrence);
        for (const auto& entity : m_entities)
        {
            bytes += entity.identity.capacity() + entity.qualified_name.capacity() +
                     entity.signature.capacity() + entity.type.capacity() +
                     entity.configuration.capacity() +
                     entity.occurrences.capacity() * sizeof(std::uint32_t);
        }

        for (const auto& occurrence : m_occurrences)
        {
            bytes +=
                occurrence.name.capacity() + occurrence.candidates.capacity() * sizeof(EntityId);
        }

        bytes += m_units.capacity() * sizeof(IndexedUnit) +
                 m_issues.capacity() * sizeof(SymbolCoverageIssue);
        for (const auto& unit : m_units)
        {
            bytes +=
                unit.configuration.capacity() + unit.dependencies.capacity() * sizeof(DocumentId) +
                unit.issues.capacity() * sizeof(SymbolIndexIssue);
            for (const auto& argument : unit.command)
            {
                bytes += sizeof(std::string) + argument.capacity();
            }
        }

        bytes += m_names.bucket_count() * sizeof(void*) + m_local.bucket_count() * sizeof(void*) +
                 m_document_ranges.bucket_count() * sizeof(void*);
        for (const auto& [name, ids] : m_names)
        {
            bytes += sizeof(std::string) + name.capacity() + sizeof(std::vector<EntityId>) +
                     ids.capacity() * sizeof(EntityId);
        }

        bytes +=
            m_local.size() * (sizeof(std::uint64_t) + sizeof(EntityId) + sizeof(void*)) +
            m_document_ranges.size() *
                (sizeof(DocumentId) + sizeof(std::pair<std::size_t, std::size_t>) + sizeof(void*));
        return bytes;
    }
} // namespace heimdall
