#include "MacroProtocol.hpp"

#include <Heimdall/MacroExpander.hpp>

#include <filesystem>

namespace heimdall::lsp
{

	namespace
	{

		constexpr std::size_t kMaxBodyChars       = 200;
		constexpr std::size_t kMaxInvocationChars = 4096;
		constexpr std::string_view kDefineKeyword  = "define";

		bool IsWordChar(char c) noexcept
		{
			return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') || c == '_' ||
				(c >= '0' && c <= '9');
		}

		bool IsBlank(char c) noexcept
		{
			return c == ' ' || c == '\t';
		}

		std::string_view LineAround(std::string_view text, std::size_t offset,
			std::size_t& lineBegin)
		{
			lineBegin = offset > text.size() ? text.size() : offset;
			while (lineBegin > 0 && text[lineBegin - 1] != '\n')
			{
				--lineBegin;
			}

			std::size_t lineEnd = text.find('\n', offset > text.size() ? text.size() : offset);
			lineEnd             = lineEnd == std::string_view::npos ? text.size() : lineEnd;
			return text.substr(lineBegin, lineEnd - lineBegin);
		}

		bool IsDirectiveLine(std::string_view line) noexcept
		{
			std::size_t i = 0;
			while (i < line.size() && IsBlank(line[i]))
			{
				++i;
			}

			return i < line.size() && line[i] == '#';
		}

		std::string Cut(std::string text, std::size_t maxChars)
		{
			if (text.size() > maxChars)
			{
				text.resize(maxChars);
				text += "...";
			}

			return text;
		}

	} // namespace

	std::optional<WordRange> IdentifierAt(std::string_view text, std::size_t offset)
	{
		if (offset > text.size())
		{
			return std::nullopt;
		}

		std::size_t begin = offset;
		while (begin > 0 && IsWordChar(text[begin - 1]))
		{
			--begin;
		}

		std::size_t end = offset;
		while (end < text.size() && IsWordChar(text[end]))
		{
			++end;
		}

		if (begin == end ||(text[begin] >= '0' && text[begin] <= '9'))
		{
			return std::nullopt;
		}

		return WordRange {begin, end};
	}

	const MacroDef* MacroAt(const MacroEnvironment& env, std::string_view text, WordRange word)
	{
		const std::string_view name = text.substr(word.begin, word.end - word.begin);
		if (const MacroDef * live = env.Find(name, word.begin))
		{
			return live;
		}

		std::size_t lineBegin       = 0;
		const std::string_view line = LineAround(text, word.begin, lineBegin);
		if (IsDirectiveLine(line))
		{
			return env.Find(name, lineBegin + line.size() + 1);
		}

		return nullptr;
	}

	std::string MacroSignature(const MacroDef& macro)
	{
		std::string text = "#define " + macro.name;
		if (macro.functionLike)
		{
			text += '(';
			for (std::size_t i = 0; i < macro.params.size(); ++i)
			{
				if (i != 0)
				{
					text += ", ";
				}

				const bool unnamedVariadic =
					macro.variadic && i + 1 == macro.params.size() && macro.params[i] == "__VA_ARGS__";
				text += unnamedVariadic ? "..." : macro.params[i];
				if (macro.variadic && i + 1 == macro.params.size() && !unnamedVariadic)
				{
					text += "...";
				}
			}

			text += ')';
		}

		if (!macro.body.empty())
		{
			std::string body = macro.body;
			for (char& c : body)
			{
				c = c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
			}

			text += ' ' + Cut(std::move(body), kMaxBodyChars);
		}

		return text;
	}

	namespace
	{

		// End of the invocation that starts at `word`: the name, or the name and its balanced
		// argument list. Empty when a function-like name is not called.
		std::optional<std::size_t> InvocationEnd(std::string_view text, WordRange word,
												 const MacroDef& macro)
		{
			if (!macro.functionLike)
			{
				return word.end;
			}

			std::size_t paren = word.end;
			while (paren < text.size() && (IsBlank(text[paren]) || text[paren] == '\n'))
			{
				++paren;
			}

			if (paren >= text.size() || text[paren] != '(')
			{
				return std::nullopt;
			}

			std::size_t level = 0;
			std::size_t i     = paren;
			for (; i < text.size() && i - word.begin < kMaxInvocationChars; ++i)
			{
				level += text[i] == '(' ? 1 : 0;
				if (text[i] == ')' && --level == 0)
				{
					break;
				}
			}

			if (i >= text.size() || text[i] != ')')
			{
				return std::nullopt;
			}

			return i + 1;
		}

		struct Expanded
		{
			std::string text;
			bool clean = true;
		};

		Expanded ExpandInvocation(const MacroEnvironment& env, std::string_view invocation,
								  std::size_t offset)
		{
			const heimdall::MacroTable table       = env.At(offset);
			const heimdall::ExpansionResult result = heimdall::MacroExpander(table).Expand(invocation);
			Expanded expanded;
			expanded.clean = !result.truncated && result.diagnostics.empty();
			for (const heimdall::ExpandedToken& token : result.tokens)
			{
				if (!expanded.text.empty() && token.spaceBefore)
				{
					expanded.text += ' ';
				}

				expanded.text += token.text;
			}

			return expanded;
		}

		bool IsOperatorChar(char c) noexcept
		{
			return std::string_view("+-*/%&|^<>=!:.#~?").find(c) != std::string_view::npos;
		}

	} // namespace

	std::string MacroExpansionPreview(
		const MacroEnvironment& env,
		std::string_view text,
		WordRange word,
		const MacroDef& macro,
		std::size_t maxChars)
	{
		const auto end = InvocationEnd(text, word, macro);
		if (!end)
		{
			return {};
		}

		const std::string_view invocation = text.substr(word.begin, *end - word.begin);
		auto expanded                     = ExpandInvocation(env, invocation, word.begin);
		if (expanded.text == invocation)
		{
			return {};
		}

		return Cut(std::move(expanded.text), maxChars);
	}

	std::optional<MacroExpansionEdit> MacroExpansionForEdit(
		const MacroEnvironment& env,
		std::string_view text,
		WordRange word,
		const MacroDef& macro)
	{
		std::size_t lineBegin       = 0;
		const std::string_view line = LineAround(text, word.begin, lineBegin);
		if (IsDirectiveLine(line))
		{
			return std::nullopt;
		}

		const auto end = InvocationEnd(text, word, macro);
		if (!end)
		{
			return std::nullopt;
		}

		const std::string_view invocation = text.substr(word.begin, *end - word.begin);
		auto expanded                     = ExpandInvocation(env, invocation, word.begin);
		if (!expanded.clean || expanded.text == invocation ||
			expanded.text.find('\n') != std::string::npos)
		{
			return std::nullopt;
		}

		// Keep neighbouring operators from fusing into a different token (`a-` + `-1`).
		if (!expanded.text.empty())
		{
			if (word.begin > 0 && IsOperatorChar(text[word.begin - 1]) &&
				IsOperatorChar(expanded.text.front()))
			{
				expanded.text.insert(expanded.text.begin(), ' ');
			}

			if (*end < text.size() && IsOperatorChar(text[*end]) &&
				IsOperatorChar(expanded.text.back()))
			{
				expanded.text.push_back(' ');
			}
		}

		return MacroExpansionEdit { word.begin, *end, std::move(expanded.text) };
	}

	std::string MacroHoverMarkdown(const MacroDef& macro, std::string_view expansion)
	{
		std::string value = "```cpp\n" + MacroSignature(macro) + "\n```";
		if (!expansion.empty())
		{
			value += "\n\nExpands to:\n```cpp\n" + std::string(expansion) + "\n```";
		}

		switch (macro.origin)
		{
		case heimdall::MacroOrigin::Builtin:
			value += "\n\n_Predefined by the compiler_";
			break;
		case heimdall::MacroOrigin::CommandLine:
			value += "\n\n_Defined on the compile command_";
			break;
		case heimdall::MacroOrigin::File:
			value += "\n\n`" + std::filesystem::path(macro.file).filename().string() + "`";
			break;
		}

		return value;
	}

	std::optional<WordRange> DefinitionNameRange(std::string_view fileText,
		std::size_t directiveOffset, std::string_view name)
	{
		if (directiveOffset >= fileText.size())
		{
			return std::nullopt;
		}

		std::size_t lineBegin       = 0;
		const std::string_view line = LineAround(fileText, directiveOffset, lineBegin);
		const std::size_t keyword   = line.find(kDefineKeyword);
		if (keyword == std::string_view::npos)
		{
			return std::nullopt;
		}

		std::size_t i = keyword + kDefineKeyword.size();
		while (i < line.size() && IsBlank(line[i]))
		{
			++i;
		}

		if (line.substr(i, name.size()) != name)
		{
			return std::nullopt;
		}

		return WordRange {lineBegin + i, lineBegin + i + name.size()};
	}

} // namespace heimdall::lsp
