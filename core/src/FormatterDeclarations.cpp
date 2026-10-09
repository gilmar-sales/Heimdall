#include <Heimdall/Formatter.hpp>

#include <algorithm>

namespace heimdall
{
	std::string Formatter::DeclarationLayout(const ParseTree& tree) const
	{
		const auto source  = tree.Source();
		const auto& tokens = tree.Tokens();
		const auto& nodes  = tree.NodesSoA();
		struct Edit
		{
			std::size_t offset, length;
			std::string text;
		};

		std::vector<Edit> edits;
		const std::string newline = source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
		const auto token_end =[&](std::size_t token)
		{
			return tokens[token].offset + tokens[token].length;
		};
		const auto whitespace =[](char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n';
		};
		const auto replace_gap =
			[&](std::size_t from, std::size_t to, const std::string& replacement)
		{
			if (from > to || to > source.size())
			{
				return;
			}

			const auto gap = source.substr(from, to - from);
			if (std::all_of(gap.begin(), gap.end(), whitespace) && gap != replacement)
			{
				edits.push_back({from, to - from, replacement});
			}
		};

		for (std::size_t node = 0; node < nodes.size(); ++node)
		{
			if (m_options.max_parameters_per_line &&
				nodes.Kind(node) == GrammarKind::FunctionSuffix)
			{
				std::vector<std::size_t> parameters;
				for (const auto child : tree.DirectChildren(node))
				{
					if (nodes.Kind(child) == GrammarKind::ParameterDeclaration)
					{
						parameters.push_back(nodes.FirstToken(child));
					}
				}

				const auto open = nodes.FirstToken(node);
				const auto end  = open + nodes.TokenCount(node);
				if (open >= tokens.size() || end > tokens.size() || tokens[open].tok != Tok::LParen)
				{
					continue;
				}

				std::size_t close = open, depth = 0;
				for (; close < end; ++close)
				{
					if (tokens[close].tok == Tok::LParen)
					{
						++depth;
					}
					else if (tokens[close].tok == Tok::RParen && --depth == 0)
					{
						break;
					}
				}

				if (close == end)
				{
					continue;
				}

				auto final_token = close;
				while (final_token > open && tokens[final_token - 1].kind == TokenKind::Whitespace)
				{
					--final_token;
				}

				if (final_token > open && tokens[final_token - 1].tok == Tok::Ellipsis)
				{
					auto before = final_token - 1;
					while (before > open && tokens[before - 1].kind == TokenKind::Whitespace)
					{
						--before;
					}

					if (before > open && tokens[before - 1].tok == Tok::Comma)
					{
						parameters.push_back(final_token - 1);
					}
				}

				if (parameters.size() <= m_options.max_parameters_per_line)
				{
					continue;
				}

				// The parser has already distinguished parameters from arguments,
				// template commas, nested declarators and default expressions.
				for (const auto first : parameters)
				{
					auto delimiter = first;
					while (delimiter > open &&
						(tokens[delimiter - 1].kind == TokenKind::Whitespace ||
						tokens[delimiter - 1].kind == TokenKind::LineComment ||
						tokens[delimiter - 1].kind == TokenKind::BlockComment))
					{
						--delimiter;
					}

					if (delimiter > open && (tokens[delimiter - 1].tok == Tok::Comma ||
						tokens[delimiter - 1].tok == Tok::LParen))
					{
						auto next = delimiter;
						while (next < first && tokens[next].kind == TokenKind::Whitespace)
						{
							++next;
						}

						replace_gap(token_end(delimiter - 1), tokens[next].offset, newline);
					}
				}

				auto last = close;
				while (last > open && tokens[last - 1].kind == TokenKind::Whitespace)
				{
					--last;
				}

				if (last > open && tokens[last - 1].kind != TokenKind::LineComment)
				{
					replace_gap(token_end(last - 1), tokens[close].offset, "");
				}
			}

			const auto kind = nodes.Kind(node);
			if (!m_options.blank_line_between_methods ||
				(kind != GrammarKind::RecordDefinition &&
				kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::TranslationUnit))
			{
				continue;
			}

			std::size_t previous = nodes.size();
			bool previous_method = false;
			bool previous_access = false;
			for (const auto child : tree.DirectChildren(node))
			{
				auto method = child;
				while (nodes.Kind(method) == GrammarKind::TemplateDeclaration)
				{
					auto nested = nodes.size();
					for (const auto candidate : tree.DirectChildren(method))
					{
						if (nodes.Kind(candidate) == GrammarKind::FunctionDefinition ||
							nodes.Kind(candidate) == GrammarKind::FunctionDeclaration ||
							nodes.Kind(candidate) == GrammarKind::TemplateDeclaration)
						{
							nested = candidate;
						}
					}

					if (nested == nodes.size())
					{
						break;
					}

					method = nested;
				}

				bool is_method = nodes.Kind(method) == GrammarKind::FunctionDefinition;
				if (nodes.Kind(method) == GrammarKind::FunctionDeclaration)
				{
					// A field initialized by a call may also be represented by a
					// FunctionDeclaration in recovery. A method's declared name
					// is followed by its parameter list, unlike `(*callback)(...)`.
					std::vector<std::size_t> pending {method};
					while (!pending.empty() && !is_method)
					{
						const auto candidate = pending.back();
						pending.pop_back();
						if (nodes.Kind(candidate) == GrammarKind::DeclaredName)
						{
							auto next = nodes.FirstToken(candidate) + nodes.TokenCount(candidate);
							while (next < tokens.size() &&
								(tokens[next].kind == TokenKind::Whitespace ||
								tokens[next].kind == TokenKind::LineComment ||
								tokens[next].kind == TokenKind::BlockComment))
							{
								++next;
							}

							is_method = next < tokens.size() && tokens[next].tok == Tok::LParen;
						}
						else if (nodes.Kind(candidate) != GrammarKind::FunctionSuffix)
						{
							for (const auto nested : tree.DirectChildren(candidate))
							{
								pending.push_back(nested);
							}
						}
					}
				}

				const bool is_access = nodes.Kind(method) == GrammarKind::AccessSpecifier;
				if (nodes.Kind(method) == GrammarKind::PreprocessorDirective)
				{
					previous        = nodes.size();
					previous_method = false;
					previous_access = false;
					continue;
				}

				// Recovery on a standalone fragment can classify a control
				// header as a definition. Never separate its attached handler.
				const auto first_token = nodes.FirstToken(method);
				if (first_token >= tokens.size())
				{
					previous = nodes.size();
					continue;
				}

				const auto head = tokens[first_token].tok;
				if (head == Tok::KwIf || head == Tok::KwFor || head == Tok::KwWhile ||
					head == Tok::KwSwitch || head == Tok::KwCatch || head == Tok::KwElse ||
					head == Tok::KwDo || head == Tok::KwTry)
				{
					previous = nodes.size();
					continue;
				}

				const bool record_boundary =
					kind == GrammarKind::RecordDefinition && !previous_access &&
					(is_method || previous_method || is_access);
				if (previous != nodes.size() && (record_boundary ||(previous_method&& is_method)))
				{
					const auto previous_end =
						nodes.FirstToken(previous) + nodes.TokenCount(previous);
					const auto next_start = nodes.FirstToken(child);
					if (previous_end && previous_end <= next_start && next_start < tokens.size())
					{
						auto from = token_end(previous_end - 1);
						auto to   = tokens[next_start].offset;
						// Put the blank before documentation, never between a
						// comment and the method it documents. A trailing comment
						// on the previous method's line stays with that method.
						bool same_line = true;
						for (auto t = previous_end; t < next_start; ++t)
						{
							if (tokens[t].kind == TokenKind::Whitespace)
							{
								if (source.substr(tokens[t].offset, tokens[t].length).find('\n') !=
									std::string_view::npos)
								{
									same_line = false;
								}
							}
							else if (tokens[t].kind == TokenKind::LineComment ||
								tokens[t].kind == TokenKind::BlockComment)
							{
								if (same_line)
								{
									from = token_end(t);
								}
								else
								{
									to = tokens[t].offset;
									break;
								}
							}
							else
							{
								to = from;
								break;
							} // directive or other member
						}

						const auto gap = source.substr(from, to - from);
						if (std::count(gap.begin(), gap.end(), '\n') < 2)
						{
							replace_gap(from, to, newline + newline);
						}
					}
				}

				previous        = child;
				previous_method = is_method;
				previous_access = is_access;
			}
		}

		std::sort(edits.begin(), edits.end(),
			[](const Edit& a, const Edit& b)
			{
				return a.offset > b.offset;
		});
		std::string output(source);
		std::size_t boundary = source.size();
		for (const auto& edit : edits)
		{
			if (edit.offset + edit.length > boundary)
			{
				continue;
			}

			output.replace(edit.offset, edit.length, edit.text);
			boundary = edit.offset;
		}

		return output;
	}
} // namespace heimdall
