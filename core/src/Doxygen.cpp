#include <Heimdall/Doxygen.hpp>

#include <algorithm>
#include <cctype>

namespace heimdall
{
	namespace
	{
		constexpr std::size_t kDocumentationPrefixBytes = 3;
		constexpr std::size_t kScopeSeparatorBytes      = 2;

		bool Word(char value)
		{
			return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_';
		}

		std::size_t SkipSpace(std::string_view text, std::size_t position)
		{
			while (position < text.size() && (text[position] == ' ' || text[position] == '\t'))
			{
				++position;
			}

			return position;
		}

		void AddReference(
			std::vector<DoxygenReference>& result,
			std::string_view text,
			std::size_t begin,
			std::size_t end,
			const Token& comment,
			bool parameter)
		{
			const auto target    = text.substr(begin, end - begin);
			const auto signature = target.find('(');
			const auto nameEnd   = signature == std::string_view::npos ? target.size() : signature;
			const auto scope     = target.substr(0, nameEnd).rfind("::");
			const auto nameBegin =
				scope == std::string_view::npos ? 0 : scope + kScopeSeparatorBytes;
			result.push_back(DoxygenReference {
					comment.offset + begin, end - begin, comment.offset + begin + nameBegin,
					nameEnd - nameBegin, comment.offset + comment.length, std::string(target),
					parameter});
		}
	}

	std::vector<DoxygenReference> DoxygenReferences(const ParseTree& tree)
	{
		std::vector<DoxygenReference> result;
		bool code = false;
		for (const auto& token : tree.Tokens())
		{
			const auto text = tree.Text(token);
			if (token.kind == TokenKind::Whitespace)
			{
				continue;
			}

			if (!(text.starts_with("/**") || text.starts_with("/*!") ||
				text.starts_with("///") || text.starts_with("//!")))
			{
				code = false;
				continue;
			}

			for (std::size_t position = kDocumentationPrefixBytes; position < text.size(); ++position)
			{
				if ((text[position] != '@' && text[position] != '\\') ||
					(position > 0 && (text[position - 1] == '\\' || text[position - 1] == '@')))
				{
					continue;
				}

				const auto begin =++position;
				while (position < text.size() && Word(text[position]))
				{
					++position;
				}

				const auto command = text.substr(begin, position - begin);
				if (command == "endcode" || command == "endverbatim")
				{
					code = false;
					continue;
				}

				if (command == "code" || command == "verbatim")
				{
					code = true;
					continue;
				}

				if (code ||(command != "param" && command != "ref" &&
					command != "see" && command != "sa"))
				{
					continue;
				}

				const bool parameter = command == "param";
				position             = SkipSpace(text, position);
				if (parameter && position < text.size() && text[position] == '[')
				{
					const auto close = text.find(']', position);
					if (close == std::string_view::npos)
					{
						continue;
					}

					position = SkipSpace(text, close + 1);
				}

				while (position < text.size() && Word(text[position]))
				{
					const auto targetBegin = position;
					while (position < text.size() &&
						(Word(text[position]) || text[position] == ':'))
					{
						++position;
					}

					if (!parameter && position < text.size() && text[position] == '(')
					{
						const auto close = text.find(')', position);
						if (close == std::string_view::npos)
						{
							break;
						}

						position = close + 1;
					}

					AddReference(result, text, targetBegin, position, token, parameter);
					if (command == "ref")
					{
						position = SkipSpace(text, position);
						if (position < text.size() && text[position] == '"')
						{
							const auto close = text.find('"', position + 1);
							if (close != std::string_view::npos)
							{
								position = close + 1;
							}
						}

						break;
					}

					position = SkipSpace(text, position);
					if (position >= text.size() || text[position] != ',')
					{
						break;
					}

					position = SkipSpace(text, position + 1);
				}

				if (position > 0)
				{
					--position;
				}
			}

			if (token.kind == TokenKind::BlockComment)
			{
				code = false;
			}
		}

		return result;
	}

	namespace
	{
		std::string RenderFragment(std::string_view text)
		{
			std::string result;
			bool code = false;
			for (std::size_t position = 0; position < text.size();)
			{
				if ((text[position] != '@' && text[position] != '\\') ||
					(position > 0 && (text[position - 1] == '\\' || text[position - 1] == '@')))
				{
					result += text[position++];
					continue;
				}

				const auto start = position++;
				const auto begin = position;
				while (position < text.size() && Word(text[position]))
				{
					++position;
				}

				const auto command = text.substr(begin, position - begin);
				if (command == "code" || command == "verbatim")
				{
					code = true;
					result += "\n```\n";
				}
				else if (command == "endcode" || command == "endverbatim")
				{
					code = false;
					result += "\n```\n";
				}
				else if (!code && command == "ref")
				{
					position = SkipSpace(text, position);
				}
				else
				{
					result += text.substr(start, position - start);
				}
			}

			return result;
		}

		std::string_view Trim(std::string_view text)
		{
			const auto first = text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos)
			{
				return {};
			}

			return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
		}

		struct Section
		{
			std::string_view command;
			std::size_t begin = 0;
			std::size_t end   = 0;
		};

		void Append(std::string& destination, std::string_view text, std::string_view separator)
		{
			if (text.empty())
			{
				return;
			}

			if (!destination.empty())
			{
				destination += separator;
			}

			destination += text;
		}

		std::string ParameterBullets(std::string_view text)
		{
			text = Trim(text);
			std::string_view direction;
			if (text.starts_with('['))
			{
				const auto end = text.find(']');
				if (end == std::string_view::npos)
				{
					return RenderFragment(text);
				}

				direction = text.substr(1, end - 1);
				text      = Trim(text.substr(end + 1));
			}

			std::size_t end = 0;
			while (end < text.size())
			{
				while (end < text.size() && Word(text[end]))
				{
					++end;
				}

				const auto comma = SkipSpace(text, end);
				if (comma >= text.size() || text[comma] != ',')
				{
					break;
				}

				end = SkipSpace(text, comma + 1);
			}

			const auto names = text.substr(0, end);
			const auto description = end == text.size() ? std::string {}
			: RenderFragment(Trim(text.substr(end)));
			std::string bullets;
			for (std::size_t position = 0; position < names.size();)
			{
				const auto comma = names.find(',', position);
				const auto name = Trim(names.substr(position,
					comma == std::string_view::npos ? names.size() - position : comma - position));
				if (name.empty())
				{
					break;
				}

				std::string bullet = "- `" + std::string(name) + "`";
				if (!direction.empty())
				{
					bullet += " (" + std::string(direction) + ")";
				}

				if (!description.empty())
				{
					bullet += " \xE2\x80\x94 ";
					for (const char value : description)
					{
						bullet += value;
						if (value == '\n')
						{
							bullet += "  ";
						}
					}
				}

				Append(bullets, bullet, "\n");
				position = comma == std::string_view::npos ? names.size() : comma + 1;
			}

			return bullets;
		}
	}

	std::string RenderDoxygen(std::string_view text)
	{
		std::vector<Section> sections;
		Section current;
		bool code = false;
		for (std::size_t position = 0; position < text.size(); ++position)
		{
			if ((text[position] != '@' && text[position] != '\\') ||
				(position > 0 && (text[position - 1] == '\\' || text[position - 1] == '@')))
			{
				continue;
			}

			const auto start = position;
			const auto begin =++position;
			while (position < text.size() && Word(text[position]))
			{
				++position;
			}

			const auto command = text.substr(begin, position - begin);
			if (command == "code" || command == "verbatim")
			{
				code = true;
			}
			else if (command == "endcode" || command == "endverbatim")
			{
				code = false;
			}
			else if (!code && (command == "brief" || command == "short" || command == "details" ||
				command == "param" || command == "tparam" || command == "return" ||
				command == "returns" || command == "result" ||
				command == "see" || command == "sa"))
			{
				current.end = start;
				sections.push_back(current);
				current = Section {command, position, text.size()};
			}

			if (position > 0)
			{
				--position;
			}
		}

		current.end = text.size();
		sections.push_back(current);
		std::string summary;
		std::string parameters;
		std::string templateParameters;
		std::string returns;
		std::string seeAlso;
		for (const auto& section : sections)
		{
			const auto body = Trim(text.substr(section.begin, section.end - section.begin));
			if (section.command == "param" || section.command == "tparam")
			{
				Append(section.command == "param" ? parameters : templateParameters,
					ParameterBullets(body), "\n");
			}
			else
			{
				const auto rendered = RenderFragment(body);
				auto& destination = section.command == "return" || section.command == "returns" ||
					section.command == "result" ? returns
				: section.command == "see" || section.command == "sa" ? seeAlso : summary;
				Append(destination, rendered, "\n\n");
			}
		}

		const auto addSection = [&](std::string_view title, const std::string& body)
		{
			if (!body.empty())
			{
				Append(summary, "**" + std::string(title) + "**\n\n" + body, "\n\n");
			}
		};
		addSection("Template parameters", templateParameters);
		addSection("Parameters", parameters);
		addSection("Returns", returns);
		addSection("See also", seeAlso);
		return summary;
	}
}
