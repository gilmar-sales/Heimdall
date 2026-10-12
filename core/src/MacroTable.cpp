#include <Heimdall/MacroTable.hpp>

#include <algorithm>
#include <unordered_set>

namespace heimdall
{

	namespace
	{

		constexpr std::uint64_t kFieldSeparator = 0xFF;
		constexpr std::uint64_t kFnvBasis  = 14695981039346656037ull;
		constexpr std::uint64_t kFnvPrime  = 1099511628211ull;
		constexpr std::string_view kVaArgs = "__VA_ARGS__";
		constexpr std::string_view kEllipsis = "...";

		bool IsIdentStart(char c) noexcept
		{
			return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') || c == '_';
		}

		bool IsIdentChar(char c) noexcept
		{
			return IsIdentStart(c) ||(c >= '0' && c <= '9');
		}

		bool IsBlank(char c) noexcept
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
		}

		std::string_view Trim(std::string_view text) noexcept
		{
			while (!text.empty() && IsBlank(text.front()))
			{
				text.remove_prefix(1);
			}

			while (!text.empty() && IsBlank(text.back()))
			{
				text.remove_suffix(1);
			}

			return text;
		}

		// Replaces backslash-newline with a space, as translation phase 2 does.
		std::string SpliceLines(std::string_view text)
		{
			std::string out;
			out.reserve(text.size());
			for (std::size_t i = 0; i < text.size(); ++i)
			{
				if (text[i] == '\\')
				{
					std::size_t next = i + 1;
					if (next < text.size() && text[next] == '\r')
					{
						++next;
					}

					if (next < text.size() && text[next] == '\n')
					{
						out += ' ';
						i = next;
						continue;
					}
				}

				out += text[i];
			}

			return out;
		}

		std::uint64_t HashDefinition(const MacroDef& def)
		{
			std::uint64_t hash = kFnvBasis;
			const auto mix =[&hash](std::string_view text)
			{
				for (const char c : text)
				{
					hash = (hash ^ static_cast<unsigned char>(c)) * kFnvPrime;
				}

				hash = (hash ^ kFieldSeparator) * kFnvPrime;
			};
			mix(def.name);
			mix(def.functionLike ? "f" : "o");
			for (const std::string& param : def.params)
			{
				mix(param);
			}

			for (const MacroBodyToken& token : def.tokens)
			{
				mix(token.spaceBefore ? " " : "");
				mix(std::string_view(def.body).substr(token.token.offset, token.token.length));
			}

			return hash;
		}

		std::expected<void, std::string> ParseParameters(std::string_view text, std::size_t& pos,
			MacroDef& def)
		{
			// `pos` is just past '('.
			while (true)
			{
				while (pos < text.size() && IsBlank(text[pos]))
				{
					++pos;
				}

				if (pos >= text.size())
				{
					return std::unexpected("missing ')' in macro parameter list");
				}

				if (text[pos] == ')' && def.params.empty())
				{
					++pos;
					return {};
				}

				if (text.substr(pos, kEllipsis.size()) == kEllipsis)
				{
					pos += kEllipsis.size();
					def.variadic = true;
					def.params.emplace_back(kVaArgs);
				}
				else if (IsIdentStart(text[pos]))
				{
					const std::size_t begin = pos;
					while (pos < text.size() && IsIdentChar(text[pos]))
					{
						++pos;
					}

					std::string name(text.substr(begin, pos - begin));
					if (std::ranges::find(def.params, name) != def.params.end())
					{
						return std::unexpected("duplicate macro parameter '" + name + "'");
					}

					while (pos < text.size() && IsBlank(text[pos]))
					{
						++pos;
					}

					if (text.substr(pos, kEllipsis.size()) == kEllipsis)
					{
						pos += kEllipsis.size();
						def.variadic = true;
					}

					def.params.push_back(std::move(name));
				}
				else
				{
					return std::unexpected("invalid macro parameter list");
				}

				while (pos < text.size() && IsBlank(text[pos]))
				{
					++pos;
				}

				if (pos >= text.size())
				{
					return std::unexpected("missing ')' in macro parameter list");
				}

				if (text[pos] == ')')
				{
					++pos;
					return {};
				}

				if (text[pos] != ',' || def.variadic)
				{
					return std::unexpected("invalid macro parameter list");
				}

				++pos;
			}
		}

		std::expected<void, std::string> TokenizeBody(std::string_view raw, MacroDef& def)
		{
			def.body   = std::string(Trim(SpliceLines(raw)));
			bool space = false;
			for (const Token& token : Lexer(def.body).Lex())
			{
				if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
					token.kind == TokenKind::BlockComment)
				{
					space = true;
					continue;
				}

				def.tokens.push_back({token, space});
				space = false;
			}

			const auto text =[&def](const MacroBodyToken& token)
			{
				return std::string_view(def.body).substr(token.token.offset, token.token.length);
			};
			if (!def.tokens.empty() &&
				(text(def.tokens.front()) == "##" || text(def.tokens.back()) == "##"))
			{
				return std::unexpected("'##' cannot appear at either end of a macro expansion");
			}

			if (def.functionLike)
			{
				for (std::size_t i = 0; i < def.tokens.size(); ++i)
				{
					if (text(def.tokens[i]) != "#")
					{
						continue;
					}

					const bool operand =
						i + 1 < def.tokens.size() &&
						std::ranges::find(def.params, std::string(text(def.tokens[i + 1]))) !=
						def.params.end();
					if (!operand)
					{
						return std::unexpected("'#' is not followed by a macro parameter");
					}
				}
			}

			return {};
		}

	} // namespace

	std::expected<MacroDef, std::string> MacroTable::ParseDefinition(std::string_view directiveText)
	{
		const std::string spliced   = SpliceLines(directiveText);
		const std::string_view text = Trim(spliced);
		std::size_t pos             = 0;
		while (pos < text.size() && IsIdentChar(text[pos]))
		{
			++pos;
		}

		if (pos == 0 ||!IsIdentStart(text[0]))
		{
			return std::unexpected("#define requires a macro name");
		}

		MacroDef def;
		def.name = std::string(text.substr(0, pos));
		if (pos < text.size() && text[pos] == '(')
		{
			def.functionLike = true;
			++pos;
			if (auto parsed = ParseParameters(text, pos, def); !parsed)
			{
				return std::unexpected(std::move(parsed.error()));
			}

			// GNU `name...`: the named parameter collects the variadic arguments.
		}

		if (auto body = TokenizeBody(text.substr(pos), def); !body)
		{
			return std::unexpected(std::move(body.error()));
		}

		def.hash = HashDefinition(def);
		return def;
	}

	std::expected<MacroDef, std::string> MacroTable::ParseCommandLine(std::string_view argument)
	{
		const std::size_t equals = argument.find('=');
		std::string text(argument.substr(0, equals));
		text += ' ';
		text += equals == std::string_view::npos ? std::string_view("1")
		: argument.substr(equals + 1);
		auto parsed = ParseDefinition(text);
		if (parsed)
		{
			parsed->origin = MacroOrigin::CommandLine;
		}

		return parsed;
	}

	MacroTable MacroTable::FromMap(const Preprocessor::MacroMap& map)
	{
		MacroTable table;
		for (const auto& [name, value] : map)
		{
			std::string text = name;
			text += ' ';
			text += value;
			if (auto parsed = ParseDefinition(text))
			{
				parsed->origin = MacroOrigin::Builtin;
				table.Define(std::move(*parsed));
			}
		}

		return table;
	}

	void MacroTable::Define(MacroDef definition)
	{
		Define(std::make_shared<const MacroDef>(std::move(definition)));
	}

	void MacroTable::Define(MacroPtr definition)
	{
		if (mObserver != nullptr)
		{
			mObserver->Defined(definition);
		}

		std::string name = definition->name;
		mLocal.insert_or_assign(std::move(name), std::move(definition));
	}

	void MacroTable::Undef(std::string_view name)
	{
		if (mObserver != nullptr)
		{
			mObserver->Undefined(name);
		}

		// A null entry hides the definition of an older layer.
		if (Raw(name) == nullptr)
		{
			return;
		}

		mLocal.insert_or_assign(std::string(name), nullptr);
	}

	MacroPtr MacroTable::Lookup(std::string_view name) const noexcept
	{
		MacroPtr found = Raw(name);
		if (mObserver != nullptr)
		{
			mObserver->Read(name, found.get());
		}

		return found;
	}

	const MacroDef* MacroTable::Peek(std::string_view name) const noexcept
	{
		const MacroPtr* found = RawRef(name);
		return found == nullptr ? nullptr : found->get();
	}

	const MacroPtr* MacroTable::RawRef(std::string_view name) const noexcept
	{
		if (const auto it = mLocal.find(name); it != mLocal.end())
		{
			return it->second ? &it->second : nullptr;
		}

		for (const Layer* layer = mBase.get(); layer != nullptr; layer = layer->parent.get())
		{
			if (const auto it = layer->entries.find(name); it != layer->entries.end())
			{
				return it->second ? &it->second : nullptr;
			}
		}

		return nullptr;
	}

	MacroPtr MacroTable::Raw(std::string_view name) const noexcept
	{
		const MacroPtr* found = RawRef(name);
		return found == nullptr ? nullptr : *found;
	}

	const MacroDef* MacroTable::Find(std::string_view name) const noexcept
	{
		const MacroPtr* found = RawRef(name);
		const MacroDef* macro = found == nullptr ? nullptr : found->get();
		if (mObserver != nullptr)
		{
			mObserver->Read(name, macro);
		}

		return macro;
	}

	MacroTable MacroTable::Freeze()
	{
		if (!mLocal.empty())
		{
			auto layer     = std::make_shared<Layer>();
			layer->depth   = mBase ? mBase->depth + 1 : 1;
			layer->parent  = std::move(mBase);
			layer->entries = std::move(mLocal);
			mLocal.clear();
			mBase = std::move(layer);
			if (mBase->depth > kMaxLayerDepth)
			{
				Flatten();
			}
		}

		MacroTable snapshot;
		snapshot.mBase = mBase;
		return snapshot;
	}

	void MacroTable::ForEach(const std::function<void(const MacroDef&) > & visit) const
	{
		std::unordered_set<std::string_view> seen;
		const auto visitEntries =[&](const Entries& entries)
		{
			for (const auto& [name, def] : entries)
			{
				if (seen.insert(name).second && def)
				{
					visit(*def);
				}
			}
		};
		visitEntries(mLocal);
		for (const Layer * layer = mBase.get(); layer != nullptr; layer = layer->parent.get())
		{
			visitEntries(layer->entries);
		}
	}

	std::size_t MacroTable::Size() const
	{
		std::size_t count = 0;
		ForEach([&count](const MacroDef&)
			{
				++count;
		});
		return count;
	}

	void MacroTable::Flatten()
	{
		auto flat = std::make_shared<Layer>();
		std::unordered_set<std::string_view> seen;
		for (const Layer * layer = mBase.get(); layer != nullptr; layer = layer->parent.get())
		{
			for (const auto& [name, def] : layer->entries)
			{
				if (seen.insert(name).second && def)
				{
					flat->entries.emplace(name, def);
				}
			}
		}

		mBase = std::move(flat);
	}

} // namespace heimdall
