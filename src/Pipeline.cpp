#include "Pipeline.hpp"

#include <Heimdall/AnalysisFeatures.hpp>
#include <Heimdall/Formatter.hpp>
#include <Heimdall/IncludeAnalyzer.hpp>
#include <Heimdall/LineTable.hpp>
#include <Heimdall/MappedBuffer.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>

namespace heimdall::cli
{

    std::string_view GrammarKindName(heimdall::GrammarKind kind)
    {
        using heimdall::GrammarKind;
        switch (kind)
        {
            case GrammarKind::TranslationUnit:
                return "TranslationUnit";
            case GrammarKind::PreprocessorDirective:
                return "PreprocessorDirective";
            case GrammarKind::Declaration:
                return "Declaration";
            case GrammarKind::ParameterDeclaration:
                return "ParameterDeclaration";
            case GrammarKind::InitDeclarator:
                return "InitDeclarator";
            case GrammarKind::FunctionDefinition:
                return "FunctionDefinition";
            case GrammarKind::FunctionDeclaration:
                return "FunctionDeclaration";
            case GrammarKind::NamespaceDefinition:
                return "NamespaceDefinition";
            case GrammarKind::RecordDefinition:
                return "RecordDefinition";
            case GrammarKind::Enumerator:
                return "Enumerator";
            case GrammarKind::CompoundStatement:
                return "CompoundStatement";
            case GrammarKind::DeclarationStatement:
                return "DeclarationStatement";
            case GrammarKind::ExpressionStatement:
                return "ExpressionStatement";
            case GrammarKind::ReturnStatement:
                return "ReturnStatement";
            case GrammarKind::IfStatement:
                return "IfStatement";
            case GrammarKind::LoopStatement:
                return "LoopStatement";
            case GrammarKind::SwitchStatement:
                return "SwitchStatement";
            case GrammarKind::JumpStatement:
                return "JumpStatement";
            case GrammarKind::EmptyStatement:
                return "EmptyStatement";
            case GrammarKind::IdentifierExpression:
                return "IdentifierExpression";
            case GrammarKind::LiteralExpression:
                return "LiteralExpression";
            case GrammarKind::ParenthesizedExpression:
                return "ParenthesizedExpression";
            case GrammarKind::UnaryExpression:
                return "UnaryExpression";
            case GrammarKind::BinaryExpression:
                return "BinaryExpression";
            case GrammarKind::ConditionalExpression:
                return "ConditionalExpression";
            case GrammarKind::CallExpression:
                return "CallExpression";
            case GrammarKind::SubscriptExpression:
                return "SubscriptExpression";
            case GrammarKind::MemberExpression:
                return "MemberExpression";
            case GrammarKind::LambdaExpression:
                return "LambdaExpression";
            case GrammarKind::CaseLabel:
                return "CaseLabel";
            case GrammarKind::TryStatement:
                return "TryStatement";
            case GrammarKind::DoStatement:
                return "DoStatement";
            case GrammarKind::TemplateDeclaration:
                return "TemplateDeclaration";
            case GrammarKind::TemplateArgument:
                return "TemplateArgument";
            case GrammarKind::TemplateIdExpression:
                return "TemplateIdExpression";
            case GrammarKind::TypeSpecifier:
                return "TypeSpecifier";
            case GrammarKind::Declarator:
                return "Declarator";
            case GrammarKind::DeclaredName:
                return "DeclaredName";
            case GrammarKind::PointerOperator:
                return "PointerOperator";
            case GrammarKind::NestedNameSpecifier:
                return "NestedNameSpecifier";
            case GrammarKind::ArraySuffix:
                return "ArraySuffix";
            case GrammarKind::FunctionSuffix:
                return "FunctionSuffix";
            case GrammarKind::TrailingReturnType:
                return "TrailingReturnType";
            case GrammarKind::NoexceptSpecifier:
                return "NoexceptSpecifier";
            case GrammarKind::AttributeSpecifier:
                return "AttributeSpecifier";
            case GrammarKind::BitfieldSuffix:
                return "BitfieldSuffix";
            case GrammarKind::ModuleDeclaration:
                return "ModuleDeclaration";
            case GrammarKind::ImportDeclaration:
                return "ImportDeclaration";
            case GrammarKind::UsingDeclaration:
                return "UsingDeclaration";
            case GrammarKind::ConceptDefinition:
                return "ConceptDefinition";
            case GrammarKind::RequiresClause:
                return "RequiresClause";
            case GrammarKind::RequiresExpression:
                return "RequiresExpression";
            case GrammarKind::Requirement:
                return "Requirement";
            case GrammarKind::ErrorExpression:
                return "ErrorExpression";
            case GrammarKind::Error:
                return "Error";
            case GrammarKind::LanguageLinkageSpec:
                return "LanguageLinkageSpec";
            case GrammarKind::CastExpression:
                return "CastExpression";
        }

        return "Unknown";
    }

    std::string_view StandardName(heimdall::CppStandard standard)
    {
        switch (standard)
        {
            case heimdall::CppStandard::Cpp20:
                return "c++20";
            case heimdall::CppStandard::Cpp23:
                return "c++23";
            case heimdall::CppStandard::Cpp26:
                return "c++26";
        }

        return "c++20";
    }

    heimdall::ParserOptions ParserOptionsForFile(const std::filesystem::path&     path,
                                                 const Options&                   options,
                                                 const heimdall::CompileDatabase* database)
    {
        heimdall::ParserOptions parser_options;
        if (options.std_override)
        {
            parser_options.standard = options.standard;
        }

        if (database != nullptr)
        {
            if (const auto* command = database->Find(path); command != nullptr)
            {
                if (!options.std_override)
                {
                    parser_options.standard = command->standard;
                }

                // Shared ownership (one copy per file): the tree borrows it without
                // further copies, and -U undefines are honored here as well.
                auto macros = std::make_shared<heimdall::Preprocessor::MacroMap>(command->defines);
                for (const auto& name : command->undefines)
                {
                    macros->erase(name);
                }

                parser_options.shared_macros = std::move(macros);
            }
        }

        return parser_options;
    }

    std::vector<SyntaxDiagnostic> ToSyntaxDiagnostics(
        std::string_view source, const std::vector<heimdall::GrammarDiagnostic>& grammar)
    {
        heimdall::LineTable lines;
        lines.Build(source);
        std::vector<SyntaxDiagnostic> out;
        out.reserve(grammar.size());
        for (const auto& diagnostic : grammar)
        {
            const auto position = lines.Lookup(diagnostic.offset);
            out.push_back(
                { position.line, position.column, "syntax/parse-error", diagnostic.message });
        }

        return out;
    }

    void ProcessFile(const std::filesystem::path&     path,
                     const Options&                   options,
                     const heimdall::CompileDatabase* database,
                     FileResult&                      result)
    {
        result.path = path;
        auto buffer = heimdall::MappedBuffer::Open(path.string());
        if (!buffer)
        {
            result.error = buffer.error();
            return;
        }

        const std::string_view source = buffer->view();
        // One parse per file shared by syntax diagnostics, the rule engine and
        // the semantic pass (was: each stage re-lexed the buffer from scratch).
        std::optional<heimdall::ParseTree> tree;
        if (options.command == Command::Parse || options.command == Command::Lint ||
            options.command == Command::Check)
        {
            const auto parser_options = ParserOptionsForFile(path, options, database);
            tree                      = heimdall::ParseTree::Parse(source, parser_options);
            result.standard_name      = std::string(StandardName(tree->Standard()));
            result.syntax_diagnostics = ToSyntaxDiagnostics(source, tree->Diagnostics());
            if (options.command == Command::Parse)
            {
                result.nodes.reserve(tree->Nodes().size());
                for (const auto& node : tree->Nodes())
                {
                    std::size_t offset = source.size();
                    std::size_t length = 0;
                    if (node.first_token < tree->Tokens().size() && node.token_count > 0)
                    {
                        const std::size_t last_index =
                            std::min<std::size_t>(node.first_token + node.token_count,
                                                  tree->Tokens().size()) -
                            1;
                        offset                 = tree->Tokens()[node.first_token].offset;
                        const auto& last_token = tree->Tokens()[last_index];
                        length                 = last_token.offset + last_token.length - offset;
                    }

                    result.nodes.push_back(
                        { std::string(GrammarKindName(node.kind)), node.parent, offset, length });
                }

                return;
            }
        }

        if (options.semantic && database != nullptr)
        {
            const auto* command = database->Find(path);
            if (command != nullptr)
            {
                result.has_semantic_context = true;
                const heimdall::SemanticAnalyzer analyzer;
                const auto                       types = analyzer.CollectTypeNames(source, command);
                result.type_count                      = types.size();
                result.semantic_diagnostics = tree ? analyzer.AnalyzeUnusedLocals(*tree, command)
                                                   : analyzer.AnalyzeUnusedLocals(source, command);
                std::unordered_set<std::string> no_values;
                std::size_t                     line_start = 0;
                while (line_start < source.size())
                {
                    std::size_t line_end = source.find('\n', line_start);
                    if (line_end == std::string_view::npos)
                    {
                        line_end = source.size();
                    }

                    const auto line = source.substr(line_start, line_end - line_start);
                    switch (analyzer.ClassifyAsteriskStatement(line, types, no_values))
                    {
                        case heimdall::AsteriskMeaning::Declaration:
                            ++result.declaration_count;
                            break;
                        case heimdall::AsteriskMeaning::Multiplication:
                            break;
                        case heimdall::AsteriskMeaning::Ambiguous:
                            ++result.ambiguous_count;
                            break;
                        case heimdall::AsteriskMeaning::NotApplicable:
                            break;
                    }

                    line_start = line_end == source.size() ? source.size() : line_end + 1;
                }
            }
        }

        if (options.command == Command::Format)
        {
            heimdall::FormatOptions format_options = options.format_options;
            if (!options.rule_config_explicit)
            {
                // Per-file chain: <file dir> -> ... -> git root, so a config
                // inside the target subdirectory is honored even when the
                // command runs from elsewhere.
                auto found = heimdall::FindFormatOptions(path);
                if (!found)
                {
                    result.error = found.error();
                    return;
                }

                if (*found)
                {
                    format_options = **found;
                }

                if (options.pointer_alignment_override)
                {
                    format_options.pointer_alignment = options.pointer_alignment;
                }

                if (options.reference_alignment_override)
                {
                    format_options.reference_alignment = options.reference_alignment;
                }
            }

            result.output  = tree ? heimdall::Formatter(format_options).Format(*tree)
                                  : heimdall::Formatter(format_options).Format(source);
            result.changed = result.output != source;
        }
        else
        {
            heimdall::RuleOptions rule_options = options.rule_options;
            if (!options.rule_config_explicit &&
                (options.command == Command::Lint || options.command == Command::Check))
            {
                auto found = heimdall::FindRuleOptions(path);
                if (!found)
                {
                    result.error = found.error();
                    return;
                }

                if (*found)
                {
                    rule_options = std::move(**found);
                }

                rule_options.overrides.insert(
                    rule_options.overrides.end(), options.rule_overrides.begin(),
                    options.rule_overrides.end());
            }

            const heimdall::RuleEngine rule_engine(rule_options);
            if (!(tree && options.semantic && database != nullptr))
            {
                result.diagnostics =
                    tree ? rule_engine.Analyze(*tree) : rule_engine.Analyze(source);
            }

            if (tree && options.semantic && database != nullptr)
            {
                // cpp/no-unused-include needs the headers on disk, so like the
                // other semantic rules it needs a compile command. Headers are
                // absent from compile databases: borrow the nearest entry, as the
                // language server does.
                std::vector<heimdall::Diagnostic>               semantic;
                std::shared_ptr<const heimdall::IncludeProfile> profile;
                const auto* command = database->FindOrNearest(path);
                if (command != nullptr)
                {
                    profile  = heimdall::IncludeAnalyzer::BuildProfile(path, *tree, command);
                    semantic = heimdall::IncludeAnalyzer::Analyze(*tree, *profile);
                }

                // Rules on the bound semantic model need no compile command; the
                // project-level ones (include-what-you-use, modernize-final) see the
                // included headers only when there is a profile.
                const heimdall::ProjectContext context { path, profile.get(), command };
                // Batch analysis borrows the mapped buffer and tree for this
                // scope only; it does not allocate a project Workspace or copy
                // the source just to use the same analysis entry point as LSP.
                const auto borrowed = std::shared_ptr<const heimdall::ParseTree>(
                    &*tree,
                    [](const heimdall::ParseTree*) {});
                const heimdall::AnalysisContext analysis(
                    heimdall::AnalysisSnapshot::FromSyntax(
                        borrowed, ParserOptionsForFile(path, options, database), path),
                    0);
                result.diagnostics =
                    heimdall::AnalysisFeatures::Diagnostics(analysis, rule_engine, true, context);
                semantic = rule_engine.ApplyPolicy(std::move(semantic), *tree);
                result.diagnostics.insert(
                    result.diagnostics.end(), std::make_move_iterator(semantic.begin()),
                    std::make_move_iterator(semantic.end()));
                std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(),
                                 [](const heimdall::Diagnostic& a, const heimdall::Diagnostic& b) {
                                     return a.offset < b.offset;
                                 });
            }

            if (options.fix && !result.diagnostics.empty())
            {
                result.output = heimdall::RuleEngine::ApplyFixes(
                    source, result.diagnostics, options.fix_unsafe);
                result.changed = result.output != source;
            }
        }
    }

    void RunParallel(const std::vector<std::filesystem::path>& files,
                     const Options&                            options,
                     const heimdall::CompileDatabase*          database,
                     std::vector<FileResult>&                  results)
    {
        results.resize(files.size());
        std::atomic_size_t        next { 0 };
        const std::size_t         count = std::min(options.jobs, files.size());
        std::vector<std::jthread> workers;
        workers.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            workers.emplace_back([&]() {
                while (true)
                {
                    const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= files.size())
                    {
                        break;
                    }

                    ProcessFile(files[index], options, database, results[index]);
                }
            });
        }

        workers.clear();
    }

} // namespace heimdall::cli
