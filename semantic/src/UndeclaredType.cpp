#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <Heimdall/LineTable.hpp>
#include <Heimdall/SemanticRules.hpp>

namespace heimdall
{
	std::vector<Diagnostic> SemanticRules::AnalyzeUndeclaredType(const SemanticModel& model)
	{
		const auto& tree    = model.Tree();
		const auto& tokens  = tree.Tokens();
		const auto& nodes   = tree.NodesSoA();
		const auto& symbols = model.Symbols();
		const auto equalIgnoringCase = [](std::string_view left, std::string_view right)
		{
			if (left.size() != right.size())
			{
				return false;
			}

			for (std::size_t i = 0; i < left.size(); ++i)
			{
				if (std::tolower(static_cast<unsigned char>(left[i])) !=
					std::tolower(static_cast<unsigned char>(right[i])))
				{
					return false;
				}
			}

			return true;
		};
		const auto nextSignificant = [&](std::size_t token)
		{
			while (token < tokens.size() &&
				(tokens[token].kind == TokenKind::Whitespace ||
				tokens[token].kind == TokenKind::LineComment ||
				tokens[token].kind == TokenKind::BlockComment))
			{
				++token;
			}

			return token;
		};

		std::vector<Diagnostic> diagnostics;
		LineTable lines;
		bool linesBuilt = false;
		for (std::size_t token = 0; token < tokens.size(); ++token)
		{
			if (tokens[token].tok != Tok::KwConst)
			{
				continue;
			}

			const auto typeToken = nextSignificant(token + 1);
			if (typeToken >= tokens.size() || tokens[typeToken].kind != TokenKind::Identifier)
			{
				continue;
			}

			const auto assignment = nextSignificant(typeToken + 1);
			if (assignment >= tokens.size() || tokens[assignment].tok != Tok::Eq)
			{
				continue;
			}

			bool inTemplateBody = false;
			for (std::uint32_t node = 0; node < nodes.size(); ++node)
			{
				if (nodes.Kind(node) == GrammarKind::TemplateDeclaration)
				{
					const auto first = nodes.FirstToken(node);
					const auto end   = first + nodes.TokenCount(node);
					inTemplateBody |= typeToken >= first && typeToken < end;
				}
			}

			if (!inTemplateBody)
			{
				continue;
			}

			const auto written = tree.Text(tokens[typeToken]);
			std::string_view suggestion;
			bool ambiguous = false;
			for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				if (symbols.kind[symbol] != SymbolKind::Class)
				{
					continue;
				}

				const auto candidate = model.Names().Text(symbols.name[symbol]);
				if (candidate == written ||!equalIgnoringCase(candidate, written))
				{
					continue;
				}

				if (!suggestion.empty() && suggestion != candidate)
				{
					ambiguous = true;
					break;
				}

				suggestion = candidate;
			}

			if (suggestion.empty() || ambiguous)
			{
				continue;
			}

			if (!linesBuilt)
			{
				lines.Build(tree.Source());
				linesBuilt = true;
			}

			const auto offset   = tokens[typeToken].offset;
			const auto position = lines.Lookup(offset);
			diagnostics.push_back(Diagnostic {
					RuleId::UndeclaredType, Severity::Error, "cpp/undeclared-type",
					"'" + std::string(written) + "' does not name a type; did you mean '" +
					std::string(suggestion) + "'? [-Wtemplate-body]",
					offset, tokens[typeToken].length, position.line, position.column, false, {}
			});
		}

		return diagnostics;
	}
} // namespace heimdall
