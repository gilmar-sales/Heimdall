#include <Heimdall/IncludeInsertion.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{

	namespace
	{
		struct Line
		{
			std::size_t begin = 0;
			std::size_t end   = 0; // past the newline, or the end of the source
			std::string_view text; // without the newline
		};

		std::vector<Line> SplitLines(std::string_view source)
		{
			std::vector<Line> lines;
			std::size_t begin = 0;
			while (begin < source.size())
			{
				const std::size_t newline = source.find('\n', begin);
				const std::size_t stop    = newline == std::string_view::npos ? source.size() : newline;
				std::string_view text     = source.substr(begin, stop - begin);
				if (!text.empty() && text.back() == '\r')
				{
					text.remove_suffix(1);
				}

				lines.push_back({begin, newline == std::string_view::npos ? stop : stop + 1, text});
				begin = newline == std::string_view::npos ? source.size() : newline + 1;
			}

			return lines;
		}

		std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
			{
				text.remove_prefix(1);
			}

			while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
			{
				text.remove_suffix(1);
			}

			return text;
		}

		// `# include <x>` -> `include`, rest `<x>`.
		struct Directive
		{
			std::string_view name;
			std::string_view rest;
		};

		std::optional<Directive> ParseDirective(std::string_view line)
		{
			line = Trim(line);
			if (line.empty() || line.front() != '#')
			{
				return std::nullopt;
			}

			line             = Trim(line.substr(1));
			std::size_t name = 0;
			while (name < line.size() && (std::isalnum(static_cast<unsigned char>(line[name])) ||
				line[name] == '_'))
			{
				++name;
			}

			return Directive {line.substr(0, name), Trim(line.substr(name))};
		}

		// The header of an include directive with its delimiters, comments dropped.
		std::string_view IncludeTarget(std::string_view rest)
		{
			if (rest.empty() ||(rest.front() != '<' && rest.front() != '"'))
			{
				return {};
			}

			const char close        = rest.front() == '<' ? '>' : '"';
			const std::size_t found = rest.find(close, 1);
			return found == std::string_view::npos ? std::string_view {}: rest.substr(0, found + 1);
		}

		bool LessFolded(std::string_view left, std::string_view right)
		{
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
				[](char a, char b)
				{
					return std::tolower(static_cast<unsigned char>(a)) <
					std::tolower(static_cast<unsigned char>(b));
			});
		}

		struct Include
		{
			std::size_t line = 0;
			std::string_view target;
		};
	} // namespace

	std::optional<IncludeEdit> PlanIncludeInsertion(std::string_view source,
		std::string_view spelled)
	{
		const std::vector<Line> lines = SplitLines(source);
		std::vector<Include> includes; // top-level, in the leading directive region
		std::size_t after_guard = 0;   // line index after `#pragma once` / guard define
		bool has_guard          = false;
		std::size_t comment_end = 0; // line index after the leading comment block
		bool in_block_comment   = false;
		bool leading            = true; // still before the first line of code
		bool leading_comment    = true;
		int depth               = 0; // conditional nesting below the guard
		std::string_view guard_macro;
		for (std::size_t i = 0; i < lines.size(); ++i)
		{
			const std::string_view text = Trim(lines[i].text);
			if (in_block_comment)
			{
				in_block_comment = text.find("*/") == std::string_view::npos;
				comment_end      = leading_comment ? i + 1 : comment_end;
				continue;
			}

			if (text.empty())
			{
				leading_comment = leading_comment && comment_end == 0;
				continue;
			}

			if (text.starts_with("//") || text.starts_with("/*"))
			{
				in_block_comment = text.starts_with("/*") && text.find("*/", 2) == std::string_view::npos;
				comment_end      = leading_comment ? i + 1 : comment_end;
				continue;
			}

			leading_comment      = false;
			const auto directive = ParseDirective(text);
			if (!directive)
			{
				leading = false;
				continue;
			}

			if (directive->name == "include" || directive->name == "import" ||
				directive->name == "include_next")
			{
				const std::string_view target = IncludeTarget(directive->rest);
				if (target == spelled)
				{
					return std::nullopt;
				}

				if (leading && depth == 0 && !target.empty())
				{
					includes.push_back({i, target});
				}

				continue;
			}

			if (!leading)
			{
				continue;
			}

			if (directive->name == "pragma" && Trim(directive->rest) == "once")
			{
				after_guard = i + 1;
				has_guard   = true;
			}
			else if (directive->name == "ifndef" && !has_guard && includes.empty())
			{
				guard_macro = directive->rest;
			}
			else if (directive->name == "define" && !guard_macro.empty() && !has_guard &&
				directive->rest.starts_with(guard_macro))
			{
				after_guard = i + 1;
				has_guard   = true;
			}
			else if (directive->name.starts_with("if"))
			{
				++depth;
			}
			else if (directive->name == "endif" && depth > 0)
			{
				--depth;
			}
		}

		const bool angled = spelled.starts_with("<");
		std::size_t line  = 0;
		if (!includes.empty())
		{
			// Same-kind group: sorted position inside it. No such group: angled
			// before the first quoted include, quoted after the last include.
			const Include* first_same    = nullptr;
			const Include* last_same     = nullptr;
			const Include* insert_before = nullptr;
			for (const auto& include : includes)
			{
				if (include.target.starts_with("<") != angled)
				{
					continue;
				}

				first_same = first_same == nullptr ? &include : first_same;
				last_same  = &include;
				if (insert_before == nullptr && LessFolded(spelled, include.target))
				{
					insert_before = &include;
				}
			}

			if (insert_before != nullptr)
			{
				line = insert_before->line;
			}
			else if (last_same != nullptr)
			{
				line = last_same->line + 1;
			}
			else if (angled)
			{
				line = includes.front().line;
			}
			else
			{
				line = includes.back().line + 1;
			}
		}
		else
		{
			line = has_guard ? after_guard : comment_end;
		}

		IncludeEdit edit;
		edit.text = "#include " + std::string(spelled) + "\n";
		if (line < lines.size())
		{
			edit.offset = lines[line].begin;
		}
		else
		{
			edit.offset = source.size();
			if (!source.empty() && source.back() != '\n')
			{
				edit.text.insert(0, "\n");
			}
		}

		return edit;
	}

} // namespace heimdall
