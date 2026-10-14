#include <Heimdall/TemplateDeduction.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <Heimdall/Completion.hpp>
#include <Heimdall/Lexer.hpp>
#include <Heimdall/ParseTree.hpp>

namespace heimdall
{
	namespace
	{
		constexpr std::size_t kMaxDepth           = 16;
		constexpr std::size_t kMaxReturns         = 32;
		constexpr std::size_t kMaxExpressionBytes = 4096;
		using Bindings                            = std::unordered_map<std::string, std::string>;

		std::string_view Trim(std::string_view text)
		{
			const auto first = text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos)
			{
				return {};
			}

			return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
		}

		std::string Bare(std::string_view text)
		{
			text = Trim(text);
			for (bool changed = true; changed;)
			{
				changed = false;
				for (const auto qualifier :
					{
						"const ", "volatile ", "constexpr ", "consteval "
				})
				{
					if (text.starts_with(qualifier))
					{
						text    = Trim(text.substr(std::string_view(qualifier).size()));
						changed = true;
					}
				}

				while (text.ends_with('&'))
				{
					text = Trim(text.substr(0, text.size() - 1));
				}
			}

			return std::string(text);
		}

		std::string Slice(const ParseTree& tree, std::size_t begin, std::size_t end)
		{
			if (begin >= end || end > tree.Tokens().size())
			{
				return {};
			}

			const auto& first = tree.Tokens()[begin];
			const auto& last  = tree.Tokens()[end - 1];
			return std::string(Trim(tree.Source().substr(first.offset,
				last.offset + last.length - first.offset)));
		}

		std::vector<std::string> Words(std::string_view text)
		{
			std::vector<std::string> words;
			for (const auto& token : Lexer(text).Lex())
			{
				if (token.kind != TokenKind::Whitespace && token.kind != TokenKind::LineComment &&
					token.kind != TokenKind::BlockComment)
				{
					words.emplace_back(text.substr(token.offset, token.length));
				}
			}

			return words;
		}

		std::string Substitute(std::string_view text, const Bindings& bindings)
		{
			std::string result;
			for (const auto& token : Lexer(text).Lex())
			{
				const auto word  = text.substr(token.offset, token.length);
				const auto found = bindings.find(std::string(word));
				result += token.kind == TokenKind::Identifier && found != bindings.end()
				? std::string_view(found->second) : word;
			}

			return result;
		}

		struct TypeParts
		{
			std::string head;
			std::vector<std::string> arguments;
		};

		TypeParts Parts(std::string_view text)
		{
			const auto bare = Bare(text);
			const auto open = bare.find('<');
			TypeParts result;
			result.head = std::string(Trim(std::string_view(bare).substr(0, open)));
			if (open == std::string::npos ||!bare.ends_with('>'))
			{
				return result;
			}

			std::size_t start = open + 1;
			std::size_t depth = 0;
			for (std::size_t index = start; index < bare.size() - 1; ++index)
			{
				if (bare[index] == '<')
				{
					++depth;
				}
				else if (bare[index] == '>' && depth != 0)
				{
					--depth;
				}
				else if (bare[index] == ',' && depth == 0)
				{
					result.arguments.emplace_back(Trim(std::string_view(bare).substr(start, index - start)));
					start = index + 1;
				}
			}

			result.arguments.emplace_back(Trim(std::string_view(bare).substr(start, bare.size() - start - 1)));
			return result;
		}

		bool Unify(
			std::string_view formal,
			std::string_view actual,
			const std::vector<std::string>& parameters,
			Bindings& bindings,
			std::size_t depth = 0)
		{
			if (depth >= kMaxDepth || actual.empty())
			{
				return false;
			}

			const auto pattern = Parts(formal);
			const auto value   = Parts(actual);
			if (std::find(parameters.begin(), parameters.end(), pattern.head) != parameters.end())
			{
				const auto[found, inserted] = bindings.emplace(pattern.head, Bare(actual));
				return inserted || Bare(found->second) == Bare(actual);
			}

			if ((pattern.head != value.head && !value.head.ends_with("::" + pattern.head)) ||
				pattern.arguments.size() != value.arguments.size())
			{
				return false;
			}

			for (std::size_t index = 0; index < pattern.arguments.size(); ++index)
			{
				if (!Unify(pattern.arguments[index], value.arguments[index], parameters, bindings, depth + 1))
				{
					return false;
				}
			}

			return true;
		}

		std::optional<long long> Integer(std::string_view text)
		{
			long long value   = 0;
			const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
			return result.ec == std::errc {} && result.ptr == text.data() + text.size()
			? std::optional(value) : std::nullopt;
		}

		std::optional<bool> Condition(std::string_view text)
		{
			const auto words = Words(text);
			if (words == std::vector<std::string> {"true"})
			{
				return true;
			}

			if (words == std::vector<std::string> {"false"})
			{
				return false;
			}

			if (words.size() != 3)
			{
				return std::nullopt;
			}

			const auto left  = Integer(words[0]);
			const auto right = Integer(words[2]);
			if (!left ||!right)
			{
				return std::nullopt;
			}

			if (words[1] == "==")
			{
				return *left == *right;
			}

			if (words[1] == "!=")
			{
				return *left != *right;
			}

			if (words[1] == "<")
			{
				return *left < * right;
			}

			if (words[1] == ">")
			{
				return *left > * right;
			}

			if (words[1] == "<=")
			{
				return *left <= * right;
			}

			if (words[1] == ">=")
			{
				return *left >= * right;
			}

			return std::nullopt;
		}

		const IndexedScope* Record(
			std::string_view written,
			std::span<const IndexedScope * const> scopes,
			const std::vector<std::string>& owner)
		{
			const auto type = Parts(written);
			for (std::size_t keep = owner.size() + 1; keep > 0; --keep)
			{
				std::string qualified;
				for (std::size_t index = 0; index < keep - 1; ++index)
				{
					qualified += owner[index] + "::";
				}

				qualified += type.head;
				for (const auto* scope : scopes)
				{
					std::string path;
					for (const auto& part : scope->path)
					{
						path += path.empty() ? part : "::" + part;
					}

					if (path == qualified && scope->kind == CompletionKind::Type)
					{
						return scope;
					}
				}
			}

			return nullptr;
		}

		std::string ExpressionType(
			std::string_view expression,
			const Bindings& variables,
			std::span<const IndexedScope * const> scopes,
			const std::vector<std::string>& owner,
			bool preserveReference)
		{
			auto words   = Words(expression);
			bool grouped = false;
			while (words.size() > 2 && words.front() == "(" && words.back() == ")")
			{
				std::size_t nesting = 0;
				bool encloses       = true;
				for (std::size_t index = 0; index < words.size(); ++index)
				{
					if (words[index] == "(")
					{
						++nesting;
					}
					else if (words[index] == ")")
					{
						if (nesting == 0)
						{
							return {};
						}

						--nesting;
						if (nesting == 0 && index + 1 != words.size())
						{
							encloses = false;
						}
					}
				}

				if (!encloses || nesting != 0)
				{
					return {};
				}

				grouped = true;
				words.erase(words.begin());
				words.pop_back();
			}

			if (words.empty())
			{
				return {};
			}

			const auto brace = expression.find('{');
			if (brace != std::string_view::npos)
			{
				const auto constructed = Bare(expression.substr(0, brace));
				return Record(constructed, scopes, owner) ? constructed : std::string {};
			}

			const auto found = variables.find(words.front());
			if (found == variables.end())
			{
				return {};
			}

			std::string type = found->second;
			for (std::size_t index = 1; index < words.size(); index += 2)
			{
				if (index + 1 >= words.size() || words[index] != ".")
				{
					return {};
				}

				const auto* record = Record(type, scopes, owner);
				if (!record)
				{
					return {};
				}

				const auto arguments = Parts(type).arguments;
				Bindings substitutions;
				if (arguments.size() != record->template_params.size())
				{
					return {};
				}

				for (std::size_t parameter = 0; parameter < arguments.size(); ++parameter)
				{
					substitutions.emplace(record->template_params[parameter], arguments[parameter]);
				}

				const auto member = std::find_if(record->members.begin(), record->members.end(),
					[&](const auto& item)
					{
						return item.kind == CompletionKind::Variable && item.label == words[index + 1];
				});
				if (member == record->members.end())
				{
					return {};
				}

				const bool constant = Trim(type).starts_with("const ");
				type = Bare(Substitute(member->layout_type.empty() ? member->type_text : member->layout_type,
					substitutions));
				if (constant && (grouped ||!preserveReference))
				{
					type = "const " + type;
				}
			}

			if (!preserveReference)
			{
				return Bare(type);
			}

			return grouped ? type +(type.ends_with('&') ? "" : "&") : type;
		}
	}

	std::shared_ptr<const FunctionDeduction> TemplateDeduction::Capture(
		const ParseTree& tree,
		std::size_t nameNode)
	{
		const auto& nodes  = tree.NodesSoA();
		const auto& tokens = tree.Tokens();
		auto function      = nameNode;
		while (function < nodes.size() && nodes.Kind(function) != GrammarKind::FunctionDefinition)
		{
			if (function == ParseTree::RootNode)
			{
				return {};
			}

			function = nodes.Parent(function);
		}

		if (function >= nodes.size())
		{
			return {};
		}

		auto result = std::make_shared<FunctionDeduction>();
		for (std::size_t node = function + 1; node < nodes.SubtreeEnd(function); ++node)
		{
			if (nodes.Kind(node) == GrammarKind::LambdaExpression ||
				nodes.Kind(node) == GrammarKind::RecordDefinition ||
				nodes.Kind(node) == GrammarKind::FunctionDefinition)
			{
				node = nodes.SubtreeEnd(node) - 1;
				continue;
			}

			if (nodes.Kind(node) == GrammarKind::ParameterDeclaration)
			{
				std::size_t name = tokens.size();
				for (auto child = node + 1; child < nodes.SubtreeEnd(node); ++child)
				{
					if (nodes.Kind(child) == GrammarKind::DeclaredName)
					{
						name = nodes.FirstToken(child);
						break;
					}
				}

				result->parameterNames.push_back(name < tokens.size() ? std::string(tree.Text(tokens[name])) : "");
				result->parameterTypes.push_back(Slice(tree, nodes.FirstToken(node), name));
				node = nodes.SubtreeEnd(node) - 1;
			}
			else if (nodes.Kind(node) == GrammarKind::ReturnStatement)
			{
				ReturnRecipe recipe;
				auto end = nodes.FirstToken(node) + nodes.TokenCount(node);
				while (end > nodes.FirstToken(node) &&
					(tokens[end - 1].kind == TokenKind::Whitespace || tree.Text(tokens[end - 1]) == ";"))
				{
					--end;
				}

				recipe.expression = Slice(tree, nodes.FirstToken(node) + 1, end);
				for (auto parent = nodes.Parent(node); parent != function && parent < nodes.size(); parent = nodes.Parent(parent))
				{
					if (nodes.Kind(parent) == GrammarKind::LambdaExpression ||
						nodes.Kind(parent) == GrammarKind::FunctionDefinition)
					{
						recipe.expression.clear();
						break;
					}

					if (nodes.Kind(parent) != GrammarKind::IfStatement)
					{
						continue;
					}

					const auto first = nodes.FirstToken(parent);
					const auto last  = first + nodes.TokenCount(parent);
					auto open        = first;
					bool constant    = false;
					while (open < last && tree.Text(tokens[open]) != "(")
					{
						constant |= tree.Text(tokens[open]) == "constexpr";
						++open;
					}

					if (!constant || open == last)
					{
						continue;
					}

					auto close        = open + 1;
					std::size_t depth = 1;
					for (; close < last; ++close)
					{
						if (tree.Text(tokens[close]) == "(")
						{
							++depth;
						}

						if (tree.Text(tokens[close]) == ")" && --depth == 0)
						{
							break;
						}
					}

					if (close == last)
					{
						return {};
					}

					std::size_t consequentEnd = close + 1;
					for (const auto child : tree.DirectChildren(parent))
					{
						if (nodes.FirstToken(child) > close)
						{
							consequentEnd = nodes.FirstToken(child) + nodes.TokenCount(child);
							break;
						}
					}

					recipe.conditions.push_back({Slice(tree, open + 1, close),
							nodes.FirstToken(node) >= consequentEnd});
				}

				if (!recipe.expression.empty())
				{
					if (result->returns.size() == kMaxReturns || recipe.expression.size() > kMaxExpressionBytes)
					{
						return {};
					}

					std::reverse(recipe.conditions.begin(), recipe.conditions.end());
					result->returns.push_back(std::move(recipe));
				}
			}
		}

		return result->returns.empty() ? nullptr : result;
	}

	std::string TemplateDeduction::Instantiate(
		const FunctionDeduction& function,
		const std::vector<std::string>& templateParameters,
		const std::vector<std::string>& explicitArguments,
		const std::vector<std::string>& argumentTypes,
		std::span<const IndexedScope * const> scopes,
		const std::vector<std::string>& owner,
		bool preserveReference)
	{
		if (explicitArguments.size() > templateParameters.size() ||
			argumentTypes.size() != function.parameterTypes.size())
		{
			return {};
		}

		Bindings bindings;
		for (std::size_t index = 0; index < explicitArguments.size(); ++index)
		{
			bindings.emplace(templateParameters[index], explicitArguments[index]);
		}

		for (std::size_t index = 0; index < argumentTypes.size(); ++index)
		{
			if (!Unify(function.parameterTypes[index], argumentTypes[index], templateParameters, bindings))
			{
				return {};
			}
		}

		Bindings variables;
		for (std::size_t index = 0; index < argumentTypes.size(); ++index)
		{
			variables.emplace(function.parameterNames[index],
				Substitute(function.parameterTypes[index], bindings));
		}

		std::string result;
		for (const auto& recipe : function.returns)
		{
			bool selected = true;
			for (const auto& condition : recipe.conditions)
			{
				const auto value = Condition(Substitute(condition.expression, bindings));
				if (!value)
				{
					return {};
				}

				selected &= *value != condition.negated;
				if (!selected)
				{
					break;
				}
			}

			if (!selected)
			{
				continue;
			}

			const auto type = ExpressionType(Substitute(recipe.expression, bindings), variables,
				scopes, owner, preserveReference);
			if (type.empty() ||(!result.empty() && result != type))
			{
				return {};
			}

			result = type;
		}

		return result;
	}
}
