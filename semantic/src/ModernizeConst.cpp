#include <Heimdall/SemanticRules.hpp>

#include "detail/ConstantAnalysis.hpp"
#include "detail/RuleSupport.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace heimdall
{

    namespace
    {

        using detail::ConstantAnalysis;
        using detail::Reporter;
        using detail::SortByOffset;
        namespace Spec = detail::Spec;

        // Where the written type of a declaration begins: the first of its
        // qualifier (`std::`) and type specifier, so `const` goes before both.
        std::uint32_t TypeStart(const SemanticModel& model, std::uint32_t decl_node)
        {
            const auto& nodes = model.Tree().NodesSoA();
            std::uint32_t first = kNone;
            for (const auto child : model.ChildrenOf(decl_node))
            {
                if ((nodes.Kind(child) == GrammarKind::TypeSpecifier || nodes.Kind(child) == GrammarKind::NestedNameSpecifier) &&
                    (first == kNone || nodes.FirstToken(child) < first))
                {
                    first = nodes.FirstToken(child);
                }
            }

            // The grammar folds storage keywords into the type: `const` and `constexpr`
            // read better after them (`static const int`).
            const auto& tokens = model.Tree().Tokens();
            while (first != kNone && first < tokens.size())
            {
                const Tok tok = tokens[first].tok;
                const bool storage = tok == Tok::KwStatic || tok == Tok::KwInline || tok == Tok::KwExtern ||
                    tok == Tok::KwThreadLocal || tok == Tok::KwRegister;
                const bool trivia = tokens[first].kind == TokenKind::Whitespace || tokens[first].kind == TokenKind::LineComment ||
                    tokens[first].kind == TokenKind::BlockComment;
                if (!storage && !trivia)
                {
                    break;
                }

                ++first;
            }

            return first;
        }

        // Replacement for the type-start insertion: `text` right before the type.
        bool InsertBeforeType(
            const SemanticModel& model,
            std::uint32_t decl_node,
            std::string_view text,
            TextEdit& edit)
        {
            const auto token = TypeStart(model, decl_node);
            if (token == kNone)
            {
                return false;
            }

            edit = TextEdit{model.Tree().Tokens()[token].offset, 0, std::string(text)};
            return true;
        }

        std::string NameOf(const SemanticModel& model, SymbolId symbol)
        {
            return std::string(model.Names().Text(model.Symbols().name[symbol]));
        }

        Diagnostic Report(
            Reporter& reporter,
            const SemanticModel& model,
            SymbolId symbol,
            RuleId rule,
            std::string_view code,
            std::string message,
            TextEdit fix,
            std::string title)
        {
            const auto& token = model.Tree().Tokens()[model.Symbols().decl_token[symbol]];
            return reporter.Make(rule, code, std::move(message), token.offset, token.length, std::move(fix),
                std::move(title));
        }

    } // namespace

    std::vector<Diagnostic> SemanticRules::AnalyzeConst(const FlowModel& flow)
    {
        const auto& model = flow.Model();
        const auto& types = flow.Types();
        const auto& table = types.Types();
        const auto& symbols = model.Symbols();
        const auto& nodes = model.Tree().NodesSoA();
        ConstantAnalysis constants(flow);
        Reporter reporter(model.Tree());
        std::vector<Diagnostic> diagnostics;

        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (symbols.kind[symbol] != SymbolKind::Variable || flow.OwnerOf(symbol) == kNone ||
                !flow.IsNeverModified(symbol) || symbols.decl_node[symbol] >= nodes.size() ||
                !constants.DeclaresSingleName(symbol))
            {
                continue;
            }

            const auto decl = symbols.decl_node[symbol];
            const auto parent = nodes.Parent(decl);
            if (parent < nodes.size() && (nodes.Kind(parent) == GrammarKind::LoopStatement ||
                nodes.Kind(parent) == GrammarKind::IfStatement || nodes.Kind(parent) == GrammarKind::SwitchStatement))
            {
                continue; // declared in a statement header
            }

            const auto spec = constants.SpecifiersOf(decl, symbols.decl_token[symbol]);
            if ((spec.mask & (Spec::Constexpr | Spec::Consteval | Spec::Const | Spec::Opaque)) != 0)
            {
                continue;
            }

            // Values, library and user classes, enums and arrays of those. References,
            // pointers and everything the Typer does not know stay out.
            const auto type = types.SymbolType(symbol);
            bool eligible = false;
            switch (table.Kind(type))
            {
            case TypeKind::Builtin:
                eligible =!table.IsBuiltin(type, BuiltinType::Void);
                break;
            case TypeKind::Enum:
            case TypeKind::Class:
            case TypeKind::External:
                eligible = true;
                break;
            case TypeKind::Array:
                eligible = table.IsKnown(table.Element(type)) && table.Kind(table.Element(type)) != TypeKind::Const;
                break;
            default:
                break;
            }

            // A constant initializer is cpp/modernize-constexpr's: it says more.
            if (!eligible || constants.ConstexprVariable(symbol) != ConstantAnalysis::Candidate::None)
            {
                continue;
            }

            TextEdit fix{};
            if (!InsertBeforeType(model, decl, "const ", fix))
            {
                continue;
            }

            const auto name = NameOf(model, symbol);
            diagnostics.push_back(Report(reporter, model, symbol, RuleId::ModernizeConst, "cpp/modernize-const",
                "'" + name + "' is never modified; declare it 'const'", std::move(fix),
                "Declare '" + name + "' const"));
        }

        SortByOffset(diagnostics);
        return diagnostics;
    }

    std::vector<Diagnostic> SemanticRules::AnalyzeConstexpr(const FlowModel& flow)
    {
        const auto& model = flow.Model();
        const auto& symbols = model.Symbols();
        const auto& nodes = model.Tree().NodesSoA();
        ConstantAnalysis constants(flow);
        Reporter reporter(model.Tree());
        std::vector<Diagnostic> diagnostics;

        for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
        {
            if (symbols.decl_node[symbol] >= nodes.size())
            {
                continue;
            }

            const auto decl = symbols.decl_node[symbol];
            const auto name = NameOf(model, symbol);
            if (symbols.kind[symbol] == SymbolKind::Function)
            {
                TextEdit fix{};
                if (constants.EligibleFunction(symbol) && InsertBeforeType(model, decl, "constexpr ", fix))
                {
                    diagnostics.push_back(Report(reporter, model, symbol, RuleId::ModernizeConstexpr,
                        "cpp/modernize-constexpr",
                        "function '" + name + "' can be evaluated at compile time; declare it 'constexpr'",
                        std::move(fix), "Declare '" + name + "' constexpr"));
                }

                continue;
            }

            if (symbols.kind[symbol] != SymbolKind::Variable)
            {
                continue;
            }

            switch (constants.ConstexprVariable(symbol))
            {
            case ConstantAnalysis::Candidate::ReplaceConst:
            {
                const auto spec = constants.SpecifiersOf(decl, symbols.decl_token[symbol]);
                const auto& keyword = model.Tree().Tokens()[model.Significant()[spec.const_position]];
                diagnostics.push_back(Report(reporter, model, symbol, RuleId::ModernizeConstexpr,
                    "cpp/modernize-constexpr",
                    "'" + name + "' has a constant initializer; declare it 'constexpr' instead of 'const'",
                    TextEdit{keyword.offset, keyword.length, "constexpr"}, "Declare '" + name + "' constexpr"));
                break;
            }
            case ConstantAnalysis::Candidate::InsertConstexpr:
            {
                TextEdit fix{};
                if (InsertBeforeType(model, decl, "constexpr ", fix))
                {
                    diagnostics.push_back(Report(reporter, model, symbol, RuleId::ModernizeConstexpr,
                        "cpp/modernize-constexpr",
                        "'" + name + "' is never modified and has a constant initializer; declare it 'constexpr'",
                        std::move(fix), "Declare '" + name + "' constexpr"));
                }

                break;
            }
            case ConstantAnalysis::Candidate::None:
                break;
            }
        }

        SortByOffset(diagnostics);
        return diagnostics;
    }

} // namespace heimdall
