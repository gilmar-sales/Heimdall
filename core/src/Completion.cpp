#include <Heimdall/Completion.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Heimdall/Doxygen.hpp>
#include <Heimdall/Lexer.hpp>
#include <Heimdall/Navigation.hpp>
#include <Heimdall/ParseTree.hpp>
#include <Heimdall/Preprocessor.hpp>
#include <Heimdall/TypeLayout.hpp>

namespace heimdall
{

	namespace
	{

		// ---- Flat hash map with interned string IDs (u32) for Completion ----
		// Replaces unordered_map<string, CompletionItem> to avoid string hashing/allocation
		// on hot paths. Uses open addressing with linear probing, power-of-two table.
		class StringInterner
		{
		public:
			// Get or assign an ID for a string. Returns existing ID if present.
			std::uint32_t GetOrAssign(std::string_view s)
			{
				if (s.empty())
				{
					return 0;
				}

				const std::size_t hash = Hash(s);
				const std::size_t mask = m_table.size() - 1;
				std::size_t idx        = hash & mask;

				while (m_table[idx].id != 0)
				{
					if (m_strings[m_table[idx].id] == s)
					{
						return m_table[idx].id;
					}

					idx = (idx + 1) & mask;
				}

				const std::uint32_t new_id = static_cast<std::uint32_t>(m_strings.size());
				m_strings.push_back(std::string(s));
				m_table[idx] = {new_id, hash};
				if (++m_count > m_table.size() / 2)
				{
					Rehash();
				}

				return new_id;
			}

			// Get ID for a string, or 0 if not found.
			std::uint32_t Get(std::string_view s) const
			{
				if (s.empty())
				{
					return 0;
				}

				const std::size_t hash = Hash(s);
				const std::size_t mask = m_table.size() - 1;
				std::size_t idx        = hash & mask;

				while (m_table[idx].id != 0)
				{
					if (m_strings[m_table[idx].id] == s)
					{
						return m_table[idx].id;
					}

					idx = (idx + 1) & mask;
				}

				return 0;
			}

			std::string_view Resolve(std::uint32_t id) const
			{
				return id < m_strings.size() ? m_strings[id] : std::string_view {};
			}

			std::size_t Size() const
			{
				return m_strings.size();
			}

		private:
			struct Entry
			{
				std::uint32_t id = 0;
				std::size_t hash = 0;
			}

			;

			static std::size_t Hash(std::string_view s)
			{
				// FNV-1a 64-bit
				std::uint64_t h = 14695981039346656037ull;
				for (unsigned char c : s)
				{
					h ^= c;
					h *= 1099511628211ull;
				}

				return static_cast<std::size_t>(h);
			}

			void Rehash()
			{
				std::vector<Entry> old = std::move(m_table);
				m_table.assign(old.size() * 2, Entry {});
				m_count = 0;
				for (const auto& e : old)
				{
					if (e.id != 0)
					{
						const std::string_view s = m_strings[e.id];
						const std::size_t idx    = e.hash & (m_table.size() - 1);
						std::size_t i            = idx;
						while (m_table[i].id != 0)
						{
							i = (i + 1) & (m_table.size() - 1);
						}

						m_table[i] = e;
						++m_count;
					}
				}
			}

			std::vector<std::string> m_strings {1}; // index 0 = empty
			std::vector<Entry> m_table {8, Entry {}};
			std::size_t m_count = 0;
		};

		// Flat hash map from interned string ID (u32) to CompletionItem
		// Uses open addressing with linear probing, power-of-two table.
		template <typename Value>
		class FlatHashMap
		{
		public:
			struct Entry
			{
				std::uint32_t key = 0;
				Value value;
			}

			;

			FlatHashMap() = default;

			explicit FlatHashMap(std::size_t reserve)
			{
				Reserve(reserve);
			}

			void Reserve(std::size_t n)
			{
				const std::size_t cap = NextPowerOfTwo(n * 2);
				m_entries.assign(cap, Entry {});
				m_mask = cap - 1;
			}

			// Insert or update. Returns pair<iterator, bool> like unordered_map.
			std::pair<Entry*, bool> InsertOrAssign(std::uint32_t key, Value&& value)
			{
				if (key == 0)
				{
					return {nullptr, false};
				}

				if (m_size > m_entries.size() / 2)
				{
					Rehash();
				}

				std::size_t idx = key & m_mask;
				while (m_entries[idx].key != 0)
				{
					if (m_entries[idx].key == key)
					{
						return {&m_entries[idx], false};
					}

					idx = (idx + 1) & m_mask;
				}

				m_entries[idx] = {key, std::move(value)};
				++m_size;
				return {&m_entries[idx], true};
			}

			// Find by key, returns nullptr if not found.
			Entry* Find(std::uint32_t key)
			{
				if (key == 0 || m_entries.empty())
				{
					return nullptr;
				}

				std::size_t idx = key & m_mask;
				while (m_entries[idx].key != 0)
				{
					if (m_entries[idx].key == key)
					{
						return &m_entries[idx];
					}

					idx = (idx + 1) & m_mask;
				}

				return nullptr;
			}

			const Entry* Find(std::uint32_t key) const
			{
				if (key == 0 || m_entries.empty())
				{
					return nullptr;
				}

				std::size_t idx = key & m_mask;
				while (m_entries[idx].key != 0)
				{
					if (m_entries[idx].key == key)
					{
						return &m_entries[idx];
					}

					idx = (idx + 1) & m_mask;
				}

				return nullptr;
			}

			std::size_t Size() const
			{
				return m_size;
			}

			const std::vector<Entry>& Entries() const
			{
				return m_entries;
			}

			// Iterate all entries (for collecting results)
			template <typename F>
			void ForEach(F&& f) const
			{
				for (const auto& e : m_entries)
				{
					if (e.key != 0)
					{
						f(e.key, e.value);
					}
				}
			}

		private:
			static std::size_t NextPowerOfTwo(std::size_t n)
			{
				std::size_t p = 1;
				while (p < n)
				{
					p <<= 1;
				}

				return std::max(p, std::size_t(8));
			}

			void Rehash()
			{
				std::vector<Entry> old = std::move(m_entries);
				m_entries.assign(old.size() * 2, Entry {});
				m_mask = m_entries.size() - 1;
				m_size = 0;
				for (auto& e : old)
				{
					if (e.key != 0)
					{
						std::size_t idx = e.key & m_mask;
						while (m_entries[idx].key != 0)
						{
							idx = (idx + 1) & m_mask;
						}

						m_entries[idx] = std::move(e);
						++m_size;
					}
				}
			}

			std::vector<Entry> m_entries;
			std::size_t m_mask = 0;
			std::size_t m_size = 0;
		};

		// Helper to collect CompletionItems from FlatHashMap into a sorted vector
		template <typename Map>
		std::vector<CompletionItem> CollectAndSort(const Map& map, const StringInterner& interner)
		{
			(void) interner;
			// Labels are the map keys, so they are unique: sort pointers by label and
			// copy each item once instead of swapping whole items.
			std::vector<const CompletionItem*> sorted;
			sorted.reserve(map.Size());
			map.ForEach([&](std::uint32_t, const CompletionItem& value)
				{
					sorted.push_back(&value);
				});
			std::sort(sorted.begin(), sorted.end(),
				[](const CompletionItem* left, const CompletionItem* right)
				{
					return left->label < right->label;
				});
			std::vector<CompletionItem> items;
			items.reserve(sorted.size());
			for (const CompletionItem* item : sorted)
			{
				items.push_back(*item);
			}

			return items;
		}

		// Overload for string-based labels (when we don't have interner IDs in CompletionItem)
		template <typename Map>
		std::vector<CompletionItem> CollectAndSort(const Map& map)
		{
			std::vector<CompletionItem> items;
			items.reserve(map.Size());
			map.ForEach([&](std::uint32_t key, const CompletionItem& value)
				{
					items.push_back(value);
				});
			std::sort(items.begin(), items.end(),
				[&](const CompletionItem& left, const CompletionItem& right)
				{
					if (left.label != right.label)
					{
						return left.label < right.label;
					}

					return static_cast<int>(left.kind) < static_cast<int>(right.kind);
				});
			return items;
		}

		// ---- End flat hash map ----

		constexpr unsigned int kNonAsciiThreshold = 0x80;
		constexpr int kPriorityMacro              = 5;
		constexpr int kPriorityTypeNamespace      = 4;
		constexpr int kPriorityFunction           = 3;
		constexpr int kPriorityVariableDirective  = 2;
		constexpr std::size_t kMaxParentWalkDepth = 8;
		constexpr std::size_t kMaxScopeWalkDepth  = 32;
		constexpr std::size_t kMaxDetailLen       = 256;
		constexpr std::size_t kMaxShortDetailLen  = 128;
		constexpr std::size_t kMaxTypeTextLen     = 160;
		constexpr int kDoubleAngleCount           = 2;

		constexpr bool IsIdentChar(char c)
		{
			return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') ||(c >= '0' && c <= '9') ||
				c == '_' || static_cast<unsigned char>(c) >= kNonAsciiThreshold;
		}

		constexpr bool IsIdentStart(char c)
		{
			return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') || c == '_' ||
				static_cast<unsigned char>(c) >= kNonAsciiThreshold;
		}

		constexpr std::string_view kKeywords[] = {
			"alignas",
			"alignof",
			"and",
			"and_eq",
			"asm",
			"auto",
			"bitand",
			"bitor",
			"bool",
			"break",
			"case",
			"catch",
			"char",
			"char8_t",
			"char16_t",
			"char32_t",
			"class",
			"compl",
			"concept",
			"const",
			"consteval",
			"constexpr",
			"constinit",
			"const_cast",
			"continue",
			"co_await",
			"co_return",
			"co_yield",
			"decltype",
			"default",
			"delete",
			"do",
			"double",
			"dynamic_cast",
			"else",
			"enum",
			"explicit",
			"export",
			"extern",
			"false",
			"float",
			"for",
			"friend",
			"goto",
			"if",
			"inline",
			"int",
			"long",
			"mutable",
			"namespace",
			"new",
			"noexcept",
			"not",
			"not_eq",
			"nullptr",
			"operator",
			"or",
			"or_eq",
			"private",
			"protected",
			"public",
			"reinterpret_cast",
			"requires",
			"return",
			"short",
			"signed",
			"sizeof",
			"static",
			"static_assert",
			"static_cast",
			"struct",
			"switch",
			"template",
			"this",
			"thread_local",
			"throw",
			"true",
			"try",
			"typedef",
			"typeid",
			"typename",
			"union",
			"unsigned",
			"using",
			"virtual",
			"void",
			"volatile",
			"wchar_t",
			"while",
			"xor",
			"xor_eq",
			"compl",
			"import",
			"module",
			"co_await",
			"co_return",
			"char8_t",
			"char16_t",
			"consteval",
			"constinit",
		};

		constexpr std::string_view kBuiltinTypes[] = {
			"void", "bool", "char", "char8_t", "char16_t", "char32_t",
			"wchar_t", "short", "int", "long", "signed", "unsigned",
			"float", "double", "auto", "void", "typename", "decltype",
		};

		constexpr std::string_view kDirectives[] = {
			"include", "define", "undef", "if", "ifdef", "ifndef",
			"elif", "else", "endif", "pragma", "error", "line",
		};

		bool IsBuiltinType(std::string_view word)
		{
			for (const auto type : kBuiltinTypes)
			{
				if (word == type)
				{
					return true;
				}
			}

			return false;
		}

		bool IsKeyword(std::string_view word)
		{
			for (const auto keyword : kKeywords)
			{
				if (word == keyword)
				{
					return true;
				}
			}

			return false;
		}

		int KindPriority(CompletionKind kind)
		{
			switch (kind)
			{
			case CompletionKind::Macro:
				return kPriorityMacro;
			case CompletionKind::Type:
			case CompletionKind::Namespace:
				return kPriorityTypeNamespace;
			case CompletionKind::Function:
				return kPriorityFunction;
			case CompletionKind::Variable:
				return kPriorityVariableDirective;
			case CompletionKind::Directive:
				return kPriorityVariableDirective;
			case CompletionKind::Keyword:
				return 1;
			}

			return 0;
		}

		std::string KindDetail(CompletionKind kind)
		{
			switch (kind)
			{
			case CompletionKind::Keyword:
				return "keyword";
			case CompletionKind::Type:
				return "type";
			case CompletionKind::Namespace:
				return "namespace";
			case CompletionKind::Function:
				return "function";
			case CompletionKind::Variable:
				return "variable";
			case CompletionKind::Macro:
				return "macro";
			case CompletionKind::Directive:
				return "directive";
			}

			return "";
		}

		bool StartsWith(std::string_view text, std::string_view prefix)
		{
			return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
		}

		constexpr char FoldCase(char c)
		{
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
		}

		// How well a candidate matches the typed word, best first in the ranking.
		enum class MatchQuality : std::uint8_t
		{
			None,
			Fuzzy,
			FoldedPrefix,
			Prefix,
		};

		// Start of a word segment: `Bar` and `bar` in `FooBar`, `foo_bar`, `x2bar`.
		bool IsSegmentStart(std::string_view name, std::size_t at)
		{
			if (at == 0)
			{
				return true;
			}

			const char previous = name[at - 1];
			const char current  = name[at];
			const auto is_lower = [](char c)
			{
				return c >= 'a' && c <= 'z';
			};
			const auto is_upper = [](char c)
			{
				return c >= 'A' && c <= 'Z';
			};
			const auto is_digit = [](char c)
			{
				return c >= '0' && c <= '9';
			};
			return (previous == '_' && current != '_') ||(is_upper(current) && is_lower(previous)) ||
				(!is_digit(current) && is_digit(previous));
		}

		// Exact prefix, then case-insensitive prefix, then a case-insensitive fuzzy
		// match in clangd's style: the first character matches a segment start and
		// each following one either continues the previous match or starts a new
		// segment (`Foba` finds `FooBar`, `ba` finds `BigBang`, `ec` misses `explicit`).
		MatchQuality MatchName(std::string_view name, std::string_view query)
		{
			if (query.empty() || StartsWith(name, query))
			{
				return MatchQuality::Prefix;
			}

			if (name.size() < query.size())
			{
				return MatchQuality::None;
			}

			bool folded = true;
			for (std::size_t i = 0; i < query.size(); ++i)
			{
				if (FoldCase(name[i]) != FoldCase(query[i]))
				{
					folded = false;
					break;
				}
			}

			if (folded)
			{
				return MatchQuality::FoldedPrefix;
			}

			constexpr std::size_t kMaxFuzzyName = 256;
			if (name.size() > kMaxFuzzyName)
			{
				return MatchQuality::None;
			}

			// matched[j]: query[0..i] matches with query[i] at name[j].
			std::array<bool, kMaxFuzzyName> matched {};
			std::array<bool, kMaxFuzzyName> next {};
			for (std::size_t j = 0; j < name.size(); ++j)
			{
				matched[j] = FoldCase(name[j]) == FoldCase(query[0]) && IsSegmentStart(name, j);
			}

			for (std::size_t i = 1; i < query.size(); ++i)
			{
				bool any_before = false;
				for (std::size_t j = 0; j < name.size(); ++j)
				{
					const bool same = FoldCase(name[j]) == FoldCase(query[i]);
					next[j] = same && j > 0 &&
						(matched[j - 1] ||(any_before && IsSegmentStart(name, j)));
					any_before = any_before || matched[j];
				}

				matched = next;
			}

			for (std::size_t j = 0; j < name.size(); ++j)
			{
				if (matched[j])
				{
					return MatchQuality::Fuzzy;
				}
			}

			return MatchQuality::None;
		}

		bool MatchesPrefix(std::string_view name, std::string_view prefix)
		{
			return !name.empty() && MatchName(name, prefix) != MatchQuality::None;
		}

		// Macros only complete from an exact prefix: fuzzy matches on them are noise.
		bool MatchesItem(const CompletionItem& item, std::string_view prefix)
		{
			if (item.kind == CompletionKind::Macro)
			{
				return prefix.empty() || StartsWith(item.label, prefix);
			}

			return MatchesPrefix(item.filter_text.empty() ? std::string_view(item.label)
				: std::string_view(item.filter_text), prefix);
		}

		// Overloaded operators live in the index (smart-pointer dereference
		// resolves through `operator->`) but are noise in member lists unless
		// the user is already typing the name. Plain identifiers that merely
		// start with the word (`operator_of`) are unaffected.
		bool OfferMember(const CompletionItem& item, std::string_view prefix)
		{
			if (!prefix.empty() ||!item.label.starts_with("operator"))
			{
				return true;
			}

			const std::string_view rest = std::string_view(item.label).substr(8);
			return !rest.empty() && IsIdentChar(rest.front());
		}

		// Candidate label for filtering: override suggestions are labeled with the
		// whole signature but filtered by the function name.
		std::string_view FilterTextOf(const CompletionItem& item)
		{
			return item.filter_text.empty() ? std::string_view(item.label)
			: std::string_view(item.filter_text);
		}

		// Records and enums are listed by their owner with the scope item shape
		// (detail `struct ns::Name`); completion reports them as types.
		CompletionItem NormalizeForCompletion(CompletionItem item)
		{
			if (item.kind == CompletionKind::Namespace && !item.detail.starts_with("namespace"))
			{
				item.kind = CompletionKind::Type;
			}

			return item;
		}

		std::vector<CompletionItem> RankItems(
			std::vector<CompletionItem> items,
			std::string_view prefix)
		{
			if (prefix.empty())
			{
				for (auto& item : items)
				{
					item = NormalizeForCompletion(std::move(item));
				}

				return items;
			}

			std::vector<std::pair<MatchQuality, CompletionItem>> ranked;
			ranked.reserve(items.size());
			for (auto& item : items)
			{
				const MatchQuality quality = MatchName(FilterTextOf(item), prefix);
				ranked.emplace_back(quality, NormalizeForCompletion(std::move(item)));
			}

			std::stable_sort(ranked.begin(), ranked.end(),
				[](const auto& left, const auto& right)
				{
					return left.first > right.first;
				});
			items.clear();
			for (auto& [quality, item] : ranked)
			{
				items.push_back(std::move(item));
			}

			return items;
		}

		enum class CursorContext
		{
			Expression,
			MemberAccess,
			ScopeAccess,
			Preprocessor,
			Suppressed,
		};

		// Finds the token holding offset-1 (the character just typed). Returns
		// tokens.size() when offset == 0 or source is empty.
		std::size_t TokenBefore(const std::vector<Token>& tokens, std::size_t offset)
		{
			if (offset == 0)
			{
				return tokens.size();
			}

			const std::size_t pos = offset - 1;
			for (std::size_t i = 0; i < tokens.size(); ++i)
			{
				if (tokens[i].offset <= pos && pos < tokens[i].offset + tokens[i].length)
				{
					return i;
				}
			}

			return tokens.size();
		}

		std::size_t PreviousSignificant(
			const std::vector<Token>& tokens,
			std::size_t index,
			std::string_view source)
		{
			std::size_t i = index;
			while (i > 0)
			{
				--i;
				const TokenKind kind = tokens[i].kind;
				if (kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
					kind == TokenKind::BlockComment)
				{
					continue;
				}

				(void) source;
				return i;
			}

			return tokens.size();
		}

		std::string_view TokenText(std::string_view source, const Token& token)
		{
			return source.substr(token.offset, token.length);
		}

		// Index of the `.`, `->`, `.*` or `::` punctuation token directly preceding
		// the completion prefix (whitespace allowed, e.g. `ns :: name`), or
		// tokens.size() when there is none. Shared by context classification and
		// qualifier extraction so the two can never disagree.
		std::size_t AccessOperatorBefore(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t offset,
			std::string_view prefix)
		{
			const std::size_t before = TokenBefore(tokens, offset);
			// The prefix itself is either the identifier token under the cursor or
			// empty (cursor after punctuation/whitespace).
			bool prefix_is_identifier = false;
			if (before < tokens.size() && tokens[before].kind == TokenKind::Identifier)
			{
				const std::string_view text = TokenText(source, tokens[before]);
				if (!prefix.empty() && text.size() >= prefix.size() &&
					text.substr(text.size() - prefix.size()) == prefix &&
					tokens[before].offset + tokens[before].length >= offset &&
					tokens[before].offset < offset)
				{
					prefix_is_identifier = true;
				}
			}

			std::size_t prev = tokens.size();
			if (prefix_is_identifier)
			{
				prev = PreviousSignificant(tokens, before, source);
			}
			else if (before < tokens.size())
			{
				// Cursor is right after punctuation/whitespace: the token before the
				// cursor is significant unless it is whitespace.
				if (tokens[before].kind == TokenKind::Whitespace)
				{
					prev = PreviousSignificant(tokens, before, source);
				}
				else
				{
					prev = before;
				}
			}
			else
			{
				// Offset past the end: scan back to the last significant token.
				prev = PreviousSignificant(tokens, tokens.size(), source);
			}

			if (prev < tokens.size() && tokens[prev].kind == TokenKind::Punctuation)
			{
				return prev;
			}

			return tokens.size();
		}

		// Whether offset lies in a conditional branch that is provably skipped:
		// `#if 0`, `#if false`, the `#else` of `#if 1`. Conditions it cannot
		// evaluate (macros, `defined`) count as active.
		bool InDisabledBranch(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t offset)
		{
			struct Frame
			{
				bool known  = false;
				bool active = true;
				bool taken  = false;
			};

			enum class Truth : std::uint8_t
			{
				Unknown,
				False,
				True
			};

			const auto evaluate = [](std::string_view condition)
			{
				while (!condition.empty() && (condition.front() == ' ' || condition.front() == '\t'))
				{
					condition.remove_prefix(1);
				}

				const std::size_t comment = condition.find("//");
				condition                 = condition.substr(0, comment);
				const std::size_t block   = condition.find("/*");
				condition                 = condition.substr(0, block);

				while (!condition.empty() &&
					(condition.back() == ' ' || condition.back() == '\t' || condition.back() == '\r'))
				{
					condition.remove_suffix(1);
				}

				if (condition == "0" || condition == "false")
				{
					return Truth::False;
				}

				if (condition == "1" || condition == "true")
				{
					return Truth::True;
				}

				return Truth::Unknown;
			};

			std::vector<Frame> frames;
			for (std::size_t i = 0; i < tokens.size() && tokens[i].offset < offset; ++i)
			{
				if (tokens[i].kind != TokenKind::Punctuation || TokenText(source, tokens[i]) != "#")
				{
					continue;
				}

				std::size_t name = i + 1;
				while (name < tokens.size() && tokens[name].kind == TokenKind::Whitespace &&
					TokenText(source, tokens[name]).find('\n') == std::string_view::npos)
				{
					++name;
				}

				if (name >= tokens.size() || tokens[name].kind != TokenKind::Identifier)
				{
					continue;
				}

				const std::string_view directive = TokenText(source, tokens[name]);
				const std::size_t rest           = tokens[name].offset + tokens[name].length;
				std::size_t line_end             = source.find('\n', rest);
				line_end                         = line_end == std::string_view::npos ? source.size() : line_end;
				const std::string_view condition = source.substr(rest, line_end - rest);
				if (directive == "if")
				{
					const Truth truth = evaluate(condition);
					frames.push_back({truth != Truth::Unknown, truth != Truth::False,
							truth == Truth::True});
				}
				else if (directive == "ifdef" || directive == "ifndef")
				{
					frames.push_back({});
				}
				else if (directive == "elif" && !frames.empty())
				{
					Frame& frame      = frames.back();
					const Truth truth = evaluate(condition);
					if (frame.known && frame.taken)
					{
						frame.active = false;
					}
					else if (frame.known && truth != Truth::Unknown)
					{
						frame.active = truth == Truth::True;
						frame.taken  = frame.active;
					}
					else
					{
						frame = {};
					}
				}
				else if (directive == "else" && !frames.empty())
				{
					Frame& frame = frames.back();
					frame.active = !frame.known ||!frame.taken;
				}
				else if (directive == "endif" && !frames.empty())
				{
					frames.pop_back();
				}
			}

			return std::any_of(frames.begin(), frames.end(),[](const Frame& frame)
				{
					return !frame.active;
				});
		}

		// `int na|`: the word being typed names a new variable or parameter.
		bool NamesNewDeclarator(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t offset,
			std::string_view prefix)
		{
			static constexpr std::string_view kCompleteTypes[] = {
				"bool", "char", "char8_t", "char16_t", "char32_t", "wchar_t", "int", "float",
				"double", "void", "auto"
			};
			std::size_t start = offset - std::min(offset, prefix.size());
			while (start > 0 && (source[start - 1] == ' ' || source[start - 1] == '\t'))
			{
				--start;
			}

			if (start == offset - std::min(offset, prefix.size()))
			{
				return false;
			}

			const std::size_t before = TokenBefore(tokens, start);
			if (before >= tokens.size() || tokens[before].kind != TokenKind::Identifier)
			{
				return false;
			}

			const std::string_view word = TokenText(source, tokens[before]);
			return std::find(std::begin(kCompleteTypes), std::end(kCompleteTypes), word) !=
				std::end(kCompleteTypes);
		}

		CursorContext ClassifyContext(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t offset,
			std::string_view prefix)
		{
			if (offset > source.size())
			{
				offset = source.size();
			}

			const std::size_t before = TokenBefore(tokens, offset);
			if (before < tokens.size())
			{
				const TokenKind kind = tokens[before].kind;
				if (kind == TokenKind::LineComment || kind == TokenKind::BlockComment ||
					kind == TokenKind::StringLiteral || kind == TokenKind::CharacterLiteral ||
					kind == TokenKind::RawStringLiteral)
				{
					return CursorContext::Suppressed;
				}

				if (kind == TokenKind::Number)
				{
					return CursorContext::Suppressed;
				}
			}

			// Preprocessor directive line: `^\s*#\s*\w*$` before the cursor.
			std::size_t line_start = source.rfind('\n', offset > 0 ? offset - 1 : 0);
			line_start             = line_start == std::string_view::npos ? 0 : line_start + 1;
			// If offset is at the start of a line, rfind finds the previous newline;
			// recompute when offset points just after '\n'.
			if (offset > 0 && offset <= source.size() && source[offset - 1] == '\n')
			{
				line_start = offset;
			}

			std::size_t first = line_start;
			while (first < offset && (source[first] == ' ' || source[first] == '\t'))
			{
				++first;
			}

			if (first < offset && source[first] == '#')
			{
				return CursorContext::Preprocessor;
			}

			if (InDisabledBranch(source, tokens, offset))
			{
				return CursorContext::Suppressed;
			}

			// Member / scope access: look at the significant token before the prefix.
			const std::size_t access = AccessOperatorBefore(source, tokens, offset, prefix);
			if (access < tokens.size())
			{
				const std::string_view text = TokenText(source, tokens[access]);
				if (text == "." || text == "->" || text == ".*")
				{
					return CursorContext::MemberAccess;
				}

				if (text == "::")
				{
					return CursorContext::ScopeAccess;
				}
			}

			return CursorContext::Expression;
		}

		CompletionKind ClassifyDeclaredName(const ParseTree& tree, std::size_t node_index)
		{
			std::size_t current = tree.NodesSoA().Parent(node_index);
			for (std::size_t depth = 0;
				depth < kMaxParentWalkDepth && current < tree.NodesSoA().size(); ++depth)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(current);
				if (kind == GrammarKind::ParameterDeclaration ||
					kind == GrammarKind::CompoundStatement ||
					kind == GrammarKind::DeclarationStatement ||
					kind == GrammarKind::ExpressionStatement ||
					kind == GrammarKind::ReturnStatement || kind == GrammarKind::IfStatement ||
					kind == GrammarKind::LoopStatement || kind == GrammarKind::SwitchStatement ||
					kind == GrammarKind::CaseLabel || kind == GrammarKind::TryStatement ||
					kind == GrammarKind::DoStatement || kind == GrammarKind::LambdaExpression)
				{
					return CompletionKind::Variable;
				}

				if (kind == GrammarKind::FunctionDefinition ||
					kind == GrammarKind::FunctionDeclaration)
				{
					return CompletionKind::Function;
				}

				if (kind == GrammarKind::RecordDefinition ||
					kind == GrammarKind::ConceptDefinition ||
					kind == GrammarKind::TemplateDeclaration)
				{
					if (tree.NodesSoA().Parent(node_index) == current ||
						tree.NodesSoA().Parent(tree.NodesSoA().Parent(node_index)) == current)
					{
						return CompletionKind::Type;
					}

					return CompletionKind::Variable;
				}

				if (kind == GrammarKind::Enumerator)
				{
					return CompletionKind::Variable;
				}

				if (kind == GrammarKind::UsingDeclaration)
				{
					return CompletionKind::Type;
				}

				if (kind == GrammarKind::Declaration)
				{
					const auto declaration = tree.NodesSoA()[current];
					const std::size_t last = std::min<std::size_t>(
						declaration.GetFirstToken() + declaration.GetTokenCount(),
						tree.Tokens().size());
					std::size_t seen                          = 0;
					constexpr std::size_t kMaxSpecifierTokens = 4;
					for (std::size_t t = declaration.GetFirstToken();
						t < last && seen < kMaxSpecifierTokens; ++t)
					{
						const TokenKind token_kind = tree.Tokens()[t].kind;
						if (token_kind == TokenKind::Whitespace ||
							token_kind == TokenKind::LineComment ||
							token_kind == TokenKind::BlockComment)
						{
							continue;
						}

						if (tree.Text(tree.Tokens()[t]) == "typedef")
						{
							return CompletionKind::Type;
						}

						++seen;
					}
				}

				current = tree.NodesSoA().Parent(current);
			}

			return CompletionKind::Variable;
		}

		struct DetailParameter
		{
			std::string_view type; // as written, without the name or default
			std::string_view name; // empty when unnamed
			std::string_view text; // the whole parameter, default included
		};

		// Parameters of a function detail (`int add(int left, int right = 0)`),
		// split at top-level commas. Empty for `()`.
		std::vector<DetailParameter> DetailParameters(
			std::string_view detail,
			std::string_view label)
		{
			std::vector<DetailParameter> parameters;
			std::size_t open = std::string_view::npos;
			for (std::size_t at = detail.find(label); at != std::string_view::npos;
				at = detail.find(label, at + 1))
			{
				const std::size_t after = at + label.size();
				if ((at == 0 ||!IsIdentChar(detail[at - 1])) && after < detail.size() &&
					detail[after] == '(')
				{
					open = after;
					break;
				}
			}

			if (open == std::string_view::npos)
			{
				open = detail.find('(');
			}

			if (open == std::string_view::npos)
			{
				return parameters;
			}

			const auto trim = [](std::string_view text)
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
			};
			const auto add = [&](std::string_view parameter)
			{
				const std::string_view whole = trim(parameter);
				int depth                    = 0;
				std::size_t equals           = parameter.size();
				for (std::size_t i = 0; i < parameter.size(); ++i)
				{
					const char c = parameter[i];
					depth += c == '(' || c == '<' || c == '[' || c == '{' ? 1
					: c == ')' || c == '>' || c == ']' || c == '}' ? -1
					: 0;
					if (c == '=' && depth == 0)
					{
						equals = i;
						break;
					}
				}

				parameter = trim(parameter.substr(0, equals));
				if (parameter.empty() || parameter == "void")
				{
					return;
				}

				std::size_t word = parameter.size();
				while (word > 0 && IsIdentChar(parameter[word - 1]))
				{
					--word;
				}

				const std::string_view last = parameter.substr(word);
				const std::string_view rest = trim(parameter.substr(0, word));
				const bool named = !last.empty() && !rest.empty() && rest != "this" &&
					!IsKeyword(last) && !IsBuiltinType(last) && rest.back() != ':';
				DetailParameter entry;
				entry.type = named ? rest : parameter;
				entry.name = named ? last : std::string_view {};
				entry.text = whole;
				parameters.push_back(entry);
			};
			int depth         = 0;
			std::size_t start = open + 1;
			for (std::size_t i = open + 1; i < detail.size(); ++i)
			{
				const char c = detail[i];
				if (c == '(' || c == '<' || c == '[' || c == '{')
				{
					++depth;
				}
				else if (c == ')' && depth == 0)
				{
					add(detail.substr(start, i - start));
					break;
				}
				else if (c == ')' || c == '>' || c == ']' || c == '}')
				{
					--depth;
				}
				else if (c == ',' && depth == 0)
				{
					add(detail.substr(start, i - start));
					start = i + 1;
				}
			}

			return parameters;
		}

		// Parameters of a function detail that carry a name (`int day` but not `int`),
		// so a redeclaration with names describes the function better.
		std::size_t NamedParameterCount(std::string_view detail)
		{
			std::size_t named = 0;
			for (const auto& parameter : DetailParameters(detail, {}))
			{
				named += parameter.name.empty() ? 0 : 1;
			}

			return named;
		}

		// `(int,constchar*)c`: parameter types without names or defaults, plus the
		// const qualifier, identifying an overload.
		std::string ParameterTypesKey(const CompletionItem& function)
		{
			std::string key = "(";
			for (const auto& parameter : DetailParameters(function.detail, function.label))
			{
				for (const char c : parameter.type)
				{
					if (c != ' ' && c != '\t')
					{
						key += c;
					}
				}

				key += ',';
			}

			key += function.is_const ? ")c" : ")";
			return key;
		}

		// InsertItem for FlatHashMap with StringInterner
		void InsertItem(
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best,
			CompletionItem item)
		{
			if (item.label.empty() ||!IsIdentStart(item.label.front()))
			{
				return;
			}

			const std::uint32_t key = interner.GetOrAssign(item.label);
			auto[entry, inserted]   = best.InsertOrAssign(key, std::move(item));
			if (inserted)
			{
				return;
			}

			CompletionItem& old = entry->value;
			item.deprecated     = item.deprecated || old.deprecated;
			old.deprecated      = item.deprecated;
			if (KindPriority(item.kind) > KindPriority(old.kind) ||
				(KindPriority(item.kind) == KindPriority(old.kind) && item.is_definition &&
				!old.is_definition &&
				(item.kind == CompletionKind::Type || item.kind == CompletionKind::Namespace)))
			{
				// The same symbol seen as a record scope (with its doc comment) and
				// as a bare tag/forward declaration (without one): keep the comment.
				if (item.documentation.empty())
				{
					item.documentation = std::move(old.documentation);
				}

				if (item.type_text.empty())
				{
					item.type_text = std::move(old.type_text);
				}

				old = std::move(item);
				return;
			}

			if (item.kind == old.kind)
			{
				// Same symbol seen twice (reopened namespace, declaration + definition):
				// fill in details the first sighting lacked.
				if (old.documentation.empty() && !item.documentation.empty())
				{
					old.documentation = std::move(item.documentation);
				}

				if (old.type_text.empty() && !item.type_text.empty())
				{
					old.type_text = std::move(item.type_text);
				}

				if (old.detail == KindDetail(old.kind) && item.detail != KindDetail(item.kind))
				{
					old.detail = std::move(item.detail);
				}
				else if (old.kind == CompletionKind::Function &&
					old.param_count == item.param_count &&
					NamedParameterCount(item.detail) >= NamedParameterCount(old.detail) &&
					NamedParameterCount(item.detail) > 0)
				{
					// Redeclarations: the one naming its parameters reads best.
					old.detail = std::move(item.detail);
				}
			}
			else if (old.documentation.empty() && !item.documentation.empty())
			{
				// Lower-priority sighting (record scope) that carries the doc comment.
				old.documentation = std::move(item.documentation);
			}
		}

		// Legacy InsertItem for string-based maps (used in some preprocessor paths)
		void InsertItem(
			std::unordered_map<std::string, CompletionItem>& best,
			CompletionItem item)
		{
			if (item.label.empty() ||!IsIdentStart(item.label.front()))
			{
				return;
			}

			const auto found = best.find(item.label);
			if (found == best.end())
			{
				best.emplace(item.label, std::move(item));
				return;
			}

			CompletionItem& old = found->second;
			if (KindPriority(item.kind) > KindPriority(old.kind) ||
				(KindPriority(item.kind) == KindPriority(old.kind) && item.is_definition &&
				!old.is_definition &&
				(item.kind == CompletionKind::Type || item.kind == CompletionKind::Namespace)))
			{
				if (item.documentation.empty())
				{
					item.documentation = std::move(old.documentation);
				}

				if (item.type_text.empty())
				{
					item.type_text = std::move(old.type_text);
				}

				old = std::move(item);
				return;
			}

			if (item.kind == old.kind)
			{
				if (old.documentation.empty() && !item.documentation.empty())
				{
					old.documentation = std::move(item.documentation);
				}

				if (old.type_text.empty() && !item.type_text.empty())
				{
					old.type_text = std::move(item.type_text);
				}

				if (old.detail == KindDetail(old.kind) && item.detail != KindDetail(item.kind))
				{
					old.detail = std::move(item.detail);
				}
			}
			else if (old.documentation.empty() && !item.documentation.empty())
			{
				old.documentation = std::move(item.documentation);
			}
		}

		// Fields complete and hover with their declared type, not just the
		// specifier: `Node* next;` reads `Node *`, not `Node`. The index stores
		// the specifier spelling, so member listings upgrade the copy.
		CompletionItem WithFieldDetail(CompletionItem item)
		{
			if (item.kind == CompletionKind::Variable && item.detail != item.layout_type &&
				!item.layout_type.empty() && item.layout_type != "?")
			{
				item.detail = item.layout_type;
			}

			return item;
		}

		// --- Scope-aware name resolution -------------------------------------------
		// Unqualified lookup walks the scope chain (block, function, namespace,
		// global) honoring the point of declaration; qualified lookup (`ns::name`)
		// resolves the qualifier to a scope node and lists its direct members.

		constexpr std::size_t NoIndex = static_cast<std::size_t>(-1);

		bool IsScopeKind(GrammarKind kind)
		{
			switch (kind)
			{
			case GrammarKind::TranslationUnit:
			case GrammarKind::NamespaceDefinition:
			case GrammarKind::RecordDefinition:
			case GrammarKind::FunctionDefinition:
			case GrammarKind::CompoundStatement:
			case GrammarKind::LambdaExpression:
				return true;
			default:
				return false;
			}
		}

		std::pair<std::size_t, std::size_t> NodeRange(const ParseTree& tree, std::size_t node)
		{
			if (node >= tree.NodesSoA().size())
				return {0, 0};
			const auto grammar = tree.NodesSoA()[node];
			if (grammar.GetTokenCount() == 0 || grammar.GetFirstToken() >= tree.Tokens().size())
				return {0, 0};
			const std::size_t last =
				grammar.GetFirstToken() + grammar.GetTokenCount() <= tree.Tokens().size()
			? grammar.GetFirstToken() + grammar.GetTokenCount() - 1
			: tree.Tokens().size() - 1;
			const Token& first      = tree.Tokens()[grammar.GetFirstToken()];
			const Token& last_token = tree.Tokens()[last];
			return {first.offset, last_token.offset + last_token.length};
		}

		bool IsLocalBoundary(GrammarKind kind)
		{
			switch (kind)
			{
			case GrammarKind::ParameterDeclaration:
			case GrammarKind::CompoundStatement:
			case GrammarKind::DeclarationStatement:
			case GrammarKind::ExpressionStatement:
			case GrammarKind::ReturnStatement:
			case GrammarKind::IfStatement:
			case GrammarKind::LoopStatement:
			case GrammarKind::SwitchStatement:
			case GrammarKind::CaseLabel:
			case GrammarKind::TryStatement:
			case GrammarKind::DoStatement:
			case GrammarKind::LambdaExpression:
				return true;
			default:
				return false;
			}
		}

		// Block-likes owning a local's visibility: the innermost compound (or the
		// loop/statement for initializers), not the declaration statement itself —
		// `long total = 0;` is usable for the rest of the block, not just its line.
		bool IsBlockBoundary(GrammarKind kind)
		{
			switch (kind)
			{
			case GrammarKind::CompoundStatement:
			case GrammarKind::LoopStatement:
			case GrammarKind::SwitchStatement:
			case GrammarKind::IfStatement:
			case GrammarKind::TryStatement:
			case GrammarKind::DoStatement:
			case GrammarKind::LambdaExpression:
				return true;
			default:
				return false;
			}
		}

		struct LocalInfo
		{
			bool is_local = false;
			// Enclosing function/lambda for parameters and body locals; NoIndex when
			// the name belongs to an outer scope (or to no function at all, such as
			// prototype parameters, which are never usable).
			std::size_t owner = NoIndex;
			// Innermost block owning a body local; the function itself for parameters.
			std::size_t boundary = NoIndex;
		};

		// Splits DeclaredName nodes into function parameters/body locals versus names
		// owned by an outer scope (the function's own name, globals, namespace and
		// record members). Lambda bodies count as function boundaries so their
		// parameters are modeled as locals of the lambda.
		LocalInfo AnalyzeLocal(const ParseTree& tree, std::size_t node)
		{
			LocalInfo info;
			if (node >= tree.NodesSoA().size())
			{
				return info;
			}

			std::size_t current = tree.NodesSoA().Parent(node);
			for (std::size_t depth = 0;
				depth < kMaxScopeWalkDepth && current < tree.NodesSoA().size(); ++depth)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(current);
				if (kind == GrammarKind::FunctionDefinition ||
					kind == GrammarKind::LambdaExpression || kind == GrammarKind::ConstevalBlock)
				{
					if (!info.is_local)
					{
						return info;
					} // the function's own name

					info.owner = current;
					if (info.boundary == NoIndex)
					{
						info.boundary = current;
					} // parameters: whole body

					return info;
				}

				if (IsLocalBoundary(kind))
				{
					info.is_local = true;
					// The owning block (`for` initializers own their loop, `if`
					// initializers their statement) — not the declaration line itself,
					// which would hide later uses in the same block.
					if (IsBlockBoundary(kind) && info.boundary == NoIndex)
					{
						info.boundary = current;
					}
				}

				if (kind == GrammarKind::TranslationUnit ||
					kind == GrammarKind::NamespaceDefinition ||
					kind == GrammarKind::RecordDefinition)
				{
					return info;
				}

				current = tree.NodesSoA().Parent(current);
			}

			return info;
		}

		bool IsTransparentForMembership(GrammarKind kind)
		{
			// Declarator plumbing between a name and the scope that owns it: the name
			// of a function defined in a namespace belongs to the namespace, while a
			// parameter or body local stops at ParameterDeclaration/CompoundStatement.
			switch (kind)
			{
			case GrammarKind::Declarator:
			case GrammarKind::InitDeclarator:
			case GrammarKind::Declaration:
			case GrammarKind::TypeSpecifier:
			case GrammarKind::FunctionSuffix:
			case GrammarKind::PointerOperator:
			case GrammarKind::NestedNameSpecifier:
			case GrammarKind::ArraySuffix:
			case GrammarKind::TrailingReturnType:
			case GrammarKind::NoexceptSpecifier:
			case GrammarKind::AttributeSpecifier:
			case GrammarKind::BitfieldSuffix:
			case GrammarKind::TemplateDeclaration:
			case GrammarKind::TemplateArgument:
			case GrammarKind::FunctionDefinition:
			case GrammarKind::FunctionDeclaration:
			case GrammarKind::Enumerator:
			case GrammarKind::RequiresClause:
			case GrammarKind::DeclaredName:
				return true;
			default:
				return false;
			}
		}

		// Scope node a name belongs to for qualified lookup: the namespace/record
		// that directly owns it (a function defined in a namespace belongs to the
		// namespace; its locals belong to body blocks instead).
		std::size_t MemberScope(const ParseTree& tree, std::size_t node)
		{
			if (node >= tree.NodesSoA().size())
			{
				return NoIndex;
			}

			std::size_t current = tree.NodesSoA().Parent(node);
			for (std::size_t depth = 0;
				depth < kMaxScopeWalkDepth && current < tree.NodesSoA().size(); ++depth)
			{
				if (IsTransparentForMembership(tree.NodesSoA().Kind(current)))
				{
					current = tree.NodesSoA().Parent(current);
					continue;
				}

				return current;
			}

			return NoIndex;
		}

		// Name elements introducing a namespace/record scope node. Compound
		// definitions (`namespace a::b`) contribute several elements; anonymous scopes
		// contribute one empty element so they can never match a typed qualifier.
		std::vector<std::string> ScopeNameElements(const ParseTree& tree, std::size_t node)
		{
			const auto& nodes = tree.NodesSoA();
			if (node >= nodes.size())
				return {{}};
			const GrammarKind kind = nodes.Kind(node);
			if (kind != GrammarKind::NamespaceDefinition && kind != GrammarKind::RecordDefinition)
			{
				return {{}};
			}

			const auto grammar = nodes[node];
			std::vector<std::size_t> significant;
			const std::size_t end =
				std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
				tree.Tokens().size());
			for (std::size_t i = grammar.GetFirstToken(); i < end; ++i)
			{
				const TokenKind token_kind = tree.Tokens()[i].kind;
				if (token_kind == TokenKind::Whitespace || token_kind == TokenKind::LineComment ||
					token_kind == TokenKind::BlockComment || tree.IsDecorationToken(i))
				{
					continue;
				}

				significant.push_back(i);
			}

			auto text = [&](std::size_t token)->std::string_view
			{
				return tree.Text(tree.Tokens()[token]);
			};
			auto is_identifier = [&](std::size_t token)
			{
				return tree.Tokens()[token].kind == TokenKind::Identifier;
			};
			std::size_t pos = 0;
			if (kind == GrammarKind::NamespaceDefinition)
			{
				while (pos < significant.size() &&
					(text(significant[pos]) == "export" || text(significant[pos]) == "inline"))
				{
					++pos;
				}

				if (pos >= significant.size() || text(significant[pos]) != "namespace")
					return {{}};
				++pos;
				// Anonymous (`{`), alias (`name = ...`), or a dotted name.
				std::vector<std::string> names;
				while (pos < significant.size() && is_identifier(significant[pos]))
				{
					const auto spelling = text(significant[pos]);
					const auto replacement = tree.ObjectReplacement(spelling,
						tree.Tokens()[significant[pos]].offset);
					if (!replacement.empty() && std::ranges::all_of(replacement, IsIdentChar))
					{
						names.emplace_back(replacement);
					}
					else
					{
						names.emplace_back(spelling);
					}

					++pos;
					if (pos + 1 < significant.size() && text(significant[pos]) == "::" &&
						is_identifier(significant[pos + 1]))
					{
						++pos;
						continue;
					}

					break;
				}

				return names.empty() ? std::vector<std::string> {{}}: names;
			}

			for (; pos < significant.size(); ++pos)
			{
				const std::string_view word = text(significant[pos]);
				if (word != "struct" && word != "class" && word != "union" && word != "enum")
				{
					continue;
				}

				++pos;
				if (word == "enum" && pos < significant.size() &&
					(text(significant[pos]) == "class" || text(significant[pos]) == "struct"))
				{
					++pos;
				}

				// `class [[nodiscard]] name`, `struct alignas(8) name`, `__attribute__((...))`.
				const auto count_of = [&](std::string_view piece, char bracket)
				{
					return !piece.empty() &&
						piece.find_first_not_of(bracket) == std::string_view::npos
					? static_cast<int>(piece.size())
					: 0;
				};
				while (pos < significant.size())
				{
					const std::string_view lead = text(significant[pos]);
					const bool call =
						(lead == "alignas" || lead == "__attribute__" || lead == "__declspec") &&
						pos + 1 < significant.size() && text(significant[pos + 1]) == "(";
					if (!call && count_of(lead, '[') == 0)
					{
						break;
					}

					int depth = 0;
					for (; pos < significant.size(); ++pos)
					{
						const std::string_view piece = text(significant[pos]);
						depth += call ? (piece == "(" ? 1
							: piece == ")" ? -1
							: 0)
						: count_of(piece, '[') - count_of(piece, ']');
						if (depth <= 0 && (call ? piece == ")" : count_of(piece, ']') > 0))
						{
							++pos;
							break;
						}
					}
				}

				if (pos < significant.size() && is_identifier(significant[pos]))
				{
					std::vector<std::string> names {std::string(text(significant[pos]))};
					while (pos + 2 < significant.size() && text(significant[pos + 1]) == "::" &&
						is_identifier(significant[pos + 2]))
					{
						pos += 2;
						names.emplace_back(text(significant[pos]));
					}

					return names;
				}

				return {{}};
			}

			return {{}};
		}

		// Whether a namespace definition is `inline namespace` (members visible as
		// direct members of the enclosing scope).
		bool IsInlineNamespace(const ParseTree& tree, std::size_t node)
		{
			const auto& nodes = tree.NodesSoA();
			if (node >= nodes.size() || nodes.Kind(node) != GrammarKind::NamespaceDefinition)
			{
				return false;
			}

			const auto grammar = nodes[node];
			const std::size_t end =
				std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
				tree.Tokens().size());
			bool seen_inline = false;
			for (std::size_t i = grammar.GetFirstToken(); i < end; ++i)
			{
				const Token& token = tree.Tokens()[i];
				if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
					token.kind == TokenKind::BlockComment)
				{
					continue;
				}

				const std::string_view word = tree.Text(token);
				if (word == "inline")
				{
					seen_inline = true;
					continue;
				}

				if (word == "export")
				{
					continue;
				}

				return seen_inline && word == "namespace";
			}

			return false;
		}

		// Qualified path of a scope node from the translation unit down, e.g.
		// `outer::inner`. Function-like levels cannot be named from the outside and
		// are skipped, so a function-local struct still resolves by its tag. Inline
		// namespaces are transparent (their members are visible as direct members of
		// the enclosing scope), which is what makes e.g. libc++ `std::__1::vector`
		// resolve as `std::vector`.
		std::vector<std::string> ScopePath(const ParseTree& tree, std::size_t node)
		{
			std::vector<std::string> path;
			std::size_t current = node;
			for (std::size_t depth = 0;
				depth < kMaxScopeWalkDepth && current < tree.NodesSoA().size(); ++depth)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(current);
				if (kind == GrammarKind::TranslationUnit)
				{
					break;
				}

				if (kind == GrammarKind::NamespaceDefinition ||
					kind == GrammarKind::RecordDefinition)
				{
					if (!IsInlineNamespace(tree, current))
					{
						const auto elements = ScopeNameElements(tree, current);
						path.insert(path.begin(), elements.begin(), elements.end());
					}
				}

				current = tree.NodesSoA().Parent(current);
			}

			return path;
		}

		struct Qualifier
		{
			std::vector<std::string> path;
			bool global     = false; // leading `::` with no names (`::name`)
			bool unresolved = false; // modeled as nothing (`A<int>::x`, `call()::y`)
		};

		// `namespace alias = a::b::c;` has no grammar node of its own: it is
		// found by a token scan. Aliases are visible after their definition;
		// nesting (an alias declared inside a namespace) is not modeled, so
		// same-named aliases keep the last definition.
		struct NamespaceAlias
		{
			std::string name;
			std::vector<std::string> target;
			std::size_t offset = 0; // byte offset of the alias name
		};

		std::size_t NextSignificantToken(const std::vector<Token>& tokens, std::size_t token);

		std::vector<NamespaceAlias> CollectNamespaceAliases(const ParseTree& tree)
		{
			std::vector<NamespaceAlias> out;
			const std::string_view source    = tree.Source();
			const std::vector<Token>& tokens = tree.Tokens();
			for (std::size_t t = 0; t < tokens.size(); ++t)
			{
				if (tokens[t].kind != TokenKind::Identifier ||
					tree.Text(tokens[t]) != "namespace")
				{
					continue;
				}

				const std::size_t name = NextSignificantToken(tokens, t);
				if (name >= tokens.size() || tokens[name].kind != TokenKind::Identifier ||
					tree.Text(tokens[name]) == "namespace")
				{
					continue;
				}

				const std::size_t equals = NextSignificantToken(tokens, name);
				if (equals >= tokens.size() || tokens[equals].kind != TokenKind::Punctuation ||
					TokenText(source, tokens[equals]) != "=")
				{
					continue; // a definition (`{`) or nested name (`::`), not an alias
				}

				NamespaceAlias alias;
				alias.name          = std::string(tree.Text(tokens[name]));
				alias.offset        = tokens[name].offset;
				std::size_t current = equals;
				while (true)
				{
					const std::size_t part = NextSignificantToken(tokens, current);
					if (part >= tokens.size() || tokens[part].kind != TokenKind::Identifier)
					{
						break;
					}

					alias.target.emplace_back(tree.Text(tokens[part]));
					const std::size_t separator = NextSignificantToken(tokens, part);
					if (separator < tokens.size() &&
						tokens[separator].kind == TokenKind::Punctuation &&
						TokenText(source, tokens[separator]) == "::")
					{
						current = separator;
						continue;
					}

					current = part;
					break;
				}

				if (alias.target.empty())
				{
					continue;
				}

				const std::size_t end = NextSignificantToken(tokens, current);
				if (end >= tokens.size() || tokens[end].kind != TokenKind::Punctuation ||
					TokenText(source, tokens[end]) != ";")
				{
					continue;
				}

				std::erase_if(out,[&](const NamespaceAlias& existing)
					{
						return existing.name == alias.name;
					});
				out.push_back(std::move(alias));
				t = end;
			}

			return out;
		}

		// Rewrites a qualifier spelled through a namespace alias
		// (`hana::x` -> `library::hana::x`). Up to one alias chain is followed;
		// unknown aliases leave the path untouched.
		std::vector<std::string> ExpandNamespaceAlias(
			const std::vector<NamespaceAlias>& aliases,
			const std::vector<std::string>& path,
			std::size_t offset)
		{
			if (path.empty())
			{
				return path;
			}

			std::vector<std::string> expanded = path;
			for (int depth = 0; depth < 4; ++depth)
			{
				bool rewritten = false;
				for (const auto& alias : aliases)
				{
					if (alias.name == expanded.front() && alias.offset <= offset &&
						!alias.target.empty())
					{
						expanded.erase(expanded.begin());
						expanded.insert(expanded.begin(), alias.target.begin(),
							alias.target.end());
						rewritten = true;
						break;
					}
				}

				if (!rewritten)
				{
					break;
				}
			}

			return expanded;
		}

		// Names in `a::b::` before the final `::` at scope_op.
		Qualifier QualifierBefore(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t scope_op)
		{
			Qualifier result;
			std::size_t current = scope_op;
			while (true)
			{
				const std::size_t name = PreviousSignificant(tokens, current, source);
				if (name >= tokens.size() || tokens[name].kind != TokenKind::Identifier)
				{
					break;
				}

				result.path.insert(result.path.begin(),
					std::string(TokenText(source, tokens[name])));
				const std::size_t separator = PreviousSignificant(tokens, name, source);
				if (separator >= tokens.size() ||
					tokens[separator].kind != TokenKind::Punctuation ||
					TokenText(source, tokens[separator]) != "::")
				{
					break;
				}

				current = separator;
			}

			if (!result.path.empty())
			{
				return result;
			}

			// No identifiers: either a global qualifier (`::name`) or something this
			// engine cannot model. A closing bracket hints at the latter
			// (`A<int>::x`); anything else is the global scope.
			const std::size_t prev = PreviousSignificant(tokens, scope_op, source);
			if (prev < tokens.size() && tokens[prev].kind == TokenKind::Punctuation)
			{
				const std::string_view text = TokenText(source, tokens[prev]);
				if (text == ">" || text == ">>" || text == ")" || text == "]")
				{
					result.unresolved = true;
				}
				else
				{
					result.global = true;
				}
			}
			else
			{
				result.global = true;
			}

			return result;
		}

		// Scope nodes whose qualified path equals the qualifier. Namespaces reopened
		// in several blocks naturally yield several targets whose members unite.
		std::vector<std::size_t> ResolveScope(
			const std::vector<std::vector<std::string>>& scope_paths,
			const std::vector<std::string>& path)
		{
			std::vector<std::size_t> targets;
			for (std::size_t n = 0; n < scope_paths.size(); ++n)
			{
				if (scope_paths[n].empty() && path.empty())
				{
					// The translation unit itself has no entry in scope_paths (only
					// namespace/record nodes are cached); the root is handled by
					// callers via MemberScope() == RootNode checks, so skip empties.
					continue;
				}

				if (scope_paths[n] == path)
				{
					targets.push_back(n);
				}
			}

			return targets;
		}

		// Qualified path of every namespace/record node, indexed by node. Built once
		// per completion request: ResolveScope, CollectChildScopes and the namespace
		// loops previously recomputed ScopePath per node on each pass.
		std::vector<std::vector<std::string>> BuildScopePaths(const ParseTree& tree)
		{
			std::vector<std::vector<std::string>> paths(tree.NodesSoA().size());
			for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(n);
				if (kind != GrammarKind::NamespaceDefinition &&
					kind != GrammarKind::RecordDefinition)
				{
					continue;
				}

				paths[n] = ScopePath(tree, n);
			}

			return paths;
		}

		// Contiguous function/lambda ranges, sorted by start offset. Built once per
		// completion request so occurrence visibility is a binary search per token
		// instead of the previous O(TxN) InnermostCallable scan per identifier.
		struct CallableInterval
		{
			std::size_t start = 0;
			std::size_t end   = 0;
			std::size_t node  = NoIndex;
		};

		std::vector<CallableInterval> BuildCallableIntervals(const ParseTree& tree)
		{
			std::vector<CallableInterval> intervals;
			constexpr std::size_t kReserveDivisor = 8;
			intervals.reserve(tree.NodesSoA().size() / kReserveDivisor);
			for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(n);
				if (kind != GrammarKind::FunctionDefinition &&
					kind != GrammarKind::LambdaExpression && kind != GrammarKind::ConstevalBlock)
				{
					continue;
				}

				const auto[start, end] = NodeRange(tree, n);
				if (end <= start && start != 0)
				{
					continue;
				}

				if (start == 0 && end == 0)
				{
					continue;
				}

				intervals.push_back({start, end, n});
			}

			std::sort(intervals.begin(), intervals.end(),
				[](const CallableInterval& left, const CallableInterval& right)
				{
					if (left.start != right.start)
					{
						return left.start < right.start;
					}

					return left.end > right.end;
				});
			return intervals;
		}

		// Innermost function/lambda range containing pos (inclusive end, matching the
		// old linear scan). Intervals nest, so walking back from the last start<=pos
		// finds the innermost container with the first end>=pos hit.
		std::size_t FindCallable(const std::vector<CallableInterval>& intervals, std::size_t pos)
		{
			std::size_t lo = 0;
			std::size_t hi = intervals.size();
			while (lo < hi)
			{
				constexpr std::size_t kBinaryHalf = 2;
				const std::size_t mid             = lo +(hi - lo) / kBinaryHalf;
				if (intervals[mid].start <= pos)
				{
					lo = mid + 1;
				}
				else
				{
					hi = mid;
				}
			}

			for (std::size_t k = lo; k > 0; --k)
			{
				const auto& interval = intervals[k - 1];
				if (pos <= interval.end)
				{
					return interval.node;
				}
			}

			return NoIndex;
		}

		// Named namespace/record ranges for the `!InNamedScope` global-qualifier
		// filter, built once per request instead of one O(N) scan per token.
		struct ScopeInterval
		{
			std::size_t start = 0;
			std::size_t end   = 0;
		};

		std::vector<ScopeInterval> BuildNamedScopeIntervals(const ParseTree& tree)
		{
			std::vector<ScopeInterval> intervals;
			for (std::size_t n = 1; n < tree.NodesSoA().size(); ++n)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(n);
				if (kind != GrammarKind::NamespaceDefinition &&
					kind != GrammarKind::RecordDefinition)
				{
					continue;
				}

				const auto[start, end] = NodeRange(tree, n);
				if (end <= start)
				{
					continue;
				}

				intervals.push_back({start, end});
			}

			return intervals;
		}

		bool InNamedScopeIntervals(
			const std::vector<ScopeInterval>& intervals,
			std::size_t offset)
		{
			for (const auto& interval : intervals)
			{
				if (interval.start <= offset && offset < interval.end)
				{
					return true;
				}
			}

			return false;
		}

		// Only spaces and tabs precede `offset` on its line.
		bool StartsLine(std::string_view source, std::size_t offset)
		{
			for (std::size_t i = offset; i > 0; --i)
			{
				const char c = source[i - 1];
				if (c == '\n')
				{
					return true;
				}

				if (c != ' ' && c != '\t')
				{
					return false;
				}
			}

			return true;
		}

		// End of the directive starting at `hash`, following `\` line continuations.
		std::size_t DirectiveLineEnd(std::string_view source, std::size_t hash)
		{
			std::size_t end = hash;
			while (end < source.size())
			{
				const std::size_t newline = source.find('\n', end);
				if (newline == std::string_view::npos)
				{
					return source.size();
				}

				std::size_t last = newline;
				while (last > end && (source[last - 1] == '\r' || source[last - 1] == ' '))
				{
					--last;
				}

				if (last == end || source[last - 1] != '\\')
				{
					return newline;
				}

				end = newline + 1;
			}

			return end;
		}

		// Merged offset ranges of class/struct/union and scoped-enum bodies whose
		// members are not visible unqualified at the cursor. Unscoped enum bodies
		// never hide their enumerators; a body containing the cursor hides nothing,
		// unless the cursor sits in an explicit-object member function, which has no
		// implicit `this`.
		class HiddenBodies
		{
		public:
			HiddenBodies(const ParseTree& tree, std::size_t offset, bool hide_enclosing_record)
			{
				std::vector<ScopeInterval> hidden;
				std::size_t innermost_start = NoIndex;
				std::size_t innermost_end   = NoIndex;
				for (std::size_t n = 1; n < tree.NodesSoA().size(); ++n)
				{
					if (tree.NodesSoA().Kind(n) != GrammarKind::RecordDefinition ||
						IsUnscopedEnum(tree, n))
					{
						continue;
					}

					const auto[start, end] = RecordBodyRange(tree, n);
					if (end <= start)
					{
						continue;
					}

					if (start <= offset && offset < end)
					{
						if (innermost_start == NoIndex || start >= innermost_start)
						{
							innermost_start = start;
							innermost_end   = end;
						}

						continue;
					}

					hidden.push_back({start, end});
				}

				if (hide_enclosing_record && innermost_start != NoIndex)
				{
					hidden.push_back({innermost_start, innermost_end});
				}

				std::sort(hidden.begin(), hidden.end(),
					[](const ScopeInterval& left, const ScopeInterval& right)
					{
						return left.start < right.start;
					});
				for (const auto& interval : hidden)
				{
					if (!mMerged.empty() && interval.start <= mMerged.back().end)
					{
						mMerged.back().end = std::max(mMerged.back().end, interval.end);
						continue;
					}

					mMerged.push_back(interval);
				}
			}

			[[nodiscard]] bool Contains(std::size_t offset) const
			{
				const auto after = std::upper_bound(mMerged.begin(), mMerged.end(), offset,
					[](std::size_t value, const ScopeInterval& interval)
					{
						return value < interval.start;
					});
				return after != mMerged.begin() && offset < std::prev(after) -> end;
			}

			// `enum E { A }` (not `enum class`): enumerators belong to the enclosing scope.
			static bool IsUnscopedEnum(const ParseTree& tree, std::size_t node)
			{
				const auto grammar = tree.NodesSoA()[node];
				const std::size_t end =
					std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
					tree.Tokens().size());
				bool after_enum = false;
				for (std::size_t t = grammar.GetFirstToken(); t < end; ++t)
				{
					const Token& token = tree.Tokens()[t];
					if (IsTriviaKind(token.kind) || tree.IsDecorationToken(t))
					{
						continue;
					}

					const std::string_view word = tree.Text(token);
					if (after_enum)
					{
						return word != "class" && word != "struct";
					}

					if (word == "enum")
					{
						after_enum = true;
					}
					else if (word == "class" || word == "struct" || word == "union" || word == "{")
					{
						return false;
					}
				}

				return false;
			}

			// From the opening brace of the record body to its end.
			static std::pair<std::size_t, std::size_t> RecordBodyRange(
				const ParseTree& tree,
				std::size_t node)
			{
				const auto grammar = tree.NodesSoA()[node];
				const std::size_t end =
					std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
					tree.Tokens().size());
				for (std::size_t t = grammar.GetFirstToken(); t < end; ++t)
				{
					const Token& token = tree.Tokens()[t];
					if (token.kind == TokenKind::Punctuation && tree.Text(token) == "{")
					{
						return {token.offset, NodeRange(tree, node).second};
					}
				}

				return {0, 0};
			}

		private:
			static bool IsTriviaKind(TokenKind kind)
			{
				return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
					kind == TokenKind::BlockComment;
			}

			std::vector<ScopeInterval> mMerged;
		};

		// The grammar does not always materialize the tag name itself as a
		// DeclaredName (e.g. `struct Widget { ... };`), so collect tag names
		// lexically as a fallback: `class/struct/union/enum [class/struct] Name`
		// plus `using Name` and `concept Name`. Mirrors SemanticAnalyzer::
		// CollectTypeNames. The range restriction serves qualified lookup, which
		// only wants tags nested directly in the resolved scope.
		struct TagName
		{
			std::string_view text;
			std::size_t offset = 0;
			// Introducing keyword (`struct`, `enum class`, `using`, `concept`) and
			// its token index, for `struct Widget`-style details and doc anchoring.
			std::string_view intro;
			std::size_t intro_token = 0;
			std::size_t name_token  = 0;
		};

		struct Define
		{
			std::string_view name;
			std::string value; // trimmed rest of the line (may hold `(args) replacement`)
			std::size_t hash_token = 0;
		};

		void ScanTagNames(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::vector<TagName>& out,
			const ParseTree* tree = nullptr)
		{
			struct Word
			{
				std::string_view text;
				std::size_t offset = 0;
				std::size_t token  = 0;
			};

			std::vector<Word> words;
			words.reserve(tokens.size());
			for (std::size_t i = 0; i < tokens.size(); ++i)
			{
				const Token& token = tokens[i];
				if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
					token.kind == TokenKind::BlockComment ||
					(tree != nullptr && tree->IsDecorationToken(i)))
				{
					continue;
				}

				words.push_back({TokenText(source, token), token.offset, i});
			}

			auto is_word = [](std::string_view value)
			{
				return !value.empty() && IsIdentStart(value.front());
			};
			for (std::size_t i = 0; i < words.size(); ++i)
			{
				auto emit = [&](std::size_t index, std::string_view intro,
					std::size_t intro_token)
				{
					if (index < words.size() && is_word(words[index].text))
					{
						out.push_back({words[index].text, words[index].offset, intro, intro_token,
								words[index].token});
					}
				};
				if (words[i].text == "using" || words[i].text == "concept")
				{
					emit(i + 1, words[i].text, words[i].token);
				}
				else if (words[i].text == "class" || words[i].text == "struct" ||
					words[i].text == "union" || words[i].text == "enum")
				{
					std::size_t name        = i + 1;
					std::string_view intro  = words[i].text;
					std::size_t intro_token = words[i].token;
					if (words[i].text == "enum" && name < words.size() &&
						(words[name].text == "class" || words[name].text == "struct"))
					{
						intro = source.substr(
							words[i].offset,
							words[name].offset + words[name].text.size() - words[i].offset);
						intro_token = words[i].token;
						++name;
					}

					// `class path::iterator` declares iterator, not path.
					while (name + 2 < words.size() && words[name + 1].text == "::" &&
						is_word(words[name + 2].text))
					{
						name += 2;
					}

					emit(name, intro, intro_token);
				}
			}
		}

		// `#define` scan with replacement text and `#` anchor for documentation.
		// Function-like macros keep their `(params)` in the value.
		void ScanDefines(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::vector<Define>& out)
		{
			constexpr std::size_t kDefineLookahead = 2;
			for (std::size_t i = 0; i + kDefineLookahead < tokens.size(); ++i)
			{
				if (tokens[i].kind != TokenKind::Punctuation)
				{
					continue;
				}

				if (TokenText(source, tokens[i]) != "#")
				{
					continue;
				}

				std::size_t j = i + 1;
				while (j < tokens.size() && tokens[j].kind == TokenKind::Whitespace)
				{
					++j;
				}

				if (j >= tokens.size() || tokens[j].kind != TokenKind::Identifier)
				{
					continue;
				}

				if (TokenText(source, tokens[j]) != "define")
				{
					continue;
				}

				++j;
				while (j < tokens.size() && tokens[j].kind == TokenKind::Whitespace)
				{
					++j;
				}

				if (j >= tokens.size() || tokens[j].kind != TokenKind::Identifier)
				{
					continue;
				}

				const Token& name_token       = tokens[j];
				const std::size_t value_start = name_token.offset + name_token.length;
				std::size_t line_end          = value_start;
				while (line_end < source.size() && source[line_end] != '\n')
				{
					++line_end;
				}

				std::string_view value = source.substr(value_start, line_end - value_start);
				while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
				{
					value.remove_prefix(1);
				}

				while (!value.empty() &&
					(value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
				{
					value.remove_suffix(1);
				}

				out.push_back({TokenText(source, name_token), std::string(value), i});
			}
		}

		// --- Symbol description (detail + documentation) ---------------------------

		// First direct child of `parent` with kind, or NoIndex. Uses the tree's
		// pre-order subtree range instead of a per-request children table.
		std::size_t FindChild(const ParseTree& tree, std::size_t parent, GrammarKind kind)
		{
			if (parent >= tree.NodesSoA().size())
			{
				return NoIndex;
			}

			const std::size_t end = tree.NodesSoA().SubtreeEnd(parent) < tree.NodesSoA().size()
			? tree.NodesSoA().SubtreeEnd(parent)
			: tree.NodesSoA().size();
			for (std::size_t i = parent + 1; i < end; ++i)
			{
				if (tree.NodesSoA().Parent(i) == parent && tree.NodesSoA().Kind(i) == kind)
				{
					return i;
				}
			}

			return NoIndex;
		}

		// First pre-order descendant of `node` with kind, or NoIndex. Subtrees occupy
		// contiguous index ranges, so a single linear scan needs no auxiliary index
		// and allocates nothing.
		std::size_t FindInSubtree(const ParseTree& tree, std::size_t node, GrammarKind kind)
		{
			if (node >= tree.NodesSoA().size())
			{
				return NoIndex;
			}

			const std::size_t end = tree.NodesSoA().SubtreeEnd(node) < tree.NodesSoA().size()
			? tree.NodesSoA().SubtreeEnd(node)
			: tree.NodesSoA().size();
			for (std::size_t i = node + 1; i < end; ++i)
			{
				if (tree.NodesSoA().Kind(i) == kind)
				{
					return i;
				}
			}

			return NoIndex;
		}

		// Exact source slice of a token range (ranges retain trivia).
		std::string SliceRange(
			const ParseTree& tree,
			std::size_t first_token,
			std::size_t token_count)
		{
			if (token_count == 0 || first_token >= tree.Tokens().size())
				return {};
			const std::size_t last =
				std::min<std::size_t>(static_cast<std::size_t>(first_token) + token_count,
				tree.Tokens().size()) -
				1;
			const Token& first      = tree.Tokens()[first_token];
			const Token& last_token = tree.Tokens()[last];
			return std::string(tree.Source().substr(
				first.offset, last_token.offset + last_token.length - first.offset));
		}

		// Single-line, capped copy for `detail` fields.
		std::string CompactWs(std::string text, std::size_t cap)
		{
			std::string out;
			out.reserve(std::min(text.size(), cap));
			bool space = true; // trim leading whitespace
			for (const char c : text)
			{
				const bool blank = c == ' ' || c == '\t' || c == '\n' || c == '\r';
				if (blank)
				{
					if (!space && out.size() < cap)
					{
						out += ' ';
					}

					space = true;
				}
				else
				{
					if (out.size() >= cap)
					{
						break;
					}

					out += c;
					space = false;
				}
			}

			while (!out.empty() && out.back() == ' ')
			{
				out.pop_back();
			}

			return out;
		}

		// Nearest Declarator ancestor of a DeclaredName, or NoIndex.
		std::size_t DeclaratorOf(const ParseTree& tree, std::size_t node)
		{
			if (node >= tree.NodesSoA().size())
			{
				return NoIndex;
			}

			std::size_t current = tree.NodesSoA().Parent(node);
			for (std::size_t depth = 0;
				depth < kMaxParentWalkDepth && current < tree.NodesSoA().size(); ++depth)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(current);
				if (kind == GrammarKind::Declarator)
				{
					return current;
				}

				if (kind == GrammarKind::FunctionDefinition ||
					kind == GrammarKind::FunctionDeclaration ||
					kind == GrammarKind::TranslationUnit ||
					kind == GrammarKind::NamespaceDefinition ||
					kind == GrammarKind::RecordDefinition || kind == GrammarKind::CompoundStatement)
				{
					return NoIndex;
				}

				current = tree.NodesSoA().Parent(current);
			}

			return NoIndex;
		}

		// `int add(int left, int right)` for a function name, or empty when the shape
		// is unexpected (callers fall back to the plain kind detail).
		// ConceptDefinition for `node` itself, its parent, or (through a
		// TemplateDeclaration wrapper) its first such child. NoIndex otherwise.
		std::size_t ConceptDefinitionOf(const ParseTree& tree, std::size_t node)
		{
			const auto& nodes = tree.NodesSoA();
			if (node < nodes.size() && nodes.Kind(node) == GrammarKind::ConceptDefinition)
			{
				return node;
			}

			const auto parent = node < nodes.size() ? nodes.Parent(node) : NoIndex;
			if (parent < nodes.size() && nodes.Kind(parent) == GrammarKind::ConceptDefinition)
			{
				return parent;
			}

			if (node < nodes.size() && nodes.Kind(node) == GrammarKind::TemplateDeclaration)
			{
				return FindChild(tree, node, GrammarKind::ConceptDefinition);
			}

			return NoIndex;
		}

		// `concept Name = constraint` in one line, capped for `detail` fields.
		std::string ConceptDefinitionText(
			const ParseTree& tree,
			std::size_t node,
			std::string_view name)
		{
			constexpr std::size_t kMaxConceptLen = 160;
			const auto conceptNode               = ConceptDefinitionOf(tree, node);
			if (conceptNode == NoIndex)
			{
				return {};
			}

			const auto ref     = tree.NodesSoA()[conceptNode];
			const auto first   = ref.GetFirstToken();
			const auto count   = ref.GetTokenCount();
			const auto& tokens = tree.Tokens();
			std::size_t equals = NoIndex;
			for (std::size_t token = first; token < first + count && token < tokens.size();
				++token)
			{
				if (tokens[token].tok == Tok::Eq)
				{
					equals = token;
					break;
				}
			}

			if (equals == NoIndex)
			{
				return {};
			}

			auto last = first + count;
			if (last > equals + 1 && last - 1 < tokens.size() && tokens[last - 1].tok == Tok::Semi)
			{
				--last;
			}

			std::string definition("concept ");
			definition += name;
			definition += " = ";
			definition += CompactWs(SliceRange(tree, equals + 1, last -(equals + 1)),
				kMaxConceptLen);
			if (definition.size() > kMaxDetailLen)
			{
				definition.resize(kMaxDetailLen);
			}

			return definition;
		}

		std::string FunctionSignature(
			const ParseTree& tree,
			std::size_t node,
			std::string_view name)
		{
			const std::size_t declarator = DeclaratorOf(tree, node);
			if (declarator == NoIndex)
				return {};
			constexpr std::size_t kMaxParamsTextLen   = 200;
			constexpr std::size_t kMaxTrailingTextLen = 64;
			constexpr std::size_t kMaxReturnTextLen   = 96;
			std::string params;
			const std::size_t suffix = FindInSubtree(tree, declarator, GrammarKind::FunctionSuffix);
			if (suffix != NoIndex)
			{
				const auto suffix_node = tree.NodesSoA()[suffix];
				params = CompactWs(
					SliceRange(tree, suffix_node.GetFirstToken(), suffix_node.GetTokenCount()),
					kMaxParamsTextLen);
			}

			std::string trailing;
			const std::size_t trailing_node =
				FindInSubtree(tree, declarator, GrammarKind::TrailingReturnType);
			if (trailing_node != NoIndex)
			{
				const auto trailing_ref = tree.NodesSoA()[trailing_node];
				trailing = CompactWs(
					SliceRange(tree, trailing_ref.GetFirstToken(), trailing_ref.GetTokenCount()),
					kMaxTrailingTextLen);
			}

			std::string returns;
			std::size_t function = tree.NodesSoA().Parent(declarator);
			for (std::size_t depth = 0;
				depth < kMaxParentWalkDepth && function < tree.NodesSoA().size(); ++depth)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(function);
				if (kind == GrammarKind::FunctionDefinition ||
					kind == GrammarKind::FunctionDeclaration)
				{
					break;
				}

				if (kind == GrammarKind::TranslationUnit ||
					kind == GrammarKind::NamespaceDefinition ||
					kind == GrammarKind::RecordDefinition || kind == GrammarKind::CompoundStatement)
				{
					function = NoIndex;
					break;
				}

				function = tree.NodesSoA().Parent(function);
			}

			if (function != NoIndex)
			{
				const std::size_t type = FindChild(tree, function, GrammarKind::TypeSpecifier);
				if (type != NoIndex)
				{
					const auto type_node = tree.NodesSoA()[type];
					returns = CompactWs(
						SliceRange(tree, type_node.GetFirstToken(), type_node.GetTokenCount()),
						kMaxReturnTextLen);
				}
			}

			std::string signature;
			if (!returns.empty())
			{
				signature += returns;
				signature += ' ';
			}

			signature += name;
			signature += params.empty() ? "()" : params;
			if (!trailing.empty())
			{
				signature += ' ';
				signature += trailing;
			}

			if (signature.size() > kMaxDetailLen)
			{
				signature.resize(kMaxDetailLen);
			}

			return signature;
		}

		// Declared type of a variable (`int`, `Widget`, ...), or empty.
		std::string VariableTypeDetail(const ParseTree& tree, std::size_t node)
		{
			std::size_t current = tree.NodesSoA().Parent(node);
			for (std::size_t depth = 0;
				depth < kMaxParentWalkDepth && current < tree.NodesSoA().size(); ++depth)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(current);
				if (kind == GrammarKind::Declarator || kind == GrammarKind::PointerOperator ||
					kind == GrammarKind::ArraySuffix || kind == GrammarKind::TypeSpecifier ||
					kind == GrammarKind::InitDeclarator)
				{
					// Declarator plumbing (shared specifiers live further up).
					current = tree.NodesSoA().Parent(current);
					continue;
				}

				if (kind == GrammarKind::ParameterDeclaration || kind == GrammarKind::Declaration ||
					kind == GrammarKind::DeclarationStatement)
				{
					const std::size_t type = FindChild(tree, current, GrammarKind::TypeSpecifier);
					if (type == NoIndex)
						return {};
					const auto type_node = tree.NodesSoA()[type];
					return CompactWs(
						SliceRange(tree, type_node.GetFirstToken(), type_node.GetTokenCount()),
						kMaxShortDetailLen);
				}

				return {};
			}

			return {};
		}

		// `Color::Red`-style detail for enumerators, or empty when the name has no
		// Enumerator ancestor.
		std::string EnumeratorDetail(
			const ParseTree& tree,
			std::size_t node,
			std::string_view name)
		{
			if (node >= tree.NodesSoA().size())
				return {};
			std::size_t current                       = tree.NodesSoA().Parent(node);
			constexpr std::size_t kMaxEnumeratorDepth = 6;
			for (std::size_t depth = 0;
				depth < kMaxEnumeratorDepth && current < tree.NodesSoA().size(); ++depth)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(current);
				if (kind == GrammarKind::Enumerator)
				{
					const std::size_t record = tree.NodesSoA().Parent(current);
					std::string out;
					if (record < tree.NodesSoA().size() &&
						tree.NodesSoA().Kind(record) == GrammarKind::RecordDefinition)
					{
						for (const auto& element : ScopePath(tree, record))
						{
							if (element.empty())
							{
								continue;
							}

							if (!out.empty())
							{
								out += "::";
							}

							out += element;
						}
					}

					if (!out.empty())
					{
						out += "::";
					}

					out += name;
					return out;
				}

				if (!IsTransparentForMembership(kind))
					return {};
				current = tree.NodesSoA().Parent(current);
			}

			return {};
		}

		bool IsDocLineComment(std::string_view text)
		{
			return text.starts_with("///") || text.starts_with("//!");
		}

		bool IsDocBlockComment(std::string_view text)
		{
			constexpr std::size_t kMinDocBlockLen = 5;
			return text.size() > kMinDocBlockLen &&
				(text.starts_with("/**") || text.starts_with("/*!"));
		}

		std::string CleanBlockComment(std::string_view text)
		{
			// Strip the opening `/**` / `/*!` and closing `*/`, then one leading `*`
			// per line (doxygen style).
			const std::size_t kBlockOpenLen    = IsDocBlockComment(text) ? 3 : 2;
			const std::size_t kBlockDelimTotal = kBlockOpenLen + 2;
			std::string_view inner = text.substr(
				kBlockOpenLen, text.size() > kBlockDelimTotal ? text.size() - kBlockDelimTotal : 0);
			std::string out;
			std::size_t pos = 0;
			while (pos <= inner.size())
			{
				std::size_t end = inner.find('\n', pos);
				if (end == std::string_view::npos)
				{
					end = inner.size();
				}

				std::string_view line = inner.substr(pos, end - pos);
				while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
				{
					line.remove_prefix(1);
				}

				if (!line.empty() && line.front() == '*')
				{
					line.remove_prefix(1);
				}

				if (!line.empty() && line.front() == ' ')
				{
					line.remove_prefix(1);
				}

				while (!line.empty() &&
					(line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
				{
					line.remove_suffix(1);
				}

				if (!out.empty())
				{
					out += '\n';
				}

				out += line;
				pos = end + 1;
			}

			while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
			{
				out.pop_back();
			}

			return out;
		}

		// True when the comment token starts its own line (nothing but whitespace
		// before it): a trailing `int a; // note` belongs to `a`, not to the
		// declaration below it.
		bool CommentStartsLine(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t index)
		{
			if (index == 0)
			{
				return true;
			}

			const Token& previous = tokens[index - 1];
			if (previous.kind != TokenKind::Whitespace)
			{
				return false;
			}

			const std::string_view text = TokenText(source, previous);
			return text.find('\n') != std::string_view::npos || index == 1;
		}

		// Documentation attached to the token at anchor_token: the contiguous
		// comments directly above it, broken by a blank line or any code. Doxygen
		// forms (`///`, `//!`, `/**`, `/*!`) always count; ordinary `//` and `/* */`
		// comments count when they sit on their own line(s) right above, which is
		// how most code documents declarations.
		std::string DocCommentFor(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t anchor_token)
		{
			if (anchor_token == 0 || anchor_token > tokens.size())
				return {};
			constexpr std::size_t kCap                  = 1000;
			constexpr int kBlankLineNewlines            = 2;
			constexpr std::size_t kLineCommentPrefixLen = 2;
			constexpr std::size_t kDocLinePrefixLen     = 3;
			constexpr std::size_t kMinBlockCommentLen   = 4;
			std::vector<std::string> lines;
			bool hasDoxygen   = false;
			std::size_t total = 0;
			for (std::size_t j = anchor_token; j > 0;)
			{
				--j;
				const Token& token          = tokens[j];
				const std::string_view text = TokenText(source, token);
				if (token.kind == TokenKind::Whitespace)
				{
					if (std::count(text.begin(), text.end(), '\n') >= kBlankLineNewlines)
					{
						break;
					} // blank line

					continue;
				}

				if (token.kind == TokenKind::LineComment && !IsDocLineComment(text))
				{
					if (!CommentStartsLine(source, tokens, j))
					{
						break;
					}

					std::string_view content = text.substr(kLineCommentPrefixLen);
					if (!content.empty() && content.front() == ' ')
					{
						content.remove_prefix(1);
					}

					while (!content.empty() && (content.back() == ' ' || content.back() == '\r'))
					{
						content.remove_suffix(1);
					}

					// `// ------`: a separator ends the comment block above the symbol.
					constexpr std::size_t kMinRulerLen = 3;
					const bool ruler = content.size() >= kMinRulerLen &&
						std::none_of(content.begin(), content.end(),[](char c)
						{
							return IsIdentChar(c) || c == ' ';
						});
					if (ruler || total + content.size() > kCap)
					{
						break;
					}

					lines.push_back(std::string(content));
					total += content.size() + 1;
					continue;
				}

				if (token.kind == TokenKind::LineComment && IsDocLineComment(text))
				{
					hasDoxygen               = true;
					std::string_view content = text.substr(kDocLinePrefixLen);
					if (!content.empty() && content.front() == ' ')
					{
						content.remove_prefix(1);
					}

					if (total + content.size() > kCap)
					{
						break;
					}

					lines.push_back(std::string(content));
					total += content.size() + 1;
					continue;
				}

				if (token.kind == TokenKind::BlockComment && text.size() > kMinBlockCommentLen &&
					(IsDocBlockComment(text) || CommentStartsLine(source, tokens, j)))
				{
					const std::string block = CleanBlockComment(text);
					hasDoxygen |= IsDocBlockComment(text);
					if (block.empty())
					{
						break;
					}

					if (total + block.size() > kCap)
					{
						break;
					}

					lines.push_back(block);
					total += block.size() + 1;
					continue;
				}

				break;
			}

			std::string out;
			for (auto it = lines.rbegin(); it != lines.rend(); ++it)
			{
				if (!out.empty())
				{
					out += '\n';
				}

				out += *it;
			}

			while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
			{
				out.pop_back();
			}

			return hasDoxygen ? RenderDoxygen(out) : out;
		}

		// First token of the outermost declaration owning a DeclaredName (through
		// `template<...>`, specifiers and declarators, stopping at scope owners), so
		// documentation sitting above the whole declaration attaches.
		std::size_t DocAnchorToken(const ParseTree& tree, std::size_t node)
		{
			if (node >= tree.NodesSoA().size())
			{
				return 0;
			}

			std::size_t current                      = node;
			constexpr std::size_t kMaxDocAnchorDepth = 16;
			for (std::size_t depth = 0; depth < kMaxDocAnchorDepth; ++depth)
			{
				const std::size_t parent = tree.NodesSoA().Parent(current);
				if (parent >= tree.NodesSoA().size() || parent == current)
				{
					break;
				}

				switch (tree.NodesSoA().Kind(parent))
				{
				case GrammarKind::Declarator:
				case GrammarKind::InitDeclarator:
				case GrammarKind::Declaration:
				case GrammarKind::ParameterDeclaration:
				case GrammarKind::FunctionDefinition:
				case GrammarKind::FunctionDeclaration:
				case GrammarKind::TemplateDeclaration:
				case GrammarKind::TypeSpecifier:
				case GrammarKind::UsingDeclaration:
				case GrammarKind::ConceptDefinition:
				case GrammarKind::Enumerator:
					current = parent;
					continue;
				default:
					break;
				}

				break;
			}

			return tree.NodesSoA().FirstToken(current);
		}

		// Return type of the function a declared name belongs to (leading type or
		// trailing `-> T`), or empty.
		std::string ReturnTypeText(const ParseTree& tree, std::size_t node)
		{
			const std::size_t declarator = DeclaratorOf(tree, node);
			if (declarator == NoIndex)
			{
				return {};
			}

			const std::size_t trailing =
				FindInSubtree(tree, declarator, GrammarKind::TrailingReturnType);
			constexpr std::size_t kArrowLen = 2;
			if (trailing != NoIndex)
			{
				std::string text = CompactWs(SliceRange(tree, tree.NodesSoA().FirstToken(trailing),
					tree.NodesSoA().TokenCount(trailing)),
					kMaxShortDetailLen);
				if (text.starts_with("->"))
				{
					text.erase(0, kArrowLen);
				}

				while (!text.empty() && text.front() == ' ')
				{
					text.erase(0, 1);
				}

				if (!text.empty())
				{
					return text;
				}
			}

			std::size_t function = tree.NodesSoA().Parent(declarator);
			for (std::size_t depth = 0;
				depth < kMaxParentWalkDepth && function < tree.NodesSoA().size(); ++depth)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(function);
				if (kind == GrammarKind::FunctionDefinition ||
					kind == GrammarKind::FunctionDeclaration)
				{
					const std::size_t type = FindChild(tree, function, GrammarKind::TypeSpecifier);
					if (type == NoIndex)
					{
						return {};
					}

					std::string result = CompactWs(SliceRange(tree, tree.NodesSoA().FirstToken(type),
						tree.NodesSoA().TokenCount(type)),
						kMaxShortDetailLen);
					constexpr std::string_view kConsteval = "consteval ";
					if (result.starts_with(kConsteval))
					{
						result.erase(0, kConsteval.size());
					}

					return result;
				}

				if (kind == GrammarKind::TranslationUnit ||
					kind == GrammarKind::NamespaceDefinition ||
					kind == GrammarKind::RecordDefinition || kind == GrammarKind::CompoundStatement)
				{
					return {};
				}

				function = tree.NodesSoA().Parent(function);
			}

			return {};
		}

		// Aliased type of `using Name = T;` / `typedef T Name;`, or empty when the
		// name is not an alias (records, `using ns::name;`).
		std::string AliasTargetText(const ParseTree& tree, std::size_t node)
		{
			std::size_t current = tree.NodesSoA().Parent(node);
			for (std::size_t depth = 0;
				depth < kMaxParentWalkDepth && current < tree.NodesSoA().size(); ++depth)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(current);
				if (kind == GrammarKind::UsingDeclaration)
				{
					const auto grammar = tree.NodesSoA()[current];
					const std::size_t last =
						std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
						tree.Tokens().size());
					for (std::size_t i = grammar.GetFirstToken(); i < last; ++i)
					{
						if (tree.Tokens()[i].kind == TokenKind::Punctuation &&
							tree.Text(tree.Tokens()[i]) == "=")
						{
							std::string text = CompactWs(
								SliceRange(tree, static_cast<std::uint32_t>(i + 1), last -(i + 1)),
								kMaxTypeTextLen);
							while (!text.empty() && (text.back() == ';' || text.back() == ' '))
							{
								text.pop_back();
							}

							return text;
						}
					}

					return {};
				}

				if (kind == GrammarKind::Declaration)
				{
					const std::size_t type = FindChild(tree, current, GrammarKind::TypeSpecifier);
					if (type == NoIndex)
					{
						return {};
					}

					return CompactWs(SliceRange(tree, tree.NodesSoA().FirstToken(type),
						tree.NodesSoA().TokenCount(type)),
						kMaxTypeTextLen);
				}

				if (kind == GrammarKind::RecordDefinition ||
					kind == GrammarKind::CompoundStatement ||
					kind == GrammarKind::TranslationUnit ||
					kind == GrammarKind::NamespaceDefinition)
				{
					return {};
				}

				current = tree.NodesSoA().Parent(current);
			}

			return {};
		}

		// `template <class T, int N = 3, typename... Ts> struct R {...}`: T, N, Ts.
		// Empty when the record is not directly wrapped in a template declaration.
		std::vector<std::string> TemplateParamNames(const ParseTree& tree, std::size_t record)
		{
			std::vector<std::string> names;
			const auto& nodes = tree.NodesSoA();
			auto wrapper      = nodes.Parent(record);
			while (wrapper < nodes.size() && wrapper != ParseTree::RootNode &&
				nodes.Kind(wrapper) != GrammarKind::TemplateDeclaration &&
				nodes.Kind(wrapper) != GrammarKind::RecordDefinition)
			{
				wrapper = nodes.Parent(wrapper);
			}

			if (wrapper >= nodes.size() || nodes.Kind(wrapper) != GrammarKind::TemplateDeclaration)
			{
				return names;
			}

			const auto& tokens = tree.Tokens();
			const std::size_t end =
				std::min<std::size_t>(nodes.FirstToken(wrapper) + nodes.TokenCount(wrapper),
				tokens.size());
			const auto text = [&](std::size_t i)
			{
				return tree.Text(tokens[i]);
			};
			std::size_t i = nodes.FirstToken(wrapper);
			while (i < end && text(i) != "<")
			{
				++i;
			}

			int depth      = 0;
			bool defaulted = false;
			std::string last;
			for (; i < end; ++i)
			{
				const TokenKind kind = tokens[i].kind;
				if (kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
					kind == TokenKind::BlockComment)
				{
					continue;
				}

				const std::string_view piece = text(i);
				if (piece == "<" || piece == "(" || piece == "[" || piece == "{")
				{
					++depth;
					continue;
				}

				const bool closing = piece == ">" || piece == ")" || piece == "]" || piece == "}";
				if (closing && --depth > 0)
				{
					continue;
				}

				if (piece == "," && depth == 1 ||(closing && depth == 0))
				{
					if (!last.empty())
					{
						names.push_back(std::move(last));
					}

					last.clear();
					defaulted = false;
					if (closing)
					{
						break;
					}

					continue;
				}

				if (piece == "=" && depth == 1)
				{
					defaulted = true;
				}
				else if (depth == 1 && !defaulted && kind == TokenKind::Identifier)
				{
					last = std::string(piece);
				}
			}

			return names;
		}

		// Base classes of a record node as written (`ns::Base`, args dropped).
		std::vector<std::string> RecordBases(const ParseTree& tree, std::size_t node)
		{
			std::vector<std::string> bases;
			const auto grammar = tree.NodesSoA()[node];
			const std::size_t last =
				std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
				tree.Tokens().size());
			std::size_t i = grammar.GetFirstToken();
			for (; i < last; ++i)
			{
				const Token& token = tree.Tokens()[i];
				if (token.kind != TokenKind::Punctuation)
				{
					continue;
				}

				const std::string_view text = tree.Text(token);
				if (text == "{" || text == ";")
				{
					return bases;
				}

				if (text == ":")
				{
					break;
				}
			}

			std::string current;
			int angle = 0;
			for (++i; i < last; ++i)
			{
				const Token& token = tree.Tokens()[i];
				if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
					token.kind == TokenKind::BlockComment)
				{
					continue;
				}

				const std::string_view text = tree.Text(token);
				if (token.kind == TokenKind::Punctuation)
				{
					if (text == "{")
					{
						break;
					}

					if (text == "<")
					{
						++angle;
					}
					else if (text == ">")
					{
						angle = std::max(0, angle - 1);
					}
					else if (text == ">>")
					{
						angle = std::max(0, angle - kDoubleAngleCount);
					}
					else if (text == "," && angle == 0)
					{
						if (!current.empty())
						{
							bases.push_back(std::move(current));
							current.clear();
						}
					}
					else if (text == "::" && angle == 0)
					{
						current += "::";
					}

					continue;
				}

				if (angle > 0 || token.kind != TokenKind::Identifier)
				{
					continue;
				}

				if (text == "public" || text == "private" || text == "protected" ||
					text == "virtual" || text == "typename")
				{
					continue;
				}

				if (!current.empty() && !current.ends_with("::"))
				{
					continue; // `final`, attributes
				}

				current += text;
			}

			if (!current.empty())
			{
				bases.push_back(std::move(current));
			}

			return bases;
		}

		// `type_text` only names the specifier; the declarator around the name
		// (`*`, `&`, `[4]`) decides what is actually stored. Rebuilds the full type
		// for layout purposes: `static ` marks storage outside the instance, `?` a
		// declarator that cannot be spelled as a type (bitfield).
		std::string DeclaredLayoutType(
			const ParseTree& tree,
			std::size_t node,
			const std::string& type_text)
		{
			const auto& tokens     = tree.Tokens();
			const std::size_t name = tree.NodesSoA().FirstToken(node);
			if (type_text.empty() || name >= tokens.size())
			{
				return {};
			}

			const auto trivia = [&](std::size_t i)
			{
				return tokens[i].kind == TokenKind::Whitespace ||
					tokens[i].kind == TokenKind::LineComment ||
					tokens[i].kind == TokenKind::BlockComment;
			};

			std::string ops;
			for (std::size_t j = name; j > 0;)
			{
				--j;
				if (trivia(j))
				{
					continue;
				}

				const std::string_view text = tree.Text(tokens[j]);
				if (text != "*" && text != "&" && text != "&&" && text != "const" &&
					text != "volatile")
				{
					break;
				}

				ops.insert(0, std::string(text) + ' ');
			}

			std::string dims;
			std::size_t k = name + 1;
			const auto skip_trivia = [&]
			{
				while (k < tokens.size() && trivia(k))
				{
					++k;
				}
			};
			skip_trivia();
			while (k < tokens.size() && tree.Text(tokens[k]) == "[")
			{
				std::string dim;
				int depth = 1;
				for (++k; k < tokens.size(); ++k)
				{
					const std::string_view text = tree.Text(tokens[k]);
					if (text == "[")
					{
						++depth;
					}
					else if (text == "]" && --depth == 0)
					{
						break;
					}

					if (!trivia(k))
					{
						dim += text;
					}
				}

				if (k >= tokens.size())
				{
					return "?";
				}

				++k;
				dims += '[' + dim + ']';
				skip_trivia();
			}

			if (k < tokens.size() && tree.Text(tokens[k]) == ":")
			{
				return "?";
			}

			bool outside_instance = false;
			std::size_t current   = tree.NodesSoA().Parent(node);
			for (std::size_t depth = 0;
				depth < kMaxParentWalkDepth && current < tree.NodesSoA().size(); ++depth)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(current);
				if (kind == GrammarKind::Declaration || kind == GrammarKind::DeclarationStatement ||
					kind == GrammarKind::ParameterDeclaration)
				{
					const std::size_t first = tree.NodesSoA().FirstToken(current);
					for (std::size_t i = first; i < name; ++i)
					{
						const std::string_view text = tree.Text(tokens[i]);
						if (text == "static" || text == "thread_local" || text == "extern" ||
							text == "typedef" || text == "friend")
						{
							outside_instance = true;
						}
					}

					break;
				}

				current = tree.NodesSoA().Parent(current);
			}

			std::string full = outside_instance ? "static " : "";
			full += type_text;
			if (!ops.empty())
			{
				ops.pop_back();
				full += ' ';
				full += ops;
			}

			return full + dims;
		}

		bool HasWord(std::string_view text, std::string_view word)
		{
			for (std::size_t at = text.find(word); at != std::string_view::npos;
				at = text.find(word, at + 1))
			{
				const bool starts = at == 0 ||!IsIdentChar(text[at - 1]);
				const bool ends = at + word.size() >= text.size() ||
					!IsIdentChar(text[at + word.size()]);
				if (starts && ends)
				{
					return true;
				}
			}

			return false;
		}

		// Qualifiers read from a function signature (`virtual void f(int) const override`).
		void ApplyFunctionQualifiers(CompletionItem& item)
		{
			const std::string_view detail = item.detail;
			std::size_t open              = std::string_view::npos;
			for (std::size_t at = detail.find(item.label); at != std::string_view::npos;
				at = detail.find(item.label, at + 1))
			{
				const std::size_t after = at + item.label.size();
				if ((at == 0 ||!IsIdentChar(detail[at - 1])) && after < detail.size() &&
					detail[after] == '(')
				{
					open = after;
					break;
				}
			}

			if (open == std::string_view::npos)
			{
				return;
			}

			int depth         = 0;
			std::size_t close = std::string_view::npos;
			for (std::size_t i = open; i < detail.size(); ++i)
			{
				if (detail[i] == '(')
				{
					++depth;
				}
				else if (detail[i] == ')' && --depth == 0)
				{
					close = i;
					break;
				}
			}

			if (close == std::string_view::npos)
			{
				return;
			}

			std::string_view suffix = detail.substr(close + 1);
			suffix                  = suffix.substr(0, suffix.find("->"));
			std::string_view params = detail.substr(open + 1, close - open - 1);
			while (!params.empty() && params.front() == ' ')
			{
				params.remove_prefix(1);
			}

			item.explicit_object = params.starts_with("this ") || params.starts_with("this\t");
			item.is_virtual = HasWord(detail.substr(0, open), "virtual") ||
				HasWord(suffix, "override") || HasWord(suffix, "final");
			// Callable on a const object: const-qualified, static, or taking the
			// explicit object by value or by const reference.
			const std::string_view self = params.substr(0, params.find(','));
			item.is_const = HasWord(suffix, "const") || HasWord(detail.substr(0, open), "static") ||
				(item.explicit_object &&
				(HasWord(self, "const") || self.find('&') == std::string_view::npos));
		}

		// `f() = delete;` after the declared name, at the declaration's top level.
		bool DeclaredDeleted(const ParseTree& tree, std::size_t node)
		{
			const auto& tokens          = tree.Tokens();
			const std::size_t name      = tree.NodesSoA().FirstToken(node);
			int depth                   = 0;
			bool after_equals           = false;
			constexpr std::size_t kScan = 64;
			for (std::size_t t = name + 1; t < tokens.size() && t < name + kScan; ++t)
			{
				if (tokens[t].kind == TokenKind::Whitespace ||
					tokens[t].kind == TokenKind::LineComment ||
					tokens[t].kind == TokenKind::BlockComment)
				{
					continue;
				}

				const std::string_view text = tree.Text(tokens[t]);
				if (after_equals)
				{
					return text == "delete";
				}

				if (text == "(" || text == "[")
				{
					++depth;
				}
				else if (text == ")" || text == "]")
				{
					--depth;
				}
				else if (depth == 0 && (text == ";" || text == "{"))
				{
					return false;
				}
				else if (depth == 0 && text == "=")
				{
					after_equals = true;
				}
			}

			return false;
		}

		// `[[deprecated]]`, `[[deprecated("...")]]` or `__attribute__((deprecated))`
		// written before the declared name, including the attribute list right
		// before the declaration.
		bool DeclaredDeprecated(const ParseTree& tree, std::size_t node)
		{
			const std::size_t name_token = tree.NodesSoA().FirstToken(node);
			std::size_t top              = node;
			std::size_t current          = tree.NodesSoA().Parent(node);
			for (std::size_t depth = 0;
				depth < kMaxScopeWalkDepth && current < tree.NodesSoA().size(); ++depth)
			{
				if (!IsTransparentForMembership(tree.NodesSoA().Kind(current)))
				{
					break;
				}

				top     = current;
				current = tree.NodesSoA().Parent(current);
			}

			const auto& tokens                          = tree.Tokens();
			std::size_t first                           = tree.NodesSoA().FirstToken(top);
			constexpr std::size_t kMaxAttributeLookback = 32;
			for (std::size_t back = 0; back < kMaxAttributeLookback && first > 0; ++back)
			{
				const Token& token = tokens[first - 1];
				const bool trivia = token.kind == TokenKind::Whitespace ||
					token.kind == TokenKind::LineComment || token.kind == TokenKind::BlockComment;
				const std::string_view text = tree.Text(token);
				if (!trivia && text != "]" && text != "]]" && text != "[" && text != "[[" &&
					text != "(" && text != ")" && text != "," &&
					token.kind != TokenKind::Identifier && token.kind != TokenKind::StringLiteral)
				{
					break;
				}

				if (!trivia && token.kind == TokenKind::Identifier && text != "deprecated" &&
					text != "nodiscard" && text != "maybe_unused" && text != "noreturn" &&
					text != "__attribute__" && text != "gnu")
				{
					break;
				}

				--first;
			}

			for (std::size_t t = first; t < name_token && t < tokens.size(); ++t)
			{
				if (tokens[t].kind == TokenKind::Identifier && tree.Text(tokens[t]) == "deprecated")
				{
					return true;
				}
			}

			return false;
		}

		bool HasAutoPlaceholder(std::string_view type)
		{
			for (auto position = type.find("auto"); position != std::string_view::npos;
				position = type.find("auto", position + 4))
			{
				if ((position == 0 ||!IsIdentChar(type[position - 1])) &&
					(position + 4 == type.size() ||!IsIdentChar(type[position + 4])))
				{
					return true;
				}
			}

			return false;
		}

		CompletionItem DescribeDeclared(
			const ParseTree& tree,
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t node,
			std::string_view name,
			CompletionKind kind)
		{
			constexpr std::size_t kMaxDocLen        = 1000;
			constexpr std::size_t kMaxConstraintLen = 96;
			std::string detail                      = KindDetail(kind);
			if (kind == CompletionKind::Function)
			{
				const std::string signature = FunctionSignature(tree, node, name);
				if (!signature.empty())
				{
					detail = signature;
				}

				if (name.starts_with("operator "))
				{
					auto function = tree.NodesSoA().Parent(node);
					for (std::size_t depth = 0; depth < kMaxParentWalkDepth &&
						function < tree.NodesSoA().size(); ++depth)
					{
						const auto parentKind = tree.NodesSoA().Kind(function);
						if (parentKind == GrammarKind::FunctionDefinition || parentKind == GrammarKind::FunctionDeclaration)
						{
							const auto first      = tree.NodesSoA().FirstToken(function);
							const auto nameToken  = tree.NodesSoA().FirstToken(node);
							const auto qualifiers = CompactWs(SliceRange(tree, first, nameToken - first), kMaxDetailLen);
							if (!qualifiers.empty())
							{
								detail = qualifiers + " " + detail;
							}

							break;
						}

						function = tree.NodesSoA().Parent(function);
					}
				}

				// Associated constraints belong to the signature: show the
				// requires-clause (leading or trailing) with the function.
				const std::size_t clause = FindInSubtree(tree, node, GrammarKind::RequiresClause);
				if (clause == NoIndex)
				{
					// Trailing clauses may sit beside the declarator rather
					// than under it; look from the enclosing function.
					auto function = tree.NodesSoA().Parent(node);
					for (std::size_t depth = 0;
						depth < kMaxParentWalkDepth && function < tree.NodesSoA().size();
						++depth)
					{
						const GrammarKind parentKind = tree.NodesSoA().Kind(function);
						if (parentKind == GrammarKind::FunctionDefinition ||
							parentKind == GrammarKind::FunctionDeclaration)
						{
							break;
						}

						function = tree.NodesSoA().Parent(function);
					}

					if (function < tree.NodesSoA().size() &&
						(tree.NodesSoA().Kind(function) == GrammarKind::FunctionDefinition ||
						tree.NodesSoA().Kind(function) == GrammarKind::FunctionDeclaration))
					{
						const std::size_t outer =
							FindInSubtree(tree, function, GrammarKind::RequiresClause);
						if (outer != NoIndex)
						{
							const auto clauseRef = tree.NodesSoA()[outer];
							const std::string constraint = CompactWs(
								SliceRange(tree, clauseRef.GetFirstToken(),
								clauseRef.GetTokenCount()),
								kMaxConstraintLen);
							if (!constraint.empty())
							{
								detail += ' ';
								detail += constraint;
							}
						}
					}
				}
				else
				{
					const auto clauseRef = tree.NodesSoA()[clause];
					const std::string constraint = CompactWs(
						SliceRange(tree, clauseRef.GetFirstToken(), clauseRef.GetTokenCount()),
						kMaxConstraintLen);
					if (!constraint.empty())
					{
						detail += ' ';
						detail += constraint;
					}
				}
			}
			else if (kind == CompletionKind::Type && ConceptDefinitionOf(tree, node) != NoIndex)
			{
				// Concepts read as their definition: `concept Name = constraint`.
				const std::string definition = ConceptDefinitionText(tree, node, name);
				if (!definition.empty())
				{
					detail = definition;
				}
			}
			else if (kind == CompletionKind::Variable)
			{
				const std::string scoped = EnumeratorDetail(tree, node, name);
				if (!scoped.empty())
				{
					detail = scoped;
				}
				else
				{
					const std::string type = VariableTypeDetail(tree, node);
					if (!type.empty())
					{
						detail = type;
					}
				}
			}

			if (detail.size() > kMaxDetailLen)
			{
				detail.resize(kMaxDetailLen);
			}

			std::string documentation = DocCommentFor(source, tokens, DocAnchorToken(tree, node));
			if (documentation.size() > kMaxDocLen)
			{
				documentation.resize(kMaxDocLen);
			}

			CompletionItem item {std::string(name), kind, std::move(detail),
				std::move(documentation)};
			if (kind == CompletionKind::Variable)
			{
				item.type_text   = VariableTypeDetail(tree, node);
				item.layout_type = DeclaredLayoutType(tree, node, item.type_text);
			}
			else if (kind == CompletionKind::Function)
			{
				item.type_text      = ReturnTypeText(tree, node);
				item.templateParams = TemplateParamNames(tree, node);
				if (HasAutoPlaceholder(item.type_text))
				{
					item.deduction = TemplateDeduction::Capture(tree, node);
				}
			}
			else if (kind == CompletionKind::Type)
			{
				item.type_text = AliasTargetText(tree, node);
				if (!item.type_text.empty())
				{
					if (item.type_text.starts_with("typedef "))
					{
						item.type_text.erase(0, 8);
					}

					item.type_text = DeclaredLayoutType(tree, node, item.type_text);
					if (item.type_text.starts_with("static "))
					{
						item.type_text.erase(0, 7);
					}

					item.detail = "using " + item.label + " = " + item.type_text;
				}
			}

			const std::size_t token = tree.NodesSoA().FirstToken(node);
			if (token < tokens.size())
			{
				item.has_location = true;
				item.offset       = tokens[token].offset;
			}

			item.is_definition = kind != CompletionKind::Function;
			if (kind == CompletionKind::Function)
			{
				const std::size_t declarator = DeclaratorOf(tree, node);
				if (declarator != NoIndex)
				{
					const std::size_t owner = tree.NodesSoA().Parent(declarator);
					item.is_definition =
						owner < tree.NodesSoA().size() &&
						tree.NodesSoA().Kind(owner) == GrammarKind::FunctionDefinition;
					const std::size_t suffix =
						FindInSubtree(tree, declarator, GrammarKind::FunctionSuffix);
					if (suffix != NoIndex)
					{
						for (const std::size_t child : tree.DirectChildren(suffix))
						{
							if (tree.NodesSoA().Kind(child) == GrammarKind::ParameterDeclaration)
							{
								++item.param_count;
							}
						}
					}
				}

				ApplyFunctionQualifiers(item);
				item.deleted = DeclaredDeleted(tree, node);
			}

			item.deprecated = DeclaredDeprecated(tree, node);
			return item;
		}

		// For `template<...> struct S`, the documentation sits above `template`, not
		// above `struct`. Walks back over one balanced `<>` group to the `template`
		// keyword; returns intro_token unchanged when the shape differs.
		std::size_t TemplateHeadBefore(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t intro_token)
		{
			std::size_t prev = PreviousSignificant(tokens, intro_token, source);
			if (prev >= tokens.size() || tokens[prev].kind != TokenKind::Punctuation)
			{
				return intro_token;
			}

			const std::string_view closer = TokenText(source, tokens[prev]);
			int depth                     = 0;
			if (closer == ">")
			{
				depth = 1;
			}
			else if (closer == ">>")
			{
				depth = kDoubleAngleCount;
			}
			else
			{
				return intro_token;
			}

			std::size_t j = prev;
			while (j > 0)
			{
				--j;
				if (tokens[j].kind == TokenKind::Whitespace ||
					tokens[j].kind == TokenKind::LineComment ||
					tokens[j].kind == TokenKind::BlockComment)
				{
					continue;
				}

				if (tokens[j].kind != TokenKind::Punctuation)
				{
					continue;
				}

				const std::string_view text = TokenText(source, tokens[j]);
				if (text == ">" || text == ">=")
				{
					++depth;
				}
				else if (text == ">>")
				{
					depth += kDoubleAngleCount;
				}
				else if (text == "<" || text == "<=" || text == "<=>")
				{
					if (--depth == 0)
					{
						const std::size_t keyword = PreviousSignificant(tokens, j, source);
						if (keyword < tokens.size() &&
							tokens[keyword].kind == TokenKind::Identifier &&
							TokenText(source, tokens[keyword]) == "template")
						{
							return keyword;
						}

						return intro_token;
					}
				}
				else
				{
					return intro_token;
				} // `;`, `{`, `(`, ... : not a template head
			}

			return intro_token;
		}

		CompletionItem MakeTagItem(
			std::string_view source,
			const std::vector<Token>& tokens,
			const TagName& tag)
		{
			std::string detail(tag.intro);
			detail += ' ';
			detail += tag.text;
			CompletionItem item {
				std::string(tag.text), CompletionKind::Type, std::move(detail),
				DocCommentFor(source, tokens, TemplateHeadBefore(source, tokens, tag.intro_token))
			};
			item.has_location = true;
			item.offset       = static_cast<std::uint32_t>(tag.offset);
			// `struct S {` / `struct S : B {` / `using S = ...` define; `struct S;`
			// and elaborated uses (`struct S x;`) do not.
			bool defines = tag.intro == "using" || tag.intro == "concept";
			for (std::size_t t = tag.name_token + 1; t < tokens.size() && !defines; ++t)
			{
				if (tokens[t].kind == TokenKind::Whitespace ||
					tokens[t].kind == TokenKind::LineComment ||
					tokens[t].kind == TokenKind::BlockComment)
				{
					continue;
				}

				const std::string_view next = TokenText(source, tokens[t]);
				defines                     = next == "{" || next == ":" || next == "final";
				break;
			}

			item.is_definition = defines;
			if (tag.intro == "using")
			{
				std::size_t begin = tag.name_token + 1;
				while (begin < tokens.size() && tokens[begin].kind == TokenKind::Whitespace)
				{
					++begin;
				}

				if (begin < tokens.size() && TokenText(source, tokens[begin]) == "=")
				{
					++begin;
					std::size_t end = begin;
					while (end < tokens.size() && TokenText(source, tokens[end]) != ";")
					{
						++end;
					}

					if (end > begin)
					{
						item.type_text = CompactWs(
							std::string(source.substr(
							tokens[begin].offset,
							tokens[end - 1].offset + tokens[end - 1].length -
							tokens[begin].offset)),
							kMaxTypeTextLen);
						item.detail += " = " + item.type_text;
					}
				}
			}

			// Concepts read as their definition (`concept Name = constraint`),
			// mirroring the `using` alias handling above.
			if (tag.intro == "concept")
			{
				std::size_t begin = tag.name_token + 1;
				while (begin < tokens.size() && tokens[begin].kind == TokenKind::Whitespace)
				{
					++begin;
				}

				// An explicit specialization (`concept C<int> = ...`) is not a
				// definition to complete from; leave the bare detail.
				if (begin < tokens.size() && TokenText(source, tokens[begin]) == "=")
				{
					++begin;
					std::size_t end = begin;
					while (end < tokens.size() && TokenText(source, tokens[end]) != ";")
					{
						++end;
					}

					if (end > begin)
					{
						item.type_text = CompactWs(
							std::string(source.substr(
							tokens[begin].offset,
							tokens[end - 1].offset + tokens[end - 1].length -
							tokens[begin].offset)),
							kMaxTypeTextLen);
						item.detail += " = " + item.type_text;
					}
				}
			}

			if (tag.intro.starts_with("enum"))
			{
				// `enum class E : std::uint8_t {`: the underlying type, else implicit.
				std::string underlying;
				bool colon = false;
				for (std::size_t t = tag.name_token + 1; t < tokens.size(); ++t)
				{
					if (tokens[t].kind == TokenKind::Whitespace ||
						tokens[t].kind == TokenKind::LineComment ||
						tokens[t].kind == TokenKind::BlockComment)
					{
						continue;
					}

					const std::string_view next = TokenText(source, tokens[t]);
					if (!colon)
					{
						colon = next == ":";
						if (!colon)
						{
							break;
						}

						continue;
					}

					if (next == "{" || next == ";")
					{
						break;
					}

					if (!underlying.empty() && IsIdentChar(next.front()) &&
						IsIdentChar(underlying.back()))
					{
						underlying += ' ';
					}

					underlying += next;
				}

				item.layout_type = std::move(underlying);
			}

			return item;
		}

		CompletionItem MakeNamespaceItem(
			const ParseTree& tree,
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t node,
			const std::vector<std::string>& full_path)
		{
			std::string joined;
			for (const auto& element : full_path)
			{
				if (element.empty())
				{
					continue;
				}

				if (!joined.empty())
				{
					joined += "::";
				}

				joined += element;
			}

			std::string detail = "namespace";
			if (!joined.empty())
			{
				detail += ' ';
				detail += joined;
			}

			// A record/enum scope reads as `enum class n::Mode`, not `namespace n::Mode`.
			if (tree.NodesSoA().Kind(node) == GrammarKind::RecordDefinition && !joined.empty())
			{
				std::string intro;
				const auto record = tree.NodesSoA()[node];
				const std::size_t record_end =
					std::min<std::size_t>(record.GetFirstToken() + record.GetTokenCount(),
					tokens.size());
				for (std::size_t t = record.GetFirstToken(); t < record_end; ++t)
				{
					if (tokens[t].kind != TokenKind::Identifier || tree.IsDecorationToken(t))
					{
						continue;
					}

					const std::string_view word = TokenText(source, tokens[t]);
					if (word != "enum" && word != "class" && word != "struct" && word != "union")
					{
						break;
					}

					intro += word;
					intro += ' ';
				}

				if (!intro.empty())
				{
					detail = intro + joined;
				}
			}

			const std::string label = joined.empty() ? std::string {}: full_path.back();
			CompletionItem item {label, CompletionKind::Namespace, std::move(detail),
				DocCommentFor(source, tokens, tree.NodesSoA().FirstToken(node))};
			// Match the declared component, including qualified record names.
			const auto grammar = tree.NodesSoA()[node];
			for (std::size_t t = grammar.GetFirstToken();
				t < grammar.GetFirstToken() + grammar.GetTokenCount() && t < tokens.size();
				++t)
			{
				if (tokens[t].kind != TokenKind::Identifier || tree.IsDecorationToken(t))
				{
					continue;
				}

				const std::string_view word = TokenText(source, tokens[t]);
				if (word != label)
				{
					continue;
				}

				item.has_location  = true;
				item.offset        = tokens[t].offset;
				item.is_definition = true;
				break;
			}

			return item;
		}

		void CollectDefines(
			std::string_view source,
			const std::vector<Token>& tokens,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best,
			std::string_view prefix)
		{
			std::vector<Define> defines;
			ScanDefines(source, tokens, defines);
			for (const auto& define : defines)
			{
				if (!prefix.empty() && !StartsWith(define.name, prefix))
				{
					continue;
				}

				std::string detail = define.value.empty() ? "macro" : define.value;
				if (detail.size() > kMaxShortDetailLen)
				{
					detail.resize(kMaxShortDetailLen);
				}

				InsertItem(interner, best,
					{std::string(define.name), CompletionKind::Macro, std::move(detail),
						DocCommentFor(source, tokens, define.hash_token)});
			}
		}

		// Overload for string-based unordered_map (preprocessor path)
		void CollectDefines(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::unordered_map<std::string, CompletionItem>& best,
			std::string_view prefix)
		{
			std::vector<Define> defines;
			ScanDefines(source, tokens, defines);
			for (const auto& define : defines)
			{
				if (!prefix.empty() && !StartsWith(define.name, prefix))
				{
					continue;
				}

				std::string detail = define.value.empty() ? "macro" : define.value;
				if (detail.size() > kMaxShortDetailLen)
				{
					detail.resize(kMaxShortDetailLen);
				}

				InsertItem(best,
					{std::string(define.name), CompletionKind::Macro, std::move(detail),
						DocCommentFor(source, tokens, define.hash_token)});
			}
		}

		void CollectTagNamesIn(
			std::string_view source,
			const std::vector<Token>& tokens,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best,
			std::string_view prefix,
			std::size_t range_start,
			std::size_t range_end,
			const HiddenBodies* hidden = nullptr)
		{
			std::vector<TagName> tags;
			ScanTagNames(source, tokens, tags);
			for (const auto& tag : tags)
			{
				if (IsKeyword(tag.text))
				{
					continue;
				}

				if (tag.offset < range_start || tag.offset >= range_end)
				{
					continue;
				}

				if (hidden != nullptr && hidden->Contains(tag.offset))
				{
					continue;
				}

				if (!MatchesPrefix(tag.text, prefix))
				{
					continue;
				}

				InsertItem(interner, best, MakeTagItem(source, tokens, tag));
			}
		}

		void CollectTagNames(
			std::string_view source,
			const std::vector<Token>& tokens,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best,
			std::string_view prefix,
			const HiddenBodies* hidden = nullptr)
		{
			CollectTagNamesIn(source, tokens, interner, best, prefix, 0, source.size(), hidden);
		}

		// Nested scope names below a qualifier: for `ns::`, a scope with path
		// `ns::inner` contributes `inner`. Powers both `ns::` member listing and the
		// global `::` scope, including names introduced by compound definitions
		// (`namespace a::b`) that have no intermediate node to match exactly.
		void CollectChildScopes(
			const ParseTree& tree,
			std::string_view source,
			const std::vector<Token>& tokens,
			const std::vector<std::vector<std::string>>& scope_paths,
			const std::vector<std::string>& qualifier,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best,
			std::string_view prefix)
		{
			for (std::size_t n = 0; n < scope_paths.size(); ++n)
			{
				if (scope_paths[n].empty())
				{
					continue;
				}

				const std::vector<std::string>& path = scope_paths[n];
				if (path.size() <= qualifier.size())
				{
					continue;
				}

				bool matches = true;
				for (std::size_t i = 0; i < qualifier.size(); ++i)
				{
					if (path[i] != qualifier[i])
					{
						matches = false;
						break;
					}
				}

				if (!matches)
				{
					continue;
				}

				const std::string& name = path[qualifier.size()];
				if (name.empty())
				{
					continue;
				}

				// Reserved (`_`-leading) scopes are never meant to be named.
				if (name.front() == '_')
				{
					continue;
				}

				if (!MatchesPrefix(name, prefix))
				{
					continue;
				}

				std::vector<std::string> full_path(path.begin(), path.begin() + qualifier.size() + 1);
				InsertItem(interner, best, MakeNamespaceItem(tree, source, tokens, n, full_path));
			}
		}

		// Access sections of a record body: the key's default (`class` private,
		// `struct`/`union` public) and each `public:`/`protected:`/`private:` label.
		struct AccessTable
		{
			MemberAccess initial = MemberAccess::Public;
			std::vector<std::pair<std::size_t, MemberAccess>> changes; // (offset, access)

			[[nodiscard]] MemberAccess At(std::size_t offset) const
			{
				MemberAccess access = initial;
				for (const auto& [at, change] : changes)
				{
					if (at >= offset)
					{
						break;
					}

					access = change;
				}

				return access;
			}
		};

		AccessTable BuildAccessTable(const ParseTree& tree, std::size_t record)
		{
			AccessTable table;
			const auto grammar = tree.NodesSoA()[record];
			const auto& tokens = tree.Tokens();
			const std::size_t end =
				std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
				tokens.size());
			const auto significant_after = [&](std::size_t t)
			{
				std::size_t next = t + 1;
				while (next < end && (tokens[next].kind == TokenKind::Whitespace ||
					tokens[next].kind == TokenKind::LineComment ||
					tokens[next].kind == TokenKind::BlockComment))
				{
					++next;
				}

				return next;
			};
			std::size_t t = grammar.GetFirstToken();
			int angle     = 0;
			for (; t < end; ++t)
			{
				const std::string_view text = tree.Text(tokens[t]);
				if (tokens[t].kind == TokenKind::Punctuation)
				{
					if (text == "{")
					{
						break;
					}

					angle += text == "<" ? 1 : text == ">" ? -1 : text == ">>" ? -2 : 0;
					continue;
				}

				if (angle <= 0 && tokens[t].kind == TokenKind::Identifier)
				{
					if (text == "class")
					{
						table.initial = MemberAccess::Private;
					}
					else if (text == "struct" || text == "union")
					{
						table.initial = MemberAccess::Public;
					}
				}
			}

			int depth = 0;
			for (; t < end; ++t)
			{
				const std::string_view text = tree.Text(tokens[t]);
				if (tokens[t].kind == TokenKind::Punctuation)
				{
					depth += text == "{" ? 1 : text == "}" ? -1 : 0;
					continue;
				}

				if (depth != 1 || tokens[t].kind != TokenKind::Identifier)
				{
					continue;
				}

				const std::size_t next = significant_after(t);
				if (next >= end || tree.Text(tokens[next]) != ":")
				{
					continue;
				}

				if (text == "public")
				{
					table.changes.emplace_back(tokens[t].offset, MemberAccess::Public);
				}
				else if (text == "protected")
				{
					table.changes.emplace_back(tokens[t].offset, MemberAccess::Protected);
				}
				else if (text == "private")
				{
					table.changes.emplace_back(tokens[t].offset, MemberAccess::Private);
				}
			}

			return table;
		}

		// `union { int a; float b; };` as a member: an unnamed class that declares no
		// object, whose members are members of the enclosing class.
		bool IsAnonymousMemberRecord(const ParseTree& tree, std::size_t node)
		{
			const auto elements = ScopeNameElements(tree, node);
			if (elements.size() != 1 ||!elements.front().empty() ||
				HiddenBodies::IsUnscopedEnum(tree, node))
			{
				return false;
			}

			const auto grammar = tree.NodesSoA()[node];
			const auto& tokens = tree.Tokens();
			bool keyword       = false;
			for (std::uint32_t k = 0; k < grammar.GetTokenCount(); ++k)
			{
				const std::string_view text = tree.Text(tokens[grammar.GetFirstToken() + k]);
				if (text == "enum")
				{
					return false;
				}

				if (text == "struct" || text == "union" || text == "class")
				{
					keyword = true;
					break;
				}
			}

			std::size_t next = grammar.GetFirstToken() + grammar.GetTokenCount();
			while (next < tokens.size() && (tokens[next].kind == TokenKind::Whitespace ||
				tokens[next].kind == TokenKind::LineComment ||
				tokens[next].kind == TokenKind::BlockComment))
			{
				++next;
			}

			return keyword && next < tokens.size() && tree.Text(tokens[next]) == ";";
		}

		// Scope whose qualified lookup lists the name: unscoped enumerators and
		// members of anonymous unions belong to the enclosing scope.
		std::size_t LookupScope(const ParseTree& tree, std::size_t node)
		{
			std::size_t scope = MemberScope(tree, node);
			while (scope < tree.NodesSoA().size() &&
				tree.NodesSoA().Kind(scope) == GrammarKind::RecordDefinition &&
				(HiddenBodies::IsUnscopedEnum(tree, scope) || IsAnonymousMemberRecord(tree, scope)))
			{
				scope = MemberScope(tree, scope);
			}

			return scope;
		}

		// Innermost function definition (not lambda) containing offset, or NoIndex.
		std::size_t EnclosingFunction(const ParseTree& tree, std::size_t offset)
		{
			std::size_t best = NoIndex;
			auto best_span   = static_cast<std::size_t>(-1);
			for (std::size_t n = 1; n < tree.NodesSoA().size(); ++n)
			{
				if (tree.NodesSoA().Kind(n) != GrammarKind::FunctionDefinition)
				{
					continue;
				}

				const auto[start, end] = NodeRange(tree, n);
				if (start <= offset && offset <= end && end - start < best_span)
				{
					best      = n;
					best_span = end - start;
				}
			}

			return best;
		}

		// `void f(this Self& self)`: the first parameter starts with `this`.
		bool HasExplicitObjectParameter(const ParseTree& tree, std::size_t function)
		{
			if (function == NoIndex)
			{
				return false;
			}

			const std::size_t declarator = FindChild(tree, function, GrammarKind::Declarator);
			const std::size_t suffix =
				declarator == NoIndex ? NoIndex
			: FindInSubtree(tree, declarator, GrammarKind::FunctionSuffix);
			if (suffix == NoIndex)
			{
				return false;
			}

			for (const std::size_t child : tree.DirectChildren(suffix))
			{
				if (tree.NodesSoA().Kind(child) != GrammarKind::ParameterDeclaration)
				{
					continue;
				}

				const auto grammar = tree.NodesSoA()[child];
				for (std::uint32_t k = 0; k < grammar.GetTokenCount(); ++k)
				{
					const Token& token = tree.Tokens()[grammar.GetFirstToken() + k];
					if (token.kind == TokenKind::Whitespace ||
						token.kind == TokenKind::LineComment ||
						token.kind == TokenKind::BlockComment)
					{
						continue;
					}

					return tree.Text(token) == "this";
				}

				return false;
			}

			return false;
		}

		// Innermost function definition and class body around the cursor.
		struct CursorScopes
		{
			std::size_t function = NoIndex;
			// Class/struct/union (or scoped enum) whose body contains the cursor.
			std::size_t record = NoIndex;
			// The function takes an explicit object parameter: no implicit `this`.
			bool explicit_object = false;
		};

		CursorScopes FindCursorScopes(const ParseTree& tree, std::size_t offset)
		{
			CursorScopes scopes;
			auto function_span = static_cast<std::size_t>(-1);
			auto record_span   = static_cast<std::size_t>(-1);
			for (std::size_t n = 1; n < tree.NodesSoA().size(); ++n)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(n);
				if (kind == GrammarKind::FunctionDefinition)
				{
					const auto[start, end] = NodeRange(tree, n);
					if (start <= offset && offset <= end && end - start < function_span)
					{
						scopes.function = n;
						function_span   = end - start;
					}
				}
				else if (kind == GrammarKind::RecordDefinition)
				{
					const auto[start, end] = HiddenBodies::RecordBodyRange(tree, n);
					if (start < offset && offset < end && end - start < record_span &&
						!HiddenBodies::IsUnscopedEnum(tree, n))
					{
						scopes.record = n;
						record_span   = end - start;
					}
				}
			}

			scopes.explicit_object = HasExplicitObjectParameter(tree, scopes.function);
			return scopes;
		}

	} // namespace

	static ScopeIndex BuildScopeIndex(const ParseTree& tree, bool for_header);

	ScopeIndex CompletionEngine::IndexScopes(
		std::string_view source,
		const ParserOptions& options)
	{
		const ParseTree tree = ParseTree::Parse(source, options);
		return IndexScopes(tree);
	}

	ScopeIndex CompletionEngine::IndexScopes(const ParseTree& tree)
	{
		return BuildScopeIndex(tree, true);
	}

	// `operator->`, `operator==`, `operator()` and conversion operators name the
	// function `operator` in the DeclaredName node; the operator itself follows.
	// Returns the full label (`operator->`) or empty when this is not an
	// operator declaration.
	std::string OperatorFunctionName(const ParseTree& tree, std::size_t name_token)
	{
		const std::vector<Token>& tokens = tree.Tokens();
		if (name_token >= tokens.size() || tree.Text(tokens[name_token]) != "operator")
		{
			return {};
		}

		auto significant = [&](std::size_t t)
		{
			while (++t < tokens.size())
			{
				const TokenKind kind = tokens[t].kind;
				if (kind != TokenKind::Whitespace && kind != TokenKind::LineComment &&
					kind != TokenKind::BlockComment)
				{
					return t;
				}
			}

			return tokens.size();
		};

		constexpr std::size_t kMaxOperatorTokens = 8;
		std::string label                        = "operator";
		std::size_t current                      = name_token;
		for (std::size_t count = 0; count < kMaxOperatorTokens; ++count)
		{
			const std::size_t next = significant(current);
			if (next >= tokens.size())
			{
				return {};
			}

			const Token& token          = tokens[next];
			const std::string_view text = tree.Text(token);
			if (token.kind == TokenKind::Punctuation)
			{
				if (text == "(")
				{
					// `(` directly after `operator` can only be the call
					// operator itself (`operator()`); anywhere else it starts
					// the parameter list, ending the operator spelling.
					if (label == "operator")
					{
						return "operator()";
					}

					return label;
				}

				if (text == ";" || text == "{" || text == "}" || text == ",")
				{
					return {};
				}

				label += text;
				current = next;
				continue;
			}

			// A conversion operator (`operator bool`, `operator ns::Type`).
			if ((label == "operator" || label.starts_with("operator ")) && text != "operator" &&
				token.kind == TokenKind::Identifier)
			{
				if (label.back() != ':')
				{
					label += ' ';
				}

				label += text;
				current = next;
				continue;
			}

			return label == "operator" ? std::string {}: label;
		}

		return {};
	}

	// `for_header` drops `_`-leading variables (implementation detail of a
	// header); the buffer's own index keeps them so `obj.` offers `_field`.
	static ScopeIndex BuildScopeIndex(const ParseTree& tree, bool for_header)
	{
		const std::string_view source    = tree.Source();
		const std::vector<Token>& tokens = tree.Tokens();
		ScopeIndex index;

		std::unordered_map<std::string, std::size_t> entry_pos;
		auto path_key = [](const std::vector<std::string>& path)
		{
			std::string key;
			for (const auto& element : path)
			{
				key += element;
				key += '\0';
			}

			return key;
		};
		auto entry_for =
			[&](const std::vector<std::string>& path, CompletionKind kind) -> IndexedScope &
		{
			const std::string key = path_key(path);
			if (const auto found = entry_pos.find(key); found != entry_pos.end())
			{
				return index[found->second];
			}

			const std::size_t pos = index.size();
			index.push_back({path, kind, {}});
			entry_pos.emplace(key, pos);
			return index.back();
		};
		entry_for({}, CompletionKind::Keyword);

		const std::vector<std::vector<std::string>> scope_paths = BuildScopePaths(tree);

		// Compound namespace definitions introduce every component, including
		// intermediate scopes that have no separate grammar node.
		for (std::size_t n = 0; n < scope_paths.size(); ++n)
		{
			if (tree.NodesSoA().Kind(n) != GrammarKind::NamespaceDefinition ||
				IsInlineNamespace(tree, n))
			{
				continue;
			}

			const auto elements = ScopeNameElements(tree, n);
			const auto& path    = scope_paths[n];
			for (std::size_t depth = path.size() - elements.size(); depth < path.size(); ++depth)
			{
				if (path[depth].empty())
				{
					continue;
				}

				const std::vector<std::string> full_path(path.begin(), path.begin() + depth + 1);
				entry_for(full_path, CompletionKind::Namespace).kind = CompletionKind::Namespace;
				if (path[depth].front() != '_')
				{
					const std::vector<std::string> owner_path(path.begin(), path.begin() + depth);
					entry_for(owner_path, CompletionKind::Namespace)
					.members.push_back(MakeNamespaceItem(tree, source, tokens, n, full_path));
				}
			}
		}

		// Declared names bucketed by owning scope; function locals land on body
		// blocks (no entry) and are skipped: they are not qualifier-addressable.
		std::unordered_map<std::size_t, AccessTable> access_tables;
		for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
		{
			if (tree.NodesSoA().Kind(n) != GrammarKind::DeclaredName)
			{
				continue;
			}

			const std::size_t scope = MemberScope(tree, n);
			if (scope >= tree.NodesSoA().size())
			{
				continue;
			}

			const GrammarKind scope_kind = tree.NodesSoA().Kind(scope);
			if (scope_kind != GrammarKind::NamespaceDefinition &&
				scope_kind != GrammarKind::RecordDefinition && scope != ParseTree::RootNode)
			{
				continue;
			}

			const std::size_t token_index = tree.NodesSoA().FirstToken(n);
			if (token_index >= tree.Tokens().size())
			{
				continue;
			}

			std::string name_storage;
			std::string_view name = tree.Text(tree.Tokens()[token_index]);
			if (name == "operator")
			{
				// Overloaded and conversion operators are indexed under their full
				// spelling (`operator->`) so smart-pointer dereference resolves.
				name_storage = OperatorFunctionName(tree, token_index);
				if (name_storage.empty())
				{
					continue;
				}

				name = name_storage;
			}
			else if (name.empty() || IsKeyword(name))
			{
				continue;
			}

			const CompletionKind kind = ClassifyDeclaredName(tree, n);
			if (for_header && kind == CompletionKind::Variable && name.front() == '_')
			{
				// Dropped from the index, but still occupies storage in its record.
				if (scope_kind == GrammarKind::RecordDefinition)
				{
					entry_for(
						scope < scope_paths.size() ? scope_paths[scope] : ScopePath(tree, scope),
						CompletionKind::Type)
					.layout_unknown = true;
				}

				continue;
			}

			CompletionItem member = DescribeDeclared(tree, source, tokens, n, name, kind);
			if (scope_kind == GrammarKind::RecordDefinition)
			{
				auto table = access_tables.find(scope);
				if (table == access_tables.end())
				{
					table = access_tables.emplace(scope, BuildAccessTable(tree, scope)).first;
				}

				member.access = table->second.At(tree.Tokens()[token_index].offset);
			}

			IndexedScope& entry =
				entry_for(scope < scope_paths.size() ? scope_paths[scope] : ScopePath(tree, scope),
				CompletionKind::Type);
			entry.members.push_back(std::move(member));

			// Unscoped enumerators are also members of the scope enclosing the enum.
			const std::size_t lookup = LookupScope(tree, n);
			if (lookup != scope && lookup < tree.NodesSoA().size() &&
				(tree.NodesSoA().Kind(lookup) == GrammarKind::NamespaceDefinition ||
				tree.NodesSoA().Kind(lookup) == GrammarKind::RecordDefinition ||
				lookup == ParseTree::RootNode))
			{
				CompletionItem hoisted = DescribeDeclared(tree, source, tokens, n, name, kind);
				if (tree.NodesSoA().Kind(lookup) == GrammarKind::RecordDefinition)
				{
					auto table = access_tables.find(lookup);
					if (table == access_tables.end())
					{
						table = access_tables.emplace(lookup, BuildAccessTable(tree, lookup)).first;
					}

					hoisted.access = table->second.At(tree.Tokens()[token_index].offset);
				}

				entry_for(lookup < scope_paths.size() ? scope_paths[lookup] : ScopePath(tree, lookup),
					CompletionKind::Type)
				.members.push_back(std::move(hoisted));
			}
		}

		// Records whose layout the indexed members cannot explain.
		const bool packed_file = source.find("#pragma pack") != std::string_view::npos;
		for (std::size_t n = 0; n < tree.NodesSoA().size() && n < scope_paths.size(); ++n)
		{
			if (tree.NodesSoA().Kind(n) != GrammarKind::RecordDefinition ||
				scope_paths[n].empty() || scope_paths[n].back().empty())
			{
				continue;
			}

			bool unknown       = packed_file;
			const auto grammar = tree.NodesSoA()[n];
			const std::size_t last =
				std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
				tokens.size());
			for (std::size_t t = grammar.GetFirstToken(); t < last && !unknown; ++t)
			{
				if (tokens[t].kind != TokenKind::Identifier)
				{
					continue;
				}

				const std::string_view word = tree.Text(tokens[t]);
				if (word == "virtual" || word == "alignas" || word == "_Alignas" ||
					word == "packed" || word == "__packed__" || word == "aligned" ||
					word == "__aligned__" || word == "no_unique_address")
				{
					unknown = true;
				}
				else if (word == "struct" || word == "union" || word == "class")
				{
					// Anonymous member record: its fields live in a scope of their own.
					std::size_t next = t + 1;
					while (next < last && (tokens[next].kind == TokenKind::Whitespace ||
						tokens[next].kind == TokenKind::LineComment ||
						tokens[next].kind == TokenKind::BlockComment))
					{
						++next;
					}

					unknown = next < last && tree.Text(tokens[next]) == "{";
				}
			}

			if (unknown)
			{
				entry_for(scope_paths[n], CompletionKind::Type).layout_unknown = true;
			}
		}

		// `using Name = Type;` carries no DeclaredName node: index the alias as a
		// Type member that remembers its target for member-access resolution.
		for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
		{
			if (tree.NodesSoA().Kind(n) != GrammarKind::UsingDeclaration)
			{
				continue;
			}

			const auto grammar = tree.NodesSoA()[n];
			const std::size_t last =
				std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
				tokens.size());
			std::size_t name_token = tokens.size();
			std::size_t equals     = tokens.size();
			for (std::size_t k = grammar.GetFirstToken(); k < last; ++k)
			{
				if (tokens[k].kind == TokenKind::Whitespace ||
					tokens[k].kind == TokenKind::LineComment ||
					tokens[k].kind == TokenKind::BlockComment)
				{
					continue;
				}

				const std::string_view word = tree.Text(tokens[k]);
				if (word == "using")
				{
					continue;
				}

				if (tokens[k].kind == TokenKind::Identifier && name_token == tokens.size())
				{
					name_token = k;
					continue;
				}

				if (name_token != tokens.size() && word == "=")
				{
					equals = k;
				}

				break;
			}

			if (name_token == tokens.size() || equals == tokens.size())
			{
				continue;
			}

			const std::size_t scope = MemberScope(tree, n);
			if (scope >= tree.NodesSoA().size())
			{
				continue;
			}

			const GrammarKind scope_kind = tree.NodesSoA().Kind(scope);
			if (scope_kind != GrammarKind::NamespaceDefinition &&
				scope_kind != GrammarKind::RecordDefinition && scope != ParseTree::RootNode)
			{
				continue;
			}

			const std::string_view name = tree.Text(tokens[name_token]);
			if (IsKeyword(name) ||(for_header && name.front() == '_'))
			{
				continue;
			}

			std::string target = CompactWs(
				SliceRange(tree, static_cast<std::uint32_t>(equals + 1), last -(equals + 1)),
				kMaxTypeTextLen);
			while (!target.empty() && (target.back() == ';' || target.back() == ' '))
			{
				target.pop_back();
			}

			CompletionItem item {std::string(name), CompletionKind::Type,
				KindDetail(CompletionKind::Type),
				DocCommentFor(source, tokens, grammar.GetFirstToken())};
			item.has_location   = true;
			item.offset         = tokens[name_token].offset;
			item.is_definition  = true;
			item.type_text      = std::move(target);
			item.templateParams = TemplateParamNames(tree, n);
			item.detail         = "using " + item.label + " = " + item.type_text;
			IndexedScope& entry =
				entry_for(scope < scope_paths.size() ? scope_paths[scope] : ScopePath(tree, scope),
				CompletionKind::Type);
			entry.members.push_back(std::move(item));
		}

		// Base classes, attached to the record's own entry (created on demand:
		// a record that only inherits has no members of its own).
		for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
		{
			if (tree.NodesSoA().Kind(n) != GrammarKind::RecordDefinition ||
				n >= scope_paths.size() || scope_paths[n].empty() || scope_paths[n].back().empty())
			{
				continue;
			}

			auto bases  = RecordBases(tree, n);
			auto params = TemplateParamNames(tree, n);

			bool hasReferenceConversion = false;
			const auto recordEnd = std::min<std::size_t>(
				tree.NodesSoA().FirstToken(n) + tree.NodesSoA().TokenCount(n), tree.Tokens().size());
			for (auto token = tree.NodesSoA().FirstToken(n); token < recordEnd; ++token)
			{
				if (tree.Tokens()[token].tok != Tok::KwOperator)
				{
					continue;
				}

				bool typeName = false;
				for (auto part = token + 1; part < recordEnd && tree.Tokens()[part].tok != Tok::LParen; ++part)
				{
					typeName |= tree.Tokens()[part].kind == TokenKind::Identifier;
					hasReferenceConversion |= typeName &&
						(tree.Tokens()[part].tok == Tok::Amp || tree.Tokens()[part].tok == Tok::AmpAmp);
				}
			}

			if (bases.empty() && params.empty() && !hasReferenceConversion)
			{
				continue;
			}

			IndexedScope& entry = entry_for(scope_paths[n], CompletionKind::Type);
			entry.hasReferenceConversion |= hasReferenceConversion;
			if (entry.template_params.empty())
			{
				entry.template_params = std::move(params);
			}

			for (auto& base : bases)
			{
				if (std::find(entry.bases.begin(), entry.bases.end(), base) == entry.bases.end())
				{
					entry.bases.push_back(std::move(base));
				}
			}
		}

		// Nested scope names owned by each entry's scope.
		for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
		{
			const GrammarKind kind = tree.NodesSoA().Kind(n);
			if (kind != GrammarKind::RecordDefinition)
			{
				continue;
			}

			const std::size_t owner = MemberScope(tree, n);
			if (owner >= tree.NodesSoA().size())
			{
				continue;
			}

			const GrammarKind owner_kind = tree.NodesSoA().Kind(owner);
			if (owner_kind != GrammarKind::NamespaceDefinition &&
				owner_kind != GrammarKind::RecordDefinition && owner != ParseTree::RootNode)
			{
				continue;
			}

			const auto elements = ScopeNameElements(tree, n);
			if (elements.empty() || elements.back().empty() || elements.back().front() == '_')
			{
				continue;
			}

			const std::vector<std::string> full_path =
				n < scope_paths.size() && !scope_paths[n].empty()
			? scope_paths[n]
			: ScopePath(tree, n);
			const std::vector<std::string> record_owner(full_path.begin(), full_path.end() - 1);
			IndexedScope& entry = entry_for(record_owner, CompletionKind::Type);
			entry.members.push_back(MakeNamespaceItem(tree, source, tokens, n, full_path));
		}

		// Tag names bucketed by innermost enclosing named scope.
		std::vector<TagName> tags;
		ScanTagNames(source, tokens, tags, &tree);
		for (const auto& tag : tags)
		{
			if (IsKeyword(tag.text) || tag.text.front() == '_')
			{
				continue;
			}

			std::size_t bucket = ParseTree::RootNode;
			auto span          = static_cast<std::size_t>(-1);
			for (std::size_t n = 1; n < tree.NodesSoA().size(); ++n)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(n);
				if (kind != GrammarKind::NamespaceDefinition &&
					kind != GrammarKind::RecordDefinition)
				{
					continue;
				}

				const auto[start, end] = NodeRange(tree, n);
				if (start <= tag.offset && tag.offset < end && end - start < span)
				{
					bucket = n;
					span   = end - start;
				}
			}

			IndexedScope& entry =
				entry_for(bucket < scope_paths.size() && !scope_paths[bucket].empty()
				? scope_paths[bucket]
				: ScopePath(tree, bucket),
				CompletionKind::Type);
			entry.members.push_back(MakeTagItem(source, tokens, tag));
		}

		// Macros are always global.
		{
			std::vector<Define> defines;
			ScanDefines(source, tokens, defines);
			IndexedScope& entry = entry_for({}, CompletionKind::Keyword);
			for (const auto& define : defines)
			{
				if (define.name.front() == '_')
				{
					continue;
				}

				std::string detail = define.value.empty() ? "macro" : define.value;
				if (detail.size() > kMaxShortDetailLen)
				{
					detail.resize(kMaxShortDetailLen);
				}

				entry.members.push_back(
					{std::string(define.name), CompletionKind::Macro, std::move(detail),
						DocCommentFor(source, tokens, define.hash_token)});
			}
		}
		for (auto& entry : index)
		{
			// Stable: redeclarations keep their source order, so the later one wins ties.
			std::stable_sort(entry.members.begin(), entry.members.end(),
				[](const CompletionItem& left, const CompletionItem& right)
				{
					if (left.label != right.label)
					{
						return left.label < right.label;
					}

					return static_cast<int>(left.kind) < static_cast<int>(right.kind);
				});
			// Merge same (label, kind) duplicates, keeping documentation. Function
			// overloads (different parameter types) stay separate entries.
			std::vector<CompletionItem> merged;
			std::size_t run_start = 0;
			for (auto& member : entry.members)
			{
				if (merged.empty() || merged.back().label != member.label ||
					merged.back().kind != member.kind)
				{
					run_start = merged.size();
				}

				CompletionItem* same = nullptr;
				for (std::size_t k = run_start; k < merged.size(); ++k)
				{
					if (member.kind != CompletionKind::Function ||
						ParameterTypesKey(merged[k]) == ParameterTypesKey(member))
					{
						same = &merged[k];
						break;
					}
				}

				if (same != nullptr)
				{
					if (same->documentation.empty() && !member.documentation.empty())
					{
						same->documentation = std::move(member.documentation);
					}

					if (same->type_text.empty() && !member.type_text.empty())
					{
						same->type_text = std::move(member.type_text);
					}

					if (same->layout_type.empty() && !member.layout_type.empty())
					{
						same->layout_type = std::move(member.layout_type);
					}

					if (same->detail == KindDetail(same->kind) &&
						member.detail != KindDetail(member.kind))
					{
						same->detail = std::move(member.detail);
					}
					else if (member.kind == CompletionKind::Function &&
						NamedParameterCount(member.detail) >= NamedParameterCount(same->detail) &&
						NamedParameterCount(member.detail) > 0)
					{
						same->detail = std::move(member.detail);
					}

					same->deprecated = same->deprecated || member.deprecated;
					continue;
				}

				merged.push_back(std::move(member));
			}

			entry.members = std::move(merged);
		}

		return index;
	}

	namespace
	{

		void CompleteExpression(
			const ParseTree& tree,
			std::string_view source,
			const std::vector<Token>& tokens,
			const ParserOptions& options,
			const std::vector<CallableInterval>& callables,
			const std::vector<std::vector<std::string>>& scope_paths,
			std::size_t offset,
			std::string_view prefix,
			const ScopeIndex* external,
			const CursorScopes& cursor,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best)
		{
			for (const auto keyword : kKeywords)
			{
				if (!MatchesPrefix(keyword, prefix))
				{
					continue;
				}

				// `compl`/`co_await` appear twice in the table; dedupe keeps one.
				const CompletionKind kind =
					IsBuiltinType(keyword) ? CompletionKind::Type : CompletionKind::Keyword;
				InsertItem(interner, best, {std::string(keyword), kind, KindDetail(kind), {}});
			}

			// Lexical fallback with occurrence visibility: an identifier is usable
			// when it occurs at global scope or earlier in the cursor's own function.
			// This hides locals of other functions while keeping words the grammar
			// has not modeled, and still works mid-recovery on incomplete code.
			// Callable intervals are binary-searched per token (was: one O(N) node
			// scan per token), and repeat identifiers are deduped by view before any
			// std::string allocation (was: one CompletionItem per occurrence).
			const std::size_t cursor_callable = FindCallable(callables, offset);
			const HiddenBodies hidden(tree, offset, cursor.explicit_object);
			std::unordered_set<std::string_view> seen_lexical;
			seen_lexical.reserve(tokens.size());
			std::size_t directive_end = 0;
			for (const auto& token : tokens)
			{
				// Directive lines name macros (completed from the `#define` scan with
				// their own filtering) and headers, not program identifiers.
				if (token.kind == TokenKind::Punctuation && TokenText(source, token) == "#" &&
					StartsLine(source, token.offset))
				{
					directive_end = DirectiveLineEnd(source, token.offset);
				}

				if (token.kind != TokenKind::Identifier || token.offset < directive_end)
				{
					continue;
				}

				const std::string_view text = TokenText(source, token);
				if (text == prefix)
				{
					continue;
				} // don't echo the word being typed

				if (seen_lexical.contains(text) ||!MatchesPrefix(text, prefix))
				{
					continue;
				}

				if (IsKeyword(text))
				{
					continue;
				} // keyword entry already added

				const std::size_t owner = FindCallable(callables, token.offset);
				if (owner != NoIndex && (owner != cursor_callable || token.offset >= offset))
				{
					continue;
				}

				if (owner == NoIndex && hidden.Contains(token.offset))
				{
					continue;
				}

				seen_lexical.insert(text);
				InsertItem(interner, best,
					{std::string(text), CompletionKind::Variable, "variable", {}});
			}

			// Declared names upgrade the kind (function/type vs plain variable) and
			// carry signatures plus documentation. Tag names (`struct Widget`) are
			// not always DeclaredName nodes, so collect them lexically as well.
			// Visible locals go first: unqualified lookup finds the innermost
			// scope, so a local shadows same-named members and globals in the
			// same-label merge below (hover and completion follow lookup).
			CollectTagNames(source, tokens, interner, best, prefix, &hidden);
			for (std::size_t pass = 0; pass < 2; ++pass)
			{
				for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
				{
					if (tree.NodesSoA().Kind(n) != GrammarKind::DeclaredName)
					{
						continue;
					}

					const std::size_t token_index = tree.NodesSoA().FirstToken(n);
					if (token_index >= tree.Tokens().size())
					{
						continue;
					}

					const std::string_view name = tree.Text(tree.Tokens()[token_index]);
					if (name.empty())
					{
						continue;
					}

					if (!MatchesPrefix(name, prefix))
					{
						continue;
					}

					const LocalInfo local = AnalyzeLocal(tree, n);
					bool usable_local     = false;
					if (local.is_local && local.owner != NoIndex)
					{
						// Point of declaration: usable only inside the owning block and
						// once the declaration itself is complete.
						const auto[block_start, block_end] = NodeRange(tree, local.boundary);
						const Token& name_token            = tree.Tokens()[token_index];
						usable_local = offset >= block_start && offset <= block_end &&
							name_token.offset + name_token.length <= offset;
					}

					if (local.is_local)
					{
						if (!usable_local || pass != 0)
						{
							continue;
						}

						InsertItem(
							interner, best,
							DescribeDeclared(tree, source, tokens, n, name, CompletionKind::Variable));
						continue;
					}

					if (pass != 1 || hidden.Contains(tree.Tokens()[token_index].offset))
					{
						continue;
					}

					const CompletionKind kind = ClassifyDeclaredName(tree, n);
					if (IsKeyword(name) && kind == CompletionKind::Variable)
					{
						continue;
					}

					InsertItem(interner, best, DescribeDeclared(tree, source, tokens, n, name, kind));
				}
			}

			// Top-level namespaces are visible unqualified (as qualifier heads) with
			// their documentation attached. Nested ones stay qualified-only. The
			// Namespace kind outranks the lexical fallback's plain Variable.
			for (std::size_t n = 0; n < scope_paths.size(); ++n)
			{
				if (tree.NodesSoA().Kind(n) != GrammarKind::NamespaceDefinition)
				{
					continue;
				}

				if (scope_paths[n].size() != 1 || scope_paths[n].front().empty())
				{
					continue;
				}

				const std::vector<std::string>& path = scope_paths[n];
				if (IsKeyword(path.front()) || path.front().front() == '_')
				{
					continue;
				}

				if (!MatchesPrefix(path.front(), prefix))
				{
					continue;
				}

				InsertItem(interner, best, MakeNamespaceItem(tree, source, tokens, n, path));
			}

			// Macros from `#define` and predefined compile-command defines.
			CollectDefines(source, tokens, interner, best, prefix);
			for (const auto& [name, value] : options.Macros())
			{
				if (!prefix.empty() && !StartsWith(name, prefix))
				{
					continue;
				}

				std::string detail = value.empty() ? "macro" : value;
				if (detail.size() > kMaxShortDetailLen)
				{
					detail.resize(kMaxShortDetailLen);
				}

				InsertItem(interner, best, {name, CompletionKind::Macro, std::move(detail), {}});
			}

			// Header globals (top-level functions, macros, using-aliases) are visible
			// unqualified once included. Namespaced header members stay qualified-only
			// unless the cursor sits inside that namespace (or one nested in it), where
			// they are in scope as in the compiler: `namespace app { void Server::Run() }`.
			if (external != nullptr)
			{
				std::vector<std::string> enclosing;
				for (std::size_t n = 0; n < scope_paths.size(); ++n)
				{
					if (tree.NodesSoA().Kind(n) != GrammarKind::NamespaceDefinition ||
						scope_paths[n].size() <= enclosing.size())
					{
						continue;
					}

					const auto[begin, end] = NodeRange(tree, n);
					if (offset >= begin && offset <= end)
					{
						enclosing = scope_paths[n];
					}
				}

				for (const auto& scope : *external)
				{
					if (scope.path.size() > enclosing.size() ||
						!std::equal(scope.path.begin(), scope.path.end(), enclosing.begin()))
					{
						continue;
					}

					for (const auto& member : scope.members)
					{
						if (!MatchesItem(member, prefix))
						{
							continue;
						}

						InsertItem(interner, best, member);
					}
				}
			}
		}

		// Absolute path a name written in `scope` refers to, trying enclosing scopes
		// from the innermost out; the written path itself when nothing matches.
		std::vector<std::string> ResolveWrittenScope(
			const std::vector<std::vector<std::string>>& scope_paths,
			const ScopeIndex* external,
			const std::vector<std::string>& scope,
			const std::vector<std::string>& written)
		{
			for (std::size_t keep = scope.size() + 1; keep > 0; --keep)
			{
				std::vector<std::string> candidate(scope.begin(), scope.begin() +(keep - 1));
				candidate.insert(candidate.end(), written.begin(), written.end());
				if (!ResolveScope(scope_paths, candidate).empty())
				{
					return candidate;
				}

				if (external != nullptr)
				{
					for (const auto& indexed : *external)
					{
						if (indexed.path == candidate)
						{
							return candidate;
						}
					}
				}
			}

			return written;
		}

		struct UsingDeclarationName
		{
			std::vector<std::string> scope;     // containing namespace
			std::vector<std::string> qualifier; // as written, before the name
			std::string name;
			bool global = false; // `using ::name;`
		};

		// `using a::b::name;` declarations (not directives or aliases).
		std::vector<UsingDeclarationName> UsingDeclarationsOf(
			const ParseTree& tree,
			const std::vector<std::vector<std::string>>& scope_paths)
		{
			std::vector<UsingDeclarationName> result;
			const auto& tokens = tree.Tokens();
			for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
			{
				if (tree.NodesSoA().Kind(n) != GrammarKind::UsingDeclaration)
				{
					continue;
				}

				const std::size_t owner = MemberScope(tree, n);
				if (owner != ParseTree::RootNode &&
					(owner >= tree.NodesSoA().size() ||
					tree.NodesSoA().Kind(owner) != GrammarKind::NamespaceDefinition))
				{
					continue;
				}

				const auto grammar = tree.NodesSoA()[n];
				UsingDeclarationName entry;
				entry.scope =
					owner == ParseTree::RootNode ? std::vector<std::string> {}: scope_paths[owner];
				std::vector<std::string> names;
				bool valid       = true;
				bool started     = false;
				bool expect_name = true;
				for (std::uint32_t k = 0; k < grammar.GetTokenCount() && valid; ++k)
				{
					const Token& token = tokens[grammar.GetFirstToken() + k];
					if (token.kind == TokenKind::Whitespace || token.kind == TokenKind::LineComment ||
						token.kind == TokenKind::BlockComment)
					{
						continue;
					}

					const std::string_view text = tree.Text(token);
					if (!started)
					{
						started = true;
						valid   = text == "using";
						continue;
					}

					if (text == ";")
					{
						break;
					}

					if (text == "::")
					{
						entry.global = entry.global || names.empty();
						expect_name  = true;
						continue;
					}

					if (token.kind != TokenKind::Identifier ||!expect_name || text == "namespace" ||
						text == "typename" || text == "enum")
					{
						valid = text == "typename" && names.empty();
						continue;
					}

					names.emplace_back(text);
					expect_name = false;
				}

				if (!valid || names.empty() ||(names.size() == 1 && !entry.global))
				{
					continue;
				}

				entry.name = names.back();
				names.pop_back();
				entry.qualifier = std::move(names);
				result.push_back(std::move(entry));
			}

			return result;
		}

		bool CollectQualifiedMembers(
			const ParseTree& tree,
			std::string_view source,
			const std::vector<Token>& tokens,
			const std::vector<std::vector<std::string>>& scope_paths,
			const std::vector<std::string>& path,
			std::string_view prefix,
			const ScopeIndex* external,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best);

		// Names a namespace exposes through `using namespace x;` and `using x::y;`.
		void CollectUsingMembers(
			const ParseTree& tree,
			std::string_view source,
			const std::vector<Token>& tokens,
			const std::vector<std::vector<std::string>>& scope_paths,
			const std::vector<std::string>& path,
			std::string_view prefix,
			const ScopeIndex* external,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best,
			int depth)
		{
			constexpr int kMaxUsingDepth = 4;
			if (depth > kMaxUsingDepth)
			{
				return;
			}

			for (const auto& directive : Navigation::UsingDirectives(tree))
			{
				if (directive.scope != path || directive.target.empty())
				{
					continue;
				}

				const auto target =
					ResolveWrittenScope(scope_paths, external, directive.scope, directive.target);
				if (target == path)
				{
					continue;
				}

				FlatHashMap<CompletionItem> nominated;
				nominated.Reserve(64);
				CollectQualifiedMembers(tree, source, tokens, scope_paths, target, prefix, external,
					interner, nominated);
				nominated.ForEach([&](std::uint32_t, const CompletionItem& item)
					{
						InsertItem(interner, best, item);
					});
				CollectUsingMembers(tree, source, tokens, scope_paths, target, prefix, external,
					interner, best, depth + 1);
			}

			for (const auto& declaration : UsingDeclarationsOf(tree, scope_paths))
			{
				if (declaration.scope != path ||!MatchesPrefix(declaration.name, prefix))
				{
					continue;
				}

				const auto target =
					declaration.global
				? declaration.qualifier
				: ResolveWrittenScope(scope_paths, external, declaration.scope,
					declaration.qualifier);
				FlatHashMap<CompletionItem> named;
				named.Reserve(16);
				if (target.empty())
				{
					for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
					{
						if (tree.NodesSoA().Kind(n) != GrammarKind::DeclaredName ||
							LookupScope(tree, n) != ParseTree::RootNode ||
							tree.Text(tree.Tokens()[tree.NodesSoA().FirstToken(n)]) !=
							declaration.name)
						{
							continue;
						}

						InsertItem(interner, named,
							DescribeDeclared(tree, source, tokens, n, declaration.name,
							ClassifyDeclaredName(tree, n)));
					}
				}
				else
				{
					CollectQualifiedMembers(tree, source, tokens, scope_paths, target,
						declaration.name, external, interner, named);
				}

				named.ForEach([&](std::uint32_t, const CompletionItem& item)
					{
						if (item.label == declaration.name)
						{
							InsertItem(interner, best, item);
						}
					});
			}
		}

		// A qualifier written inside a scope also resolves against its enclosing
		// scopes (`meta::detail::` inside `namespace fr`). Candidates run from the
		// written qualifier to progressively shorter enclosing prefixes.
		std::vector<std::vector<std::string>> QualifierCandidates(
			const ParseTree& tree,
			const std::vector<std::vector<std::string>>& scope_paths,
			std::size_t offset,
			const std::vector<std::string>& qualifier)
		{
			std::vector<std::vector<std::string>> out
			{
				qualifier
			};
			std::vector<std::string> enclosing;
			for (std::size_t n = 0; n < scope_paths.size(); ++n)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(n);
				if (kind != GrammarKind::NamespaceDefinition &&
					kind != GrammarKind::RecordDefinition)
				{
					continue;
				}

				const auto[start, end] = NodeRange(tree, n);
				if (start <= offset && offset <= end && scope_paths[n].size() > enclosing.size())
				{
					enclosing = scope_paths[n];
				}
			}

			for (std::size_t length = enclosing.size(); length > 0; --length)
			{
				std::vector<std::string> candidate(
					enclosing.begin(),
					enclosing.begin() + static_cast<std::ptrdiff_t>(length));
				candidate.insert(candidate.end(), qualifier.begin(), qualifier.end());
				if (candidate != qualifier)
				{
					out.push_back(std::move(candidate));
				}
			}

			return out;
		}

		void CompleteQualified(
			const ParseTree& tree,
			std::string_view source,
			const std::vector<Token>& tokens,
			const std::vector<std::vector<std::string>>& scope_paths,
			const std::vector<CallableInterval>& callables,
			const std::vector<ScopeInterval>& named_scopes,
			std::size_t offset,
			std::string_view prefix,
			const ScopeIndex* external,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best)
		{
			const std::size_t scope_op = AccessOperatorBefore(source, tokens, offset, prefix);
			if (scope_op >= tokens.size() || TokenText(source, tokens[scope_op]) != "::")
			{
				return;
			}

			const Qualifier qualifier = QualifierBefore(source, tokens, scope_op);
			if (qualifier.unresolved)
			{
				return;
			} // `A<int>::x`, `call()::y`: no guesses

			if (qualifier.global)
			{
				// Leading `::` names the global scope: no keywords, builtins, macros
				// or function locals, and no record members.
				for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
				{
					if (tree.NodesSoA().Kind(n) != GrammarKind::DeclaredName)
					{
						continue;
					}

					if (MemberScope(tree, n) != ParseTree::RootNode)
					{
						continue;
					}

					const std::size_t token_index = tree.NodesSoA().FirstToken(n);
					if (token_index >= tree.Tokens().size())
					{
						continue;
					}

					const std::string_view name = tree.Text(tree.Tokens()[token_index]);
					if (!MatchesPrefix(name, prefix))
					{
						continue;
					}

					const CompletionKind kind = ClassifyDeclaredName(tree, n);
					if (IsKeyword(name) && kind == CompletionKind::Variable)
					{
						continue;
					}

					InsertItem(interner, best,
						DescribeDeclared(tree, source, tokens, n, name, kind));
				}

				std::unordered_set<std::string_view> seen_global;
				seen_global.reserve(tokens.size());
				for (const auto& token : tokens)
				{
					if (token.kind != TokenKind::Identifier)
					{
						continue;
					}

					const std::string_view text = TokenText(source, token);
					if (!MatchesPrefix(text, prefix) || IsKeyword(text))
					{
						continue;
					}

					if (!seen_global.insert(text).second)
					{
						continue;
					}

					if (FindCallable(callables, token.offset) != NoIndex)
					{
						continue;
					}

					if (InNamedScopeIntervals(named_scopes, token.offset))
					{
						continue;
					}

					InsertItem(interner, best,
						{std::string(text), CompletionKind::Variable, "variable", {}});
				}

				CollectChildScopes(tree, source, tokens, scope_paths, {}, interner, best, prefix);
				if (external != nullptr)
				{
					for (const auto& scope : *external)
					{
						if (scope.path.empty())
						{
							for (const auto& member : scope.members)
							{
								if (!MatchesItem(member, prefix))
								{
									continue;
								}

								InsertItem(interner, best, member);
							}
						}
						else if (scope.path.size() == 1)
						{
							if (!MatchesPrefix(scope.path.front(), prefix))
							{
								continue;
							}

							InsertItem(
								interner, best,
								{scope.path.front(), scope.kind, KindDetail(scope.kind), {}});
						}
					}
				}

				return;
			}

			// `ns::` lists the members of ns, plus what its using-directives nominate
			// and what its using-declarations bring in.
			const std::vector<std::string> expanded_qualifier = ExpandNamespaceAlias(
				CollectNamespaceAliases(tree), qualifier.path, tokens[scope_op].offset);
			for (const auto& candidate : QualifierCandidates(tree, scope_paths, offset, expanded_qualifier))
			{
				const std::size_t before = best.Size();
				if (!CollectQualifiedMembers(tree, source, tokens, scope_paths, candidate,
					prefix, external, interner, best))
				{
					continue;
				}

				CollectUsingMembers(tree, source, tokens, scope_paths, candidate, prefix,
					external, interner, best, 0);
				if (best.Size() != before)
				{
					break;
				}
			}
		}

		// Members of the scope named by `path` (namespace or record), from the tree
		// and the header index. False when nothing knows the scope.
		bool CollectQualifiedMembers(
			const ParseTree& tree,
			std::string_view source,
			const std::vector<Token>& tokens,
			const std::vector<std::vector<std::string>>& scope_paths,
			const std::vector<std::string>& path,
			std::string_view prefix,
			const ScopeIndex* external,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best)
		{
			const std::vector<std::size_t> targets = ResolveScope(scope_paths, path);
			// External (header) scopes matching the qualifier union with local ones.
			// An empty target set with no external match means an unknown qualifier.
			bool saw_external = false;
			if (external != nullptr)
			{
				for (const auto& scope : *external)
				{
					if (scope.path != path)
					{
						continue;
					}

					saw_external = true;
					for (const auto& member : scope.members)
					{
						if (!OfferMember(member, prefix) ||!MatchesItem(member, prefix))
						{
							continue;
						}

						InsertItem(interner, best, member);
					}
				}

				// External nested scopes below the qualifier (`std::` offers `chrono`
				// from a `std::chrono` header scope).
				for (const auto& scope : *external)
				{
					if (scope.path.size() <= path.size())
					{
						continue;
					}

					bool matches = true;
					for (std::size_t i = 0; i < path.size(); ++i)
					{
						if (scope.path[i] != path[i])
						{
							matches = false;
							break;
						}
					}

					if (!matches)
					{
						continue;
					}

					saw_external            = true;
					const std::string& name = scope.path[path.size()];
					if (!MatchesPrefix(name, prefix))
					{
						continue;
					}

					InsertItem(interner, best, {name, scope.kind, KindDetail(scope.kind), {}});
				}
			}

			if (targets.empty() && !saw_external)
			{
				return false;
			} // unknown qualifier (`std::`): no guesses

			auto is_target = [&](std::size_t node)
			{
				for (const auto target : targets)
				{
					if (node == target)
					{
						return true;
					}
				}

				return false;
			};
			for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
			{
				if (tree.NodesSoA().Kind(n) != GrammarKind::DeclaredName)
				{
					continue;
				}

				if (!is_target(LookupScope(tree, n)))
				{
					continue;
				}

				const std::size_t token_index = tree.NodesSoA().FirstToken(n);
				if (token_index >= tree.Tokens().size())
				{
					continue;
				}

				const std::string_view name = tree.Text(tree.Tokens()[token_index]);
				if (!MatchesPrefix(name, prefix))
				{
					continue;
				}

				// `operator->` and friends are indexed under their full spelling;
				// the raw `operator` name is never offered.
				if (name == "operator")
				{
					continue;
				}

				const CompletionKind kind = ClassifyDeclaredName(tree, n);
				if (IsKeyword(name) && kind == CompletionKind::Variable)
				{
					continue;
				}

				InsertItem(interner, best, DescribeDeclared(tree, source, tokens, n, name, kind));
			}

			// Nested scopes (`struct Inner` / `namespace inner`) owned by a target.
			for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(n);
				if (kind != GrammarKind::NamespaceDefinition &&
					kind != GrammarKind::RecordDefinition)
				{
					continue;
				}

				if (!is_target(MemberScope(tree, n)))
				{
					continue;
				}

				const auto elements = ScopeNameElements(tree, n);
				if (elements.empty() ||!MatchesPrefix(elements.back(), prefix))
				{
					continue;
				}

				std::vector<std::string> full_path = ScopePath(tree, n);
				if (full_path.empty())
				{
					continue;
				}

				InsertItem(interner, best, MakeNamespaceItem(tree, source, tokens, n, full_path));
			}

			for (const auto target : targets)
			{
				const auto[start, end] = NodeRange(tree, target);
				CollectTagNamesIn(source, tokens, interner, best, prefix, start, end);
			}

			// Members introduced by deeper compound definitions (`ns::a::b` makes `a`
			// visible under `ns::` even without an intermediate node).
			CollectChildScopes(tree, source, tokens, scope_paths, path, interner, best,
				prefix);
			return true;
		}

		// --- Member access (`obj.` / `ptr->`) -----------------------------------
		// The receiver is parsed backwards from the operator into a chain of
		// segments (`a.b().c[0]->`), each resolved to a record path through the
		// variable/field/function declarations the tree and header index know.
		// Anything the engine cannot type (dependent types, lambdas, operators)
		// yields no items rather than a guess.

		struct TypeName
		{
			std::vector<std::string> path;
			std::vector<std::string> args; // top-level template arguments, as written
			bool ok = false;
		};

		bool IsTypeWord(std::string_view word)
		{
			static constexpr std::string_view words[] = {
				"const", "volatile", "static", "constexpr", "consteval", "constinit",
				"inline", "extern", "mutable", "register", "thread_local", "typename",
				"struct", "class", "union", "enum", "virtual", "friend",
				"explicit", "typedef", "signed", "unsigned", "this"
			};
			return std::find(std::begin(words), std::end(words), word) != std::end(words);
		}

		bool IsTrivia(TokenKind kind)
		{
			return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
				kind == TokenKind::BlockComment;
		}

		// Splits the template argument list that opens at `open` (a `<` token) into
		// top-level arguments. Returns the index of the closing token (or tokens.size()).
		std::size_t SplitTemplateArgs(
			std::string_view text,
			const std::vector<Token>& toks,
			std::size_t open,
			std::vector<std::string>& args)
		{
			int angle             = 0;
			int nest              = 0;
			std::size_t arg_start = toks[open].offset + toks[open].length;
			for (std::size_t k = open; k < toks.size(); ++k)
			{
				if (toks[k].kind != TokenKind::Punctuation)
				{
					continue;
				}

				const std::string_view p = text.substr(toks[k].offset, toks[k].length);
				if (p == "(" || p == "[" || p == "{")
				{
					++nest;
				}
				else if (p == ")" || p == "]" || p == "}")
				{
					--nest;
				}
				else if (nest == 0 && p == "<")
				{
					++angle;
				}
				else if (nest == 0 && (p == ">" || p == ">>"))
				{
					angle -= p == ">" ? 1 : kDoubleAngleCount;
					if (angle <= 0)
					{
						const std::size_t end = toks[k].offset +(p == ">>" && angle == 0 ? 1 : 0);
						if (end > arg_start)
						{
							args.push_back(
								CompactWs(std::string(text.substr(arg_start, end - arg_start)),
								kMaxTypeTextLen));
						}

						return k;
					}
				}
				else if (nest == 0 && angle == 1 && p == ",")
				{
					args.push_back(
						CompactWs(std::string(text.substr(arg_start, toks[k].offset - arg_start)),
						kMaxTypeTextLen));
					arg_start = toks[k].offset + toks[k].length;
				}
			}

			return toks.size();
		}

		// `const std::vector<int> &` -> {std, vector}, args {int}.
		TypeName ParseTypeName(std::string_view text)
		{
			TypeName result;
			const std::vector<Token> toks = Lexer(text).Lex();
			auto sig = [&](std::size_t k)
			{
				while (k < toks.size() && IsTrivia(toks[k].kind))
				{
					++k;
				}

				return k;
			};
			auto txt = [&](std::size_t k)
			{
				return text.substr(toks[k].offset, toks[k].length);
			};
			std::size_t i = sig(0);
			while (i < toks.size() && toks[i].kind == TokenKind::Identifier && IsTypeWord(txt(i)))
			{
				i = sig(i + 1);
			}

			if (i < toks.size() && toks[i].kind == TokenKind::Punctuation && txt(i) == "::")
			{
				i = sig(i + 1);
			}

			while (i < toks.size() && toks[i].kind == TokenKind::Identifier)
			{
				result.path.emplace_back(txt(i));
				result.args.clear();
				i = sig(i + 1);
				if (i < toks.size() && toks[i].kind == TokenKind::Punctuation && txt(i) == "<")
				{
					const std::size_t close = SplitTemplateArgs(text, toks, i, result.args);
					if (close >= toks.size())
					{
						return {};
					}

					i = sig(close + 1);
				}

				if (i < toks.size() && toks[i].kind == TokenKind::Punctuation && txt(i) == "::")
				{
					i = sig(i + 1);
					continue;
				}

				break;
			}

			result.ok = !result.path.empty();
			return result;
		}

		std::string PathKey(const std::vector<std::string>& path)
		{
			std::string key;
			for (const auto& element : path)
			{
				key += element;
				key += '\0';
			}

			return key;
		}

		std::vector<std::string> Join(
			const std::vector<std::string>& prefix,
			const std::vector<std::string>& rest)
		{
			std::vector<std::string> out = prefix;
			out.insert(out.end(), rest.begin(), rest.end());
			return out;
		}

		// Wrappers whose `->` reaches the first template argument.
		bool IsPointerLike(std::string_view name)
		{
			static constexpr std::string_view names[] = {
				"unique_ptr", "shared_ptr", "optional", "expected", "auto_ptr",
				"scoped_ptr", "intrusive_ptr", "weak_ptr_lock", "reference_wrapper"
			};
			return std::find(std::begin(names), std::end(names), name) != std::end(names);
		}

		// Containers whose `[]` yields the first / second template argument.
		bool SubscriptYieldsSecond(std::string_view name)
		{
			return name == "map" || name == "unordered_map" || name == "flat_map";
		}

		struct ChainSegment
		{
			std::vector<std::string> qualifier;
			std::string name;
			std::vector<std::string> targs;
			std::string post; // '(' call/construct, '[' subscript, in source order
			std::size_t callOpen  = NoIndex;
			std::size_t callClose = NoIndex;
		};

		struct Chain
		{
			std::vector<ChainSegment> segments;
			std::vector<std::string> ops; // ops[i] follows segments[i]
			bool ok                = false;
			bool new_expression    = false;
			std::size_t head_token = 0;
		};

		struct Resolved
		{
			std::vector<std::string> path;
			std::vector<std::string> args;
			// Scope the type was written in: `args` are spelled relative to it.
			std::vector<std::string> hint;
			// The type as written, when it came from a declaration (`const T&`).
			std::string text;
			bool ok = false;
			// Initializer was `new T...`: the variable holds a `T*` (hover only).
			bool from_new = false;
		};

		struct MemberFilter
		{
			// The object is const: only const member functions are callable.
			bool const_receiver = false;
			// Unqualified lookup inside a class also finds its nested types.
			bool include_types = false;
			// Hover names a member that exists, accessible or not.
			bool ignore_access = false;
		};

		class MemberResolver
		{
		public:
			MemberResolver(const ParseTree& tree, const ScopeIndex* external, std::size_t offset) :
			m_tree(tree), m_source(tree.Source()), m_tokens(tree.Tokens()), m_offset(offset)
			{
				m_aliases = CollectNamespaceAliases(tree);
				m_local   = BuildScopeIndex(tree, false);
				for (const auto& scope : m_local)
				{
					m_by_path[PathKey(scope.path)].push_back(&scope);
				}

				if (external != nullptr)
				{
					for (const auto& scope : *external)
					{
						m_by_path[PathKey(scope.path)].push_back(&scope);
					}
				}

				ComputeCursorScopes();
				for (const auto& directive : Navigation::UsingDirectives(tree))
				{
					if (directive.active_begin <= offset && offset <= directive.active_end)
					{
						m_using.push_back(directive.target);
					}
				}
			}

			// Parses the receiver of the access operator at token `op`.
			Chain ParseReceiver(std::size_t op) const
			{
				Chain chain;
				std::string op_text(TokenText(m_source, m_tokens[op]));
				std::vector<ChainSegment> reversed;
				std::vector<std::string> ops_reversed {op_text};
				std::size_t pos = PreviousSignificant(m_tokens, op, m_source);
				// `(expr).m` / `(*p)->m`: the receiver starts with a parenthesized
				// group, parsed as a chain of its own and spliced in front.
				Chain group;
				bool grouped = false;
				while (true)
				{
					ChainSegment segment;
					std::string post_reversed;
					std::size_t group_open  = m_tokens.size();
					std::size_t group_close = m_tokens.size();
					while (pos < m_tokens.size() && m_tokens[pos].kind == TokenKind::Punctuation)
					{
						const std::string_view p = TokenText(m_source, m_tokens[pos]);
						if (p != ")" && p != "]" && p != "}")
						{
							break;
						}

						const std::size_t open = MatchBackward(pos);
						if (open >= m_tokens.size())
						{
							return chain;
						}

						post_reversed += p == "]" ? '[' : '(';
						group_open  = open;
						group_close = pos;
						if (p == ")")
						{
							segment.callOpen  = open;
							segment.callClose = pos;
						}

						pos = PreviousSignificant(m_tokens, open, m_source);
						// `f<T>(...)`: keep the template arguments.
						if (post_reversed.back() == '(' && pos < m_tokens.size() &&
							m_tokens[pos].kind == TokenKind::Punctuation &&
							(TokenText(m_source, m_tokens[pos]) == ">" ||
							TokenText(m_source, m_tokens[pos]) == ">>"))
						{
							const std::size_t lt = MatchAngleBackward(pos);
							if (lt >= m_tokens.size())
							{
								return chain;
							}

							SplitTemplateArgsInSource(lt, segment.targs);
							pos = PreviousSignificant(m_tokens, lt, m_source);
						}
					}

					if (pos >= m_tokens.size() || m_tokens[pos].kind != TokenKind::Identifier)
					{
						if (post_reversed != "(" || group_open >= m_tokens.size() ||
							!IsPunct(group_close, ")"))
						{
							return chain;
						}

						group = ParseReceiver(group_close);
						if (!group.ok || group.segments.empty())
						{
							return chain;
						}

						// `((T*)p)`: a C-style cast, taken as one only when `T` names a type
						// the index knows (`(a)*p` with a variable `a` is a product).
						const std::size_t cast_close =
							PreviousSignificant(m_tokens, group.head_token, m_source);
						if (cast_close < m_tokens.size() && IsPunct(cast_close, ")"))
						{
							const std::size_t cast_open = MatchBackward(cast_close);
							if (cast_open >= m_tokens.size() ||
								PreviousSignificant(m_tokens, cast_open, m_source) != group_open)
							{
								return chain;
							}

							const std::size_t first = NextSignificant(cast_open);
							const std::size_t last =
								PreviousSignificant(m_tokens, cast_close, m_source);
							if (first >= cast_close || last >= m_tokens.size() || last < first)
							{
								return chain;
							}

							const std::string type_text(
								m_source.substr(
								m_tokens[first].offset,
								m_tokens[last].offset + m_tokens[last].length -
								m_tokens[first].offset));
							const TypeName type = ParseTypeName(type_text);
							const bool variable = type.ok && type.path.size() == 1 &&
								ResolveVariable(type.path.front(), 0).ok;
							if (!type.ok || variable ||!ResolveType(type, m_hint, 0).ok)
							{
								return chain;
							}

							ChainSegment cast;
							cast.name        = "static_cast";
							cast.targs       = {type_text};
							cast.post        = "(";
							group.segments   = {std::move(cast)};
							group.ops        = {")"};
							group.head_token = cast_open;
						}

						// The group must be exactly `(` [`*`] chain `)`.
						std::size_t head =
							PreviousSignificant(m_tokens, group.head_token, m_source);
						const bool star = head < m_tokens.size() && IsPunct(head, "*");
						if (star)
						{
							head = PreviousSignificant(m_tokens, head, m_source);
						}

						if (head != group_open)
						{
							return chain;
						}

						if (star)
						{
							group.ops.back() = "->"; // `(*p)`: through the pointer-like
						}

						chain.head_token = group_open;
						grouped          = true;
						break;
					}

					segment.name = std::string(TokenText(m_source, m_tokens[pos]));
					segment.post.assign(post_reversed.rbegin(), post_reversed.rend());
					chain.head_token   = pos;
					std::size_t before = PreviousSignificant(m_tokens, pos, m_source);
					while (before < m_tokens.size() && IsPunct(before, "::"))
					{
						const std::size_t name = PreviousSignificant(m_tokens, before, m_source);
						if (name >= m_tokens.size() || m_tokens[name].kind != TokenKind::Identifier)
						{
							break; // leading `::`
						}

						segment.qualifier.insert(segment.qualifier.begin(),
							std::string(TokenText(m_source, m_tokens[name])));
						chain.head_token = name;
						before           = PreviousSignificant(m_tokens, name, m_source);
					}

					reversed.push_back(std::move(segment));
					if (before < m_tokens.size() && (IsPunct(before, ".") || IsPunct(before, "->")))
					{
						ops_reversed.push_back(std::string(TokenText(m_source, m_tokens[before])));
						pos = PreviousSignificant(m_tokens, before, m_source);
						continue;
					}

					if (before < m_tokens.size() &&
						m_tokens[before].kind == TokenKind::Identifier &&
						TokenText(m_source, m_tokens[before]) == "new")
					{
						chain.new_expression = true;
						chain.head_token     = before;
					}

					break;
				}

				chain.segments.assign(reversed.rbegin(), reversed.rend());
				chain.ops.assign(ops_reversed.rbegin(), ops_reversed.rend());
				if (grouped)
				{
					// The group's last segment is followed by the operator that follows
					// the group, unless a `*` already forced a dereference.
					const std::string following = chain.ops.front();
					group.ops.back()            = group.ops.back() == "->" ? "->" : following;
					chain.segments.insert(chain.segments.begin(), group.segments.begin(),
						group.segments.end());
					chain.ops.erase(chain.ops.begin());
					chain.ops.insert(chain.ops.begin(), group.ops.begin(), group.ops.end());
				}

				chain.ok = true;
				return chain;
			}

			// Type spelled for the `auto` variable named at token `name`: the declared
			// type of the last call or member of its initializer chain
			// (`= a.b().c()`), or the name of a type it constructs. Empty when unknown.
			std::string DeducedTypeText(std::size_t name) const
			{
				const auto rangeType = RangeBindingType(name);
				if (!rangeType.empty())
				{
					return rangeType;
				}

				const Resolved value = ResolveInitializerAfter(name, 0);
				if (!value.text.empty())
				{
					return value.text;
				}

				if (!value.ok)
				{
					return {};
				}

				std::string written;
				for (const auto& part : value.path)
				{
					written += written.empty() ? part : "::" + part;
				}

				if (!value.args.empty())
				{
					written += "<";
					for (std::size_t i = 0; i < value.args.size(); ++i)
					{
						written +=(i == 0 ? "" : ", ") + value.args[i];
					}

					written += ">";
				}

				if (value.from_new)
				{
					written += "*";
				}

				return written;
			}

			void RefineFunctionAt(std::size_t endOffset, CompletionItem& item) const
			{
				if (!item.deduction)
				{
					return;
				}

				const auto name = TokenBefore(m_tokens, endOffset);
				if (name >= m_tokens.size())
				{
					return;
				}

				ChainSegment segment;
				segment.name = item.label;
				auto open    = NextSignificant(name);
				if (open < m_tokens.size() && IsPunct(open, "<"))
				{
					const auto close = SplitTemplateArgs(m_source, m_tokens, open, segment.targs);
					open             = close < m_tokens.size() ? NextSignificant(close) : m_tokens.size();
				}

				if (open >= m_tokens.size() ||!IsPunct(open, "("))
				{
					return;
				}

				std::size_t nesting = 1;
				auto close          = open + 1;
				for (; close < m_tokens.size(); ++close)
				{
					if (IsPunct(close, "("))
					{
						++nesting;
					}
					else if (IsPunct(close, ")") && --nesting == 0)
					{
						break;
					}
				}

				segment.callOpen  = open;
				segment.callClose = close;
				for (const auto& [key, scopes] : m_by_path)
				{
					for (const auto* scope : scopes)
					{
						const auto found = std::find_if(scope->members.begin(), scope->members.end(),
							[&](const auto& member)
							{
								return member.file == item.file && member.offset == item.offset && member.label == item.label;
							});
						if (found == scope->members.end())
						{
							continue;
						}

						const auto overloads = FunctionsNamed(scope->path, item.label, false);
						if (std::any_of(overloads.begin(), overloads.end(),[&](const auto& candidate)
							{
								return candidate.file != item.file || candidate.offset != item.offset;
							}))
						{
							return;
						}

						const auto type = DeducedReturn(item, segment, scope->path, 0);
						if (type.empty())
						{
							return;
						}

						const auto at = item.detail.find(item.type_text);
						if (at != std::string::npos)
						{
							item.detail.replace(at, item.type_text.size(), type);
						}

						item.type_text = type;
						return;
					}
				}
			}

			std::string VariableTypeText(std::size_t token) const
			{
				if (token >= m_tokens.size())
				{
					return {};
				}

				const auto value = ResolveVariable(std::string(TokenText(m_source, m_tokens[token])), 0);
				auto written     = value.text;
				if (written.empty() && value.ok)
				{
					for (const auto& part : value.path)
					{
						written += written.empty() ? part : "::" + part;
					}

					if (!value.args.empty())
					{
						written += '<';
						for (std::size_t i = 0; i < value.args.size(); ++i)
						{
							written +=(i == 0 ? "" : ", ") + value.args[i];
						}

						written += '>';
					}
				}

				auto hint                            = value.hint.empty() ? m_hint : value.hint;
				constexpr std::size_t kMaxAliasDepth = 8;
				for (std::size_t depth = 0; depth < kMaxAliasDepth; ++depth)
				{
					const auto type = ParseTypeName(written);
					if (!type.ok)
					{
						return written;
					}

					Found alias;
					for (const auto& prefix : Prefixes(hint))
					{
						const auto full = Join(prefix, type.path);
						alias           = FindAlias(Parent(full), full.back());
						if (alias.item)
						{
							break;
						}
					}

					if (!alias.item || alias.item->templateParams.size() != type.args.size())
					{
						return written;
					}

					written = SubstituteText(alias.item->type_text, alias.item->templateParams, type.args);
					hint    = alias.owner;
				}

				return {};
			}

			[[nodiscard]] Resolved CompletionValueType(const CompletionItem& item) const
			{
				if (item.kind == CompletionKind::Variable && item.file < 0)
				{
					const auto value = ResolveVariable(item.label, 0);
					if (value.ok ||!value.text.empty())
					{
						return value;
					}
				}

				for (const auto& [key, scopes] : m_by_path)
				{
					for (const auto* scope : scopes)
					{
						for (const auto& member : scope->members)
						{
							if (member.label == item.label && member.file == item.file &&
								member.offset == item.offset && member.kind == item.kind)
							{
								if (item.deduction && item.deduction->parameterTypes.empty())
								{
									std::vector<const IndexedScope*> allScopes;
									for (const auto& [scopeKey, entries] : m_by_path)
									{
										allScopes.insert(allScopes.end(), entries.begin(), entries.end());
									}

									const auto type = TemplateDeduction::Instantiate(*item.deduction,
										item.templateParams, {}, {}, allScopes, scope->path, false);
									return FromText(type, scope->path, 0);
								}

								return FromText(item.type_text, scope->path, 0);
							}
						}
					}
				}

				return FromText(item.type_text, m_hint, 0);
			}

			[[nodiscard]] bool HasImplicitReflectionConversion(const Resolved& value) const
			{
				constexpr std::string_view kConversionPrefix = "operator ";
				if (!value.ok || HasReflectionIndirection(value))
				{
					return false;
				}

				for (const auto* scope : ScopesAt(value.path))
				{
					for (const auto& member : scope->members)
					{
						if (!member.label.starts_with(kConversionPrefix) || member.deleted ||
							member.access != MemberAccess::Public || member.detail.find("explicit") != std::string::npos)
						{
							continue;
						}

						const auto target = FromText(std::string_view(member.label).substr(kConversionPrefix.size()),
							scope->path, 0);
						if (target.path == std::vector<std::string> {"std", "meta", "info"} &&
							!HasReflectionIndirection(target))
						{
							return true;
						}
					}
				}

				return false;
			}

			[[nodiscard]] bool HasReflectionIndirection(const Resolved& value) const
			{
				auto text                            = value.text;
				auto hint                            = value.hint.empty() ? m_hint : value.hint;
				constexpr std::size_t kMaxAliasDepth = 8;
				for (std::size_t depth = 0; depth < kMaxAliasDepth; ++depth)
				{
					if (text.find('*') != std::string::npos || text.find('[') != std::string::npos)
					{
						return true;
					}

					const auto type = ParseTypeName(text);
					if (!type.ok)
					{
						return false;
					}

					Found alias;
					for (const auto& prefix : Prefixes(hint))
					{
						const auto full = Join(prefix, type.path);
						alias           = FindAlias(Parent(full), full.back());
						if (alias.item)
						{
							break;
						}
					}

					if (!alias.item)
					{
						return false;
					}

					text = SubstituteText(alias.item->type_text, alias.item->templateParams, type.args);
					hint = alias.owner;
				}

				return true;
			}

			[[nodiscard]] bool IsReflectionNamespace(std::string_view name) const
			{
				const std::vector<std::string> metaPath
				{
					"std", "meta"
				};
				for (const auto& [key, scopes] : m_by_path)
				{
					for (const auto* scope : scopes)
					{
						if (scope->kind != CompletionKind::Namespace || scope->path.empty() || scope->path.back() != name)
						{
							continue;
						}

						if (scope->path.size() <= metaPath.size() &&
							std::equal(scope->path.begin(), scope->path.end(), metaPath.begin()))
						{
							return true;
						}

						for (const auto& member : scope->members)
						{
							const auto value = FromText(member.type_text, scope->path, 0);
							if ((member.kind == CompletionKind::Variable || member.kind == CompletionKind::Function) &&
								value.path == std::vector<std::string> {"std", "meta", "info"} && !HasReflectionIndirection(value))
							{
								return true;
							}
						}
					}
				}

				return false;
			}

			static std::string SubstituteText(
				std::string_view text,
				const std::vector<std::string>& parameters,
				const std::vector<std::string>& arguments)
			{
				std::string result;
				for (std::size_t begin = 0; begin < text.size();)
				{
					if (!IsIdentStart(text[begin]))
					{
						result += text[begin++];
						continue;
					}

					auto end = begin + 1;
					while (end < text.size() && IsIdentChar(text[end]))
					{
						++end;
					}

					const auto word      = text.substr(begin, end - begin);
					const auto parameter = std::find(parameters.begin(), parameters.end(), word);
					const auto index     = static_cast<std::size_t>(parameter - parameters.begin());
					result += index < arguments.size() ? std::string_view(arguments[index]) : word;
					begin = end;
				}

				return result;
			}

			std::string RangeBindingType(std::size_t name, int depth = 0) const
			{
				constexpr int kMaxRangeDepth = 6;
				if (name >= m_tokens.size() || depth > kMaxRangeDepth)
				{
					return {};
				}

				const auto& nodes = m_tree.NodesSoA();
				for (std::size_t node = 0; node < nodes.size(); ++node)
				{
					if (nodes.Kind(node) != GrammarKind::DeclarationStatement ||
						name < nodes.FirstToken(node) ||
						name >= nodes.FirstToken(node) + nodes.TokenCount(node))
					{
						continue;
					}

					auto colon               = NextSignificant(name);
					bool structured          = false;
					std::size_t bindingIndex = 0;
					bool reference           = false;
					bool constant            = false;
					for (auto token = nodes.FirstToken(node); token < name; ++token)
					{
						structured |= m_tokens[token].tok == Tok::LBracket;
						reference |= m_tokens[token].tok == Tok::Amp || m_tokens[token].tok == Tok::AmpAmp;
						constant |= m_tokens[token].tok == Tok::KwConst;
						if (structured && m_tokens[token].tok == Tok::Comma)
						{
							++bindingIndex;
						}
					}

					if (structured)
					{
						while (colon < m_tokens.size() && m_tokens[colon].tok != Tok::RBracket)
						{
							colon = NextSignificant(colon);
						}

						colon = NextSignificant(colon);
					}

					if (colon >= m_tokens.size() || m_tokens[colon].tok != Tok::Colon)
					{
						return {};
					}

					auto end          = NextSignificant(colon);
					std::size_t depth = 0;
					for (; end < m_tokens.size(); ++end)
					{
						if (m_tokens[end].tok == Tok::LParen)
						{
							++depth;
						}
						else if (m_tokens[end].tok == Tok::RParen)
						{
							if (depth == 0)
							{
								break;
							}

							--depth;
						}
					}

					const auto container  = ResolveChain(ParseChainEndingAt(end, colon), depth + 1);
					const auto type       = ParseTypeName(container.text);
					const auto& path      = container.path.empty() ? type.path : container.path;
					const auto& arguments = container.args.empty() ? type.args : container.args;
					if (path.size() < 2 || path.front() != "std" || arguments.empty())
					{
						return {};
					}

					const auto& head = path.back();
					const bool map = head == "map" || head == "unordered_map" ||
						head == "multimap" || head == "unordered_multimap";
					std::string element;
					if (map && arguments.size() >= 2)
					{
						if (structured && bindingIndex < 2)
						{
							element = (bindingIndex == 0 ? "const " : "") + arguments[bindingIndex];
						}
						else if (!structured)
						{
							element = "std::pair<const " + arguments[0] + ", " + arguments[1] + ">";
						}
					}
					else if (!structured && (head == "vector" || head == "array" || head == "span" ||
						head == "deque" || head == "list" || head == "forward_list" || head == "set" ||
						head == "unordered_set" || head == "multiset" || head == "unordered_multiset"))
					{
						element = arguments[0];
						constant |= reference && (head == "set" || head == "unordered_set" ||
							head == "multiset" || head == "unordered_multiset");
					}

					if (element.empty())
					{
						return {};
					}

					if ((constant ||(reference && container.text.starts_with("const "))) &&
						!element.starts_with("const "))
					{
						element = "const " + element;
					}

					return element +(reference && !structured ? "&" : "");
				}

				return {};
			}

			Resolved ResolveChain(const Chain& chain, int depth = 0) const
			{
				constexpr int kMaxChainDepth = 6;
				Resolved current;
				if (!chain.ok || chain.segments.empty() || depth > kMaxChainDepth)
				{
					return current;
				}

				for (std::size_t i = 0; i < chain.segments.size(); ++i)
				{
					const ChainSegment& segment = chain.segments[i];
					Resolved next = i == 0 ? ResolveHead(segment, chain.new_expression, depth)
					: ResolveMember(current, segment, depth);
					if (!next.ok)
					{
						// The last member may be a type the index does not hold
						// (`std::expected<...>` without its header): keep its spelling
						// for hover; `ok` stays false, so nothing is completed from it.
						return i + 1 == chain.segments.size() && !next.text.empty()
						? next
						: Resolved {};
					}

					current = std::move(next);
					current = Dereference(current, chain.ops[i]);
					if (!current.ok)
					{
						return {};
					}
				}

				return current;
			}

			void CollectMembers(
				const std::vector<std::string>& path,
				std::string_view prefix,
				StringInterner& interner,
				FlatHashMap<CompletionItem>& best,
				const MemberFilter& filter = {},
				const std::vector<std::string>& arguments = {}) const
			{
				std::unordered_set<std::string> visited;
				CollectInto(path, prefix, interner, best, visited, 0, filter);
				if (arguments.empty())
				{
					return;
				}

				for (const auto* scope : ScopesAt(path))
				{
					for (const auto& member : scope->members)
					{
						if (member.kind != CompletionKind::Variable ||!MatchesItem(member, prefix))
						{
							continue;
						}

						const auto key = interner.GetOrAssign(member.label);
						if (auto * entry = best.Find(key))
						{
							auto& item       = entry->value;
							item.type_text   = SubstituteText(member.type_text, scope->template_params, arguments);
							item.detail      = SubstituteText(item.detail, scope->template_params, arguments);
							item.layout_type = SubstituteText(item.layout_type, scope->template_params, arguments);
						}
					}
				}
			}

			// Virtual member functions inherited by `path` that it does not override
			// yet, nearest declaration first (`B::f() override` hides `A::f()`).
			[[nodiscard]] std::vector<CompletionItem> Overridable(
				const std::vector<std::string>& path) const
			{
				std::vector<std::string> keys;
				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& member : scope->members)
					{
						if (member.kind == CompletionKind::Function)
						{
							keys.push_back(OverrideKey(member));
						}
					}
				}

				std::vector<CompletionItem> found;
				std::unordered_set<std::string> visited {PathKey(path)};
				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& written : scope->bases)
					{
						const Resolved base = ResolveType(ParseTypeName(written), Parent(path), 0);
						if (base.ok)
						{
							CollectVirtuals(base.path, keys, found, visited, 0);
						}
					}
				}

				return found;
			}

			// Accessible overloads named `name` declared in `path`; when it declares
			// none, those of its bases (a derived declaration hides the base's).
			[[nodiscard]] std::vector<CompletionItem> FunctionsNamed(
				const std::vector<std::string>& path,
				std::string_view name,
				bool walk_bases,
				int depth = 0) const
			{
				constexpr int kMaxFunctionDepth = 16;
				std::vector<CompletionItem> found;
				if (depth > kMaxFunctionDepth)
				{
					return found;
				}

				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& member : scope->members)
					{
						if (member.kind == CompletionKind::Function && member.label == name &&
							CanAccess(member, path))
						{
							found.push_back(member);
						}
					}
				}

				if (!found.empty() ||!walk_bases)
				{
					return found;
				}

				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& written : scope->bases)
					{
						const Resolved base = ResolveType(ParseTypeName(written), Parent(path), 0);
						if (base.ok)
						{
							auto inherited = FunctionsNamed(base.path, name, true, depth + 1);
							found.insert(found.end(), inherited.begin(), inherited.end());
						}
					}
				}

				return found;
			}

			// Functions an unqualified call `name(` can reach: members of the
			// enclosing classes first (they hide outer names), else the enclosing
			// namespaces, the global scope and using-directive targets.
			[[nodiscard]] std::vector<CompletionItem> VisibleFunctions(
				std::string_view name) const
			{
				for (const auto& record : m_records)
				{
					auto members = FunctionsNamed(record, name, true);
					if (!members.empty())
					{
						return members;
					}
				}

				std::vector<CompletionItem> found;
				for (const auto& prefix : Prefixes(m_hint))
				{
					auto declared = FunctionsNamed(prefix, name, false);
					found.insert(found.end(), declared.begin(), declared.end());
				}

				return found;
			}

			// Non-static data members of a record in declaration order (aggregates).
			[[nodiscard]] std::vector<CompletionItem> FieldsInOrder(
				const std::vector<std::string>& path) const
			{
				std::vector<CompletionItem> fields;
				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& member : scope->members)
					{
						if (member.kind == CompletionKind::Variable && member.has_location &&
							!member.layout_type.starts_with("static "))
						{
							fields.push_back(member);
						}
					}
				}

				std::stable_sort(fields.begin(), fields.end(),[](const auto& left, const auto& right)
					{
						return left.offset < right.offset;
					});
				return fields;
			}

			// Record path a type spelled at the cursor names.
			[[nodiscard]] Resolved ResolveTypeText(std::string_view text) const
			{
				return ResolveType(ParseTypeName(text), m_hint, 0);
			}

			// Private members are reachable from their own class, protected ones also
			// from classes deriving from it (friendship is not modeled).
			[[nodiscard]] bool CanAccess(
				const CompletionItem& member,
				const std::vector<std::string>& owner) const
			{
				if (member.access == MemberAccess::Public)
				{
					return true;
				}

				for (const auto& record : m_records)
				{
					if (record == owner)
					{
						return true;
					}

					if (member.access == MemberAccess::Protected && DerivesFrom(record, owner, 0))
					{
						return true;
					}
				}

				return false;
			}

			// Class initialized by the brace list opened at token `open`: `T{`,
			// `new T{`, `T x{` and `T x = {`. `ok` is false for anything else.
			Resolved ResolveBraceOwner(std::size_t open) const
			{
				std::size_t end          = open;
				const std::size_t before = PreviousSignificant(m_tokens, open, m_source);
				if (before < m_tokens.size() && IsPunct(before, "="))
				{
					end = before;
				}

				const Chain chain = ParseReceiver(end);
				if (!chain.ok || chain.segments.empty())
				{
					return {};
				}

				Resolved value = ResolveChain(chain);
				if (value.ok)
				{
					return value;
				}

				const ChainSegment& last = chain.segments.back();
				if (chain.segments.size() != 1 ||!last.post.empty())
				{
					return {};
				}

				TypeName type;
				type.path = Join(last.qualifier, {last.name});
				type.args = last.targs;
				type.ok   = true;
				return ResolveType(type, m_hint, 0);
			}

			// Data members of `path` (designators of an initializer list).
			void CollectFields(
				const std::vector<std::string>& path,
				std::string_view prefix,
				const std::unordered_set<std::string>& used,
				StringInterner& interner,
				FlatHashMap<CompletionItem>& best) const
			{
				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& member : scope->members)
					{
						if (member.kind == CompletionKind::Variable &&
							!used.contains(member.label) && MatchesItem(member, prefix))
						{
							InsertItem(interner, best, WithFieldDetail(member));
						}
					}
				}
			}

			// First token of `a.b.c` style chains, for `auto` deduction.
			const std::vector<std::vector<std::string>>& EnclosingRecords() const
			{
				return m_records;
			}

		private:
			std::size_t NextSignificant(std::size_t token) const
			{
				std::size_t i = token + 1;
				while (i < m_tokens.size() && IsTrivia(m_tokens[i].kind))
				{
					++i;
				}

				return i;
			}

			bool IsPunct(std::size_t token, std::string_view text) const
			{
				return m_tokens[token].kind == TokenKind::Punctuation &&
					TokenText(m_source, m_tokens[token]) == text;
			}

			std::size_t MatchBackward(std::size_t close) const
			{
				int depth = 0;
				for (std::size_t k = close + 1; k > 0; --k)
				{
					const std::size_t i = k - 1;
					if (m_tokens[i].kind != TokenKind::Punctuation)
					{
						continue;
					}

					const std::string_view p = TokenText(m_source, m_tokens[i]);
					if (p == ")" || p == "]" || p == "}")
					{
						++depth;
					}
					else if (p == "(" || p == "[" || p == "{")
					{
						if (--depth == 0)
						{
							return i;
						}
					}
				}

				return m_tokens.size();
			}

			std::size_t MatchAngleBackward(std::size_t close) const
			{
				int depth = 0;
				for (std::size_t k = close + 1; k > 0; --k)
				{
					const std::size_t i = k - 1;
					if (m_tokens[i].kind != TokenKind::Punctuation)
					{
						continue;
					}

					const std::string_view p = TokenText(m_source, m_tokens[i]);
					if (p == ">")
					{
						++depth;
					}
					else if (p == ">>")
					{
						depth += kDoubleAngleCount;
					}
					else if (p == "<")
					{
						if (--depth <= 0)
						{
							return i;
						}
					}
					else if (p == ";" || p == "{" || p == "}")
					{
						break;
					}
				}

				return m_tokens.size();
			}

			void SplitTemplateArgsInSource(std::size_t open, std::vector<std::string>& args) const
			{
				SplitTemplateArgs(m_source, m_tokens, open, args);
			}

			void ComputeCursorScopes()
			{
				std::size_t best_namespace = NoIndex;
				auto best_span             = static_cast<std::size_t>(-1);
				std::vector<std::pair<std::size_t, std::size_t>> records; // (span, node)
				std::size_t best_function = NoIndex;
				auto function_span        = static_cast<std::size_t>(-1);
				for (std::size_t n = 1; n < m_tree.NodesSoA().size(); ++n)
				{
					const GrammarKind kind = m_tree.NodesSoA().Kind(n);
					if (kind != GrammarKind::NamespaceDefinition &&
						kind != GrammarKind::RecordDefinition &&
						kind != GrammarKind::FunctionDefinition)
					{
						continue;
					}

					const auto[start, end] = NodeRange(m_tree, n);
					if (!(start <= m_offset && m_offset <= end) || end <= start)
					{
						continue;
					}

					const std::size_t span = end - start;
					if (kind == GrammarKind::FunctionDefinition)
					{
						if (span < function_span)
						{
							function_span = span;
							best_function = n;
						}
					}
					else if (kind == GrammarKind::NamespaceDefinition)
					{
						if (span < best_span)
						{
							best_span      = span;
							best_namespace = n;
						}
					}
					else
					{
						records.emplace_back(span, n);
					}
				}

				std::sort(records.begin(), records.end());
				// Out-of-line member: `void A::f() { ... }` makes A the implicit class.
				if (best_function != NoIndex)
				{
					const std::size_t declarator =
						FindChild(m_tree, best_function, GrammarKind::Declarator);
					const std::size_t nested =
						declarator == NoIndex
					? NoIndex
					: FindChild(m_tree, declarator, GrammarKind::NestedNameSpecifier);
					if (nested != NoIndex)
					{
						std::vector<std::string> owner;
						const auto grammar = m_tree.NodesSoA()[nested];
						for (std::uint32_t k = 0; k < grammar.GetTokenCount(); ++k)
						{
							const Token& token = m_tree.Tokens()[grammar.GetFirstToken() + k];
							if (token.kind == TokenKind::Identifier)
							{
								owner.emplace_back(m_tree.Text(token));
							}
						}

						if (!owner.empty())
						{
							std::vector<std::string> prefix;
							if (best_namespace != NoIndex)
							{
								prefix = ScopePath(m_tree, best_namespace);
							}

							m_records.push_back(Join(prefix, owner));
						}
					}
				}

				for (const auto& [span, node] : records)
				{
					(void) span;
					m_records.push_back(ScopePath(m_tree, node));
				}

				if (!records.empty())
				{
					m_hint = ScopePath(m_tree, records.front().second);
				}
				else if (!m_records.empty())
				{
					m_hint = m_records.front();
				}
				else if (best_namespace != NoIndex)
				{
					m_hint = ScopePath(m_tree, best_namespace);
				}
			}

			const std::vector<const IndexedScope*>& ScopesAt(
				const std::vector<std::string>& path) const
			{
				static const std::vector<const IndexedScope*> none;
				const std::vector<std::string> expanded =
					ExpandNamespaceAlias(m_aliases, path, m_offset);
				const auto found = m_by_path.find(PathKey(expanded));
				return found == m_by_path.end() ? none : found->second;
			}

			// Prefixes tried when resolving a name written inside `hint`: the hint
			// and each of its ancestors, then the using-directive namespaces.
			std::vector<std::vector<std::string>> Prefixes(
				const std::vector<std::string>& hint) const
			{
				std::vector<std::vector<std::string>> out;
				for (std::size_t k = hint.size() + 1; k > 0; --k)
				{
					out.emplace_back(hint.begin(),
						hint.begin() + static_cast<std::ptrdiff_t>(k - 1));
				}

				for (const auto& target : m_using)
				{
					out.push_back(target);
				}

				return out;
			}

			struct Found
			{
				const CompletionItem* item = nullptr;
				std::vector<std::string> owner; // scope that declares it
			}

			;

			Found FindDeep(
				const std::vector<std::string>& path,
				std::string_view label,
				CompletionKind kind,
				std::unordered_set<std::string>& visited,
				int depth) const
			{
				constexpr int kMaxLookupDepth = 16;
				if (depth > kMaxLookupDepth ||!visited.insert(PathKey(path)).second)
				{
					return {};
				}

				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& member : scope->members)
					{
						if (member.label == label && member.kind == kind)
						{
							return {&member, path};
						}
					}
				}

				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& base : scope->bases)
					{
						const Resolved resolved = ResolveType(ParseTypeName(base), Parent(path), 0);
						if (!resolved.ok)
						{
							continue;
						}

						const Found found =
							FindDeep(resolved.path, label, kind, visited, depth + 1);
						if (found.item != nullptr)
						{
							return found;
						}
					}
				}

				return {};
			}

			static std::vector<std::string> Parent(const std::vector<std::string>& path)
			{
				return path.empty() ? path : std::vector<std::string>(path.begin(), path.end() - 1);
			}

			static std::string OverrideKey(const CompletionItem& function)
			{
				return function.label + ParameterTypesKey(function);
			}

			void CollectVirtuals(
				const std::vector<std::string>& path,
				std::vector<std::string>& keys,
				std::vector<CompletionItem>& found,
				std::unordered_set<std::string>& visited,
				int depth) const
			{
				constexpr int kMaxVirtualDepth = 16;
				if (depth > kMaxVirtualDepth ||!visited.insert(PathKey(path)).second)
				{
					return;
				}

				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& member : scope->members)
					{
						if (member.kind != CompletionKind::Function)
						{
							continue;
						}

						std::string key = OverrideKey(member);
						if (std::find(keys.begin(), keys.end(), key) != keys.end())
						{
							continue;
						}

						keys.push_back(std::move(key));
						if (member.is_virtual)
						{
							found.push_back(member);
						}
					}
				}

				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& written : scope->bases)
					{
						const Resolved base = ResolveType(ParseTypeName(written), Parent(path), 0);
						if (base.ok)
						{
							CollectVirtuals(base.path, keys, found, visited, depth + 1);
						}
					}
				}
			}

			bool DerivesFrom(
				const std::vector<std::string>& path,
				const std::vector<std::string>& base,
				int depth) const
			{
				constexpr int kMaxDeriveDepth = 16;
				if (depth > kMaxDeriveDepth)
				{
					return false;
				}

				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& written : scope->bases)
					{
						const Resolved resolved =
							ResolveType(ParseTypeName(written), Parent(path), 0);
						if (resolved.ok &&
							(resolved.path == base || DerivesFrom(resolved.path, base, depth + 1)))
						{
							return true;
						}
					}
				}

				return false;
			}

			void CollectInto(
				const std::vector<std::string>& path,
				std::string_view prefix,
				StringInterner& interner,
				FlatHashMap<CompletionItem>& best,
				std::unordered_set<std::string>& visited,
				int depth,
				MemberFilter filter) const
			{
				constexpr int kMaxCollectDepth = 16;
				if (depth > kMaxCollectDepth ||!visited.insert(PathKey(path)).second)
				{
					return;
				}

				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& member : scope->members)
					{
						const bool value = member.kind == CompletionKind::Function ||
							member.kind == CompletionKind::Variable;
						const bool type = member.kind == CompletionKind::Type ||
							(member.kind == CompletionKind::Namespace &&
							!member.detail.starts_with("namespace"));
						if (!value && !(filter.include_types && type))
						{
							continue;
						}

						if (!path.empty() && member.label == path.back())
						{
							continue; // constructor
						}

						if (!OfferMember(member, prefix) ||!MatchesItem(member, prefix) ||
							(!filter.ignore_access && !CanAccess(member, path)))
						{
							continue;
						}

						if (filter.const_receiver && member.kind == CompletionKind::Function &&
							!member.is_const)
						{
							continue;
						}

						CompletionItem item = member;
						InsertItem(interner, best, WithFieldDetail(std::move(item)));
					}
				}

				for (const IndexedScope* scope : ScopesAt(path))
				{
					for (const auto& base : scope->bases)
					{
						const Resolved resolved = ResolveType(ParseTypeName(base), Parent(path), 0);
						if (resolved.ok)
						{
							CollectInto(resolved.path, prefix, interner, best, visited, depth + 1,
								filter);
						}
					}
				}
			}

			// Type name -> canonical record path, following alias chains.
			Resolved ResolveType(
				const TypeName& type,
				const std::vector<std::string>& hint,
				int depth) const
			{
				constexpr int kMaxTypeResolveDepth = 8;
				Resolved out;
				out.hint = hint;
				if (!type.ok || depth > kMaxTypeResolveDepth)
				{
					return out;
				}

				for (const auto& prefix : Prefixes(hint))
				{
					const std::vector<std::string> full = Join(prefix, type.path);
					// A scope with members or bases is a record/namespace we can list.
					if (!ScopesAt(full).empty())
					{
						out.path = full;
						out.args = type.args;
						out.ok   = true;
						return out;
					}

					Found alias = FindAlias(Parent(full), full.back());
					if (alias.item != nullptr)
					{
						const TypeName target = ParseTypeName(alias.item->type_text);
						Resolved via          = ResolveType(target, alias.owner, depth + 1);
						if (via.ok)
						{
							if (via.args.empty())
							{
								via.args = type.args;
								via.hint = hint;
							}

							return via;
						}
					}
				}

				return out;
			}

			Found FindAlias(const std::vector<std::string>& scope, std::string_view name) const
			{
				for (const IndexedScope* entry : ScopesAt(scope))
				{
					for (const auto& member : entry->members)
					{
						if (member.label == name && member.kind == CompletionKind::Type &&
							!member.type_text.empty())
						{
							return {&member, scope};
						}
					}
				}

				return {};
			}

			Resolved FromText(
				std::string_view text,
				const std::vector<std::string>& hint,
				int depth) const
			{
				Resolved resolved = ResolveType(ParseTypeName(text), hint, depth);
				resolved.text     = std::string(text);
				return resolved;
			}

			Resolved Dereference(Resolved value, const std::string& op) const
			{
				if (op != "->" || value.path.empty())
				{
					return value;
				}

				if (IsPointerLike(value.path.back()) && !value.args.empty())
				{
					return FromText(value.args.front(), value.hint.empty() ? m_hint : value.hint, 0);
				}

				for (const auto* scope : ScopesAt(value.path))
				{
					for (const auto& member : scope->members)
					{
						if (member.kind != CompletionKind::Function || member.label != "operator->" ||
							member.type_text.empty())
						{
							continue;
						}

						// The substituted pointee (`ThreadPool` for `Arc<ThreadPool>`)
						// is spelled at the use site, not in the wrapper's scope.
						// `operator->` must return a pointer or a proxy, so any
						// resolvable substitution is followed.
						const Resolved pointee = FromText(SubstituteText(member.type_text,
							scope->template_params, value.args),
							value.hint.empty() ? m_hint : value.hint, 0);
						if (pointee.ok)
						{
							return pointee;
						}
					}
				}

				return value;
			}

			// Declared type of the variable `name` as visible at the cursor.
			Resolved ResolveVariable(const std::string& name, int depth) const
			{
				// 1. Locals and parameters: the closest preceding visible declaration.
				std::size_t best          = NoIndex;
				std::uint32_t best_offset = 0;
				for (std::size_t n = 0; n < m_tree.NodesSoA().size(); ++n)
				{
					if (m_tree.NodesSoA().Kind(n) != GrammarKind::DeclaredName)
					{
						continue;
					}

					const std::size_t token_index = m_tree.NodesSoA().FirstToken(n);
					if (token_index >= m_tokens.size() ||
						m_tree.Text(m_tokens[token_index]) != name)
					{
						continue;
					}

					const LocalInfo local = AnalyzeLocal(m_tree, n);
					if (!local.is_local || local.owner == NoIndex)
					{
						continue;
					}

					const auto[block_start, block_end] = NodeRange(m_tree, local.boundary);
					if (m_offset < block_start || m_offset > block_end)
					{
						continue;
					}

					const Token& name_token = m_tokens[token_index];
					if (name_token.offset + name_token.length > m_offset)
					{
						continue;
					}

					if (best == NoIndex || name_token.offset >= best_offset)
					{
						best        = n;
						best_offset = name_token.offset;
					}
				}

				if (best != NoIndex)
				{
					return ResolveLocal(best, depth);
				}

				// 2. Fields of the enclosing classes (including inherited ones).
				for (const auto& record : m_records)
				{
					std::unordered_set<std::string> visited;
					const Found found =
						FindDeep(record, name, CompletionKind::Variable, visited, 0);
					if (found.item != nullptr)
					{
						if (found.item->file < 0 && HasAutoPlaceholder(found.item->type_text))
						{
							const auto token = std::lower_bound(m_tokens.begin(), m_tokens.end(), found.item->offset,
								[](const Token& candidate, std::uint32_t offset)
								{
									return candidate.offset < offset;
								});
							if (token != m_tokens.end() && token->offset == found.item->offset)
							{
								return ResolveInitializerAfter(static_cast<std::size_t>(token - m_tokens.begin()), depth + 1);
							}
						}

						return FromText(found.item->type_text, found.owner, 0);
					}
				}

				// 3. Namespace-level variables (own, enclosing, global, `using namespace`).
				for (const auto& prefix : Prefixes(m_hint))
				{
					std::unordered_set<std::string> visited;
					const Found found =
						FindDeep(prefix, name, CompletionKind::Variable, visited, 0);
					if (found.item != nullptr)
					{
						if (found.item->file < 0 && HasAutoPlaceholder(found.item->type_text))
						{
							const auto token = std::lower_bound(m_tokens.begin(), m_tokens.end(), found.item->offset,
								[](const Token& candidate, std::uint32_t offset)
								{
									return candidate.offset < offset;
								});
							if (token != m_tokens.end() && token->offset == found.item->offset)
							{
								return ResolveInitializerAfter(static_cast<std::size_t>(token - m_tokens.begin()), depth + 1);
							}
						}

						return FromText(found.item->type_text, found.owner, 0);
					}
				}

				return {};
			}

			Resolved ResolveLocal(std::size_t node, int depth) const
			{
				const std::string text = VariableTypeDetail(m_tree, node);
				const TypeName type    = ParseTypeName(text);
				if (type.ok && (type.path.size() != 1 || type.path.front() != "auto"))
				{
					Resolved resolved = ResolveType(type, m_hint, 0);
					resolved.text     = text;
					return resolved;
				}

				const auto rangeType = RangeBindingType(m_tree.NodesSoA().FirstToken(node), depth);
				if (!rangeType.empty())
				{
					return FromText(rangeType, m_hint, depth);
				}

				return ResolveInitializerAfter(m_tree.NodesSoA().FirstToken(node), depth);
			}

			// `auto x = <chain>;` / `auto x{<chain>}`: type the initializer that
			// follows the declared name at token `name`.
			Resolved ResolveInitializerAfter(std::size_t name, int depth) const
			{
				std::size_t t = name + 1;
				while (t < m_tokens.size() && (IsTrivia(m_tokens[t].kind)))
				{
					++t;
				}

				if (t >= m_tokens.size() || m_tokens[t].kind != TokenKind::Punctuation ||
					(TokenText(m_source, m_tokens[t]) != "=" &&
					TokenText(m_source, m_tokens[t]) != "{"))
				{
					return {};
				}

				// Find the end of the initializer, then parse the chain that ends there.
				std::size_t end   = t + 1;
				int nest          = 0;
				int templateDepth = 0;
				for (; end < m_tokens.size(); ++end)
				{
					if (m_tokens[end].kind != TokenKind::Punctuation)
					{
						continue;
					}

					const std::string_view p = TokenText(m_source, m_tokens[end]);
					if (p == "<")
					{
						++templateDepth;
					}
					else if (p == ">" && templateDepth > 0)
					{
						--templateDepth;
					}
					else if (p == ">>" && templateDepth > 0)
					{
						templateDepth = std::max(0, templateDepth - 2);
					}

					if (p == "(" || p == "[" || p == "{")
					{
						++nest;
					}
					else if (p == ")" || p == "]" || p == "}")
					{
						if (--nest < 0)
						{
							break;
						}
					}
					else if ((p == ";" ||(p == "," && templateDepth == 0)) && nest <= 0)
					{
						break;
					}
				}

				if (end >= m_tokens.size() || end <= t + 1)
				{
					return {};
				}

				const auto first = NextSignificant(t);
				if (first < end && TokenText(m_source, m_tokens[first]) == "^^")
				{
					const auto last   = PreviousSignificant(m_tokens, end, m_source);
					const auto& nodes = m_tree.NodesSoA();
					for (std::size_t node = 0; node < nodes.size(); ++node)
					{
						if (nodes.Kind(node) == GrammarKind::UnaryExpression &&
							nodes.FirstToken(node) == first &&
							nodes.FirstToken(node) + nodes.TokenCount(node) == last + 1)
						{
							Resolved reflection;
							reflection.path = {"std", "meta", "info"};
							reflection.text = "std::meta::info";
							reflection.ok   = true;
							return reflection;
						}
					}
				}

				Chain chain = ParseChainEndingAt(end, t);
				if (!chain.ok)
				{
					return {};
				}

				Resolved value = ResolveChain(chain, depth + 1);
				value.from_new = value.ok && chain.new_expression && chain.segments.size() == 1;
				return value;
			}

			// Parses the chain whose last token precedes `end`; it must start right
			// after `floor` (the `=`), or after `new`.
			Chain ParseChainEndingAt(std::size_t end, std::size_t floor) const
			{
				// Reuse the receiver parser: it reads backwards from a pseudo
				// operator token, so point it at the token after the initializer.
				Chain chain = ParseReceiverBefore(end);
				if (!chain.ok)
				{
					return chain;
				}

				const std::size_t before =
					PreviousSignificant(m_tokens, chain.head_token, m_source);
				if (before != floor && !chain.new_expression)
				{
					chain.ok = false;
				}

				return chain;
			}

			Chain ParseReceiverBefore(std::size_t end) const
			{
				Chain chain = ParseReceiver(end);
				// ParseReceiver stores the operator text as the trailing op; an
				// initializer has none, so it is harmless there.
				return chain;
			}

			Resolved ResolveHead(
				const ChainSegment& segment,
				bool new_expression,
				int depth) const
			{
				if (segment.name == "this" && segment.qualifier.empty() && segment.post.empty())
				{
					if (m_records.empty())
					{
						return {};
					}

					Resolved self;
					self.path = m_records.front();
					self.ok   = true;
					return self;
				}

				const bool call = !segment.post.empty() && segment.post.front() == '(';
				const std::string_view rest = call ? std::string_view(segment.post).substr(1)
				: std::string_view(segment.post);
				Resolved value;
				if (segment.qualifier.empty() && !call)
				{
					value = ResolveVariable(segment.name, depth);
				}
				else if (segment.qualifier.empty() || call)
				{
					value = ResolveCallOrConstruct(segment, new_expression, depth);
				}

				if (!value.ok && !segment.qualifier.empty() && !call)
				{
					value = ResolveQualifiedVariable(segment);
				}

				return ApplyPostfix(value, rest);
			}

			Resolved ResolveQualifiedVariable(const ChainSegment& segment) const
			{
				for (const auto& prefix : Prefixes(m_hint))
				{
					const auto scope = Join(prefix, segment.qualifier);
					std::unordered_set<std::string> visited;
					const Found found =
						FindDeep(scope, segment.name, CompletionKind::Variable, visited, 0);
					if (found.item != nullptr)
					{
						return FromText(found.item->type_text, found.owner, 0);
					}
				}

				return {};
			}

			Resolved ResolveCallOrConstruct(
				const ChainSegment& segment,
				bool new_expression,
				int depth) const
			{
				(void) new_expression;
				// `std::make_unique<T>(...)` and friends.
				if (segment.name == "make_unique" || segment.name == "make_shared")
				{
					if (segment.targs.empty())
					{
						return {};
					}

					Resolved pointer;
					pointer.path = {"std",
						segment.name == "make_unique" ? "unique_ptr" : "shared_ptr"};
					pointer.args = {segment.targs.front()};
					pointer.ok   = true;
					return pointer;
				}

				// `static_cast<T>(x)` and friends are a `T`.
				if ((segment.name == "static_cast" || segment.name == "dynamic_cast" ||
					segment.name == "const_cast" || segment.name == "reinterpret_cast") &&
					segment.qualifier.empty() && segment.targs.size() == 1)
				{
					return FromText(segment.targs.front(), m_hint, 0);
				}

				// Construction: `Type(...)`, `ns::Type{...}`.
				TypeName type;
				type.path            = Join(segment.qualifier, {segment.name});
				type.args            = segment.targs;
				type.ok              = true;
				Resolved constructed = ResolveType(type, m_hint, 0);
				if (constructed.ok)
				{
					return constructed;
				}

				// Free function or member of an enclosing class.
				std::vector<std::vector<std::string>> scopes;
				for (const auto& record : m_records)
				{
					scopes.push_back(record);
				}

				for (const auto& prefix : Prefixes(m_hint))
				{
					scopes.push_back(Join(prefix, segment.qualifier));
				}

				for (const auto& scope : scopes)
				{
					std::unordered_set<std::string> visited;
					const Found found =
						FindDeep(scope, segment.name, CompletionKind::Function, visited, 0);
					if (found.item != nullptr && !found.item->type_text.empty())
					{
						if (found.item->deduction)
						{
							const auto overloads = FunctionsNamed(scope, segment.name, true);
							if (std::any_of(overloads.begin(), overloads.end(),[&](const auto& candidate)
								{
									return candidate.file != found.item->file || candidate.offset != found.item->offset;
								}))
							{
								return {};
							}

							const auto deduced = DeducedReturn(*found.item, segment, found.owner, depth + 1);
							return deduced.empty() ? Resolved {}: FromText(deduced, found.owner, depth);
						}

						return FromText(found.item->type_text, found.owner, 0);
					}
				}

				return {};
			}

			std::string ArgumentType(std::size_t begin, std::size_t end, int depth) const
			{
				constexpr int kMaxDeductionDepth = 8;
				while (begin < end && IsTrivia(m_tokens[begin].kind))
				{
					++begin;
				}

				while (end > begin && IsTrivia(m_tokens[end - 1].kind))
				{
					--end;
				}

				if (begin == end || depth >= kMaxDeductionDepth) return {};
				const auto text = TokenText(m_source, m_tokens[begin]);
				if ((text == "-" || text == "+") && begin + 1 < end)
				{
					return ArgumentType(begin + 1, end, depth + 1);
				}

				if (begin + 1 == end)
				{
					if (m_tokens[begin].kind == TokenKind::Number)
					{
						const bool hexadecimal = text.starts_with("0x") || text.starts_with("0X");
						const bool floating    = text.find_first_of(hexadecimal ? ".pP" : ".eE") != std::string_view::npos;
						if (floating)
						{
							if (text.ends_with('f') || text.ends_with('F'))
							{
								return "float";
							}

							if (text.ends_with('l') || text.ends_with('L'))
							{
								return "long double";
							}

							return "double";
						}

						const auto suffix        = text.find_last_not_of("uUlL");
						const auto ending        = suffix == std::string_view::npos ? text : text.substr(suffix + 1);
						const bool unsignedValue = ending.find_first_of("uU") != std::string_view::npos;
						const auto longs = std::count_if(ending.begin(), ending.end(),[](char character)
							{
								return character == 'l' || character == 'L';
							});
						return std::string(unsignedValue ? "unsigned " : "") +(longs >= 2 ? "long long" : longs == 1 ? "long" : "int");
					}

					if (m_tokens[begin].kind == TokenKind::CharacterLiteral)
					{
						if (text.starts_with("u8"))
						{
							return "char8_t";
						}

						if (text.starts_with('u'))
						{
							return "char16_t";
						}

						if (text.starts_with('U'))
						{
							return "char32_t";
						}

						if (text.starts_with('L'))
						{
							return "wchar_t";
						}

						return "char";
					}

					if (m_tokens[begin].kind == TokenKind::StringLiteral)
					{
						return "const char*";
					}

					if (text == "true" || text == "false")
					{
						return "bool";
					}

					if (m_tokens[begin].kind == TokenKind::Identifier)
					{
						return ResolveVariable(std::string(text), depth + 1).text;
					}
				}

				const auto chain = ParseReceiver(end);
				if (!chain.ok || chain.head_token < begin) return {};
				return ResolveChain(chain, depth + 1).text;
			}

			std::string DeducedReturn(
				const CompletionItem& function,
				const ChainSegment& segment,
				const std::vector<std::string>& owner,
				int depth) const
			{
				constexpr int kMaxDeductionDepth = 8;
				if (!function.deduction || depth >= kMaxDeductionDepth ||
					segment.callOpen >= segment.callClose || segment.callClose >= m_tokens.size()) return {};
				std::vector<std::string> arguments;
				std::size_t begin = NextSignificant(segment.callOpen);
				int nesting       = 0;
				for (std::size_t token = begin; token <= segment.callClose; ++token)
				{
					const auto word = TokenText(m_source, m_tokens[token]);
					if (token == segment.callClose ||(word == "," && nesting == 0))
					{
						if (begin < token)
						{
							arguments.push_back(ArgumentType(begin, token, depth + 1));
						}

						begin = NextSignificant(token);
					}
					else if (word == "(" || word == "[" || word == "{" || word == "<")
					{
						++nesting;
					}
					else if (word == ")" || word == "]" || word == "}" || word == ">")
					{
						--nesting;
					}
				}

				std::vector<const IndexedScope*> scopes;
				for (const auto& [key, entries] : m_by_path)
				{
					scopes.insert(scopes.end(), entries.begin(), entries.end());
				}

				return TemplateDeduction::Instantiate(*function.deduction, function.templateParams,
					segment.targs, arguments, scopes, ExpandNamespaceAlias(m_aliases, owner, m_offset),
					function.type_text.find("decltype") != std::string::npos);
			}

			Resolved ResolveMember(
				const Resolved& owner,
				const ChainSegment& segment,
				int depth) const
			{
				(void) depth;
				const bool call = !segment.post.empty() && segment.post.front() == '(';
				const std::string_view rest = call ? std::string_view(segment.post).substr(1)
				: std::string_view(segment.post);
				std::unordered_set<std::string> visited;
				const Found found = FindDeep(
					owner.path, segment.name,
					call ? CompletionKind::Function : CompletionKind::Variable, visited, 0);
				if (found.item == nullptr || found.item->type_text.empty())
				{
					return {};
				}

				if (call && !segment.targs.empty() &&
					segment.targs.size() == found.item->templateParams.size())
				{
					auto written = SubstituteText(found.item->type_text,
						found.item->templateParams, segment.targs);
					auto result = FromText(written, found.owner, 0);
					if (result.ok)
					{
						result.text.clear();
					}

					return ApplyPostfix(std::move(result), rest);
				}

				// `T& value()` of `expected<Item, E>`: the member is the argument written
				// for `T` (only a bare parameter, with its `const`, `&` and `*`).
				if (found.owner == owner.path)
				{
					// The declared text keeps the specifiers (`constexpr const _Tp`): what is
					// left once `&`, `*` and a trailing `const` go is the last word, and only
					// when nothing qualifies it (`std::vector<_Tp>`, `A::_Tp` are not bare).
					std::string_view bare = found.item->type_text;
					for (bool trimmed = true; trimmed;)
					{
						trimmed = false;
						while (!bare.empty() &&
							(bare.back() == '&' || bare.back() == '*' || bare.back() == ' '))
						{
							bare.remove_suffix(1);
							trimmed = true;
						}

						if (bare.size() > 5 && bare.ends_with(" const"))
						{
							bare.remove_suffix(6);
							trimmed = true;
						}
					}

					std::size_t word = bare.size();
					while (word > 0 && IsIdentChar(bare[word - 1]))
					{
						--word;
					}

					if (word > 0 && bare[word - 1] != ' ')
					{
						bare = {};
					}
					else
					{
						bare.remove_prefix(word);
					}

					for (const IndexedScope* scope : ScopesAt(found.owner))
					{
						const auto param = std::find(
							scope->template_params.begin(), scope->template_params.end(), bare);
						const auto index =
							static_cast<std::size_t>(param - scope->template_params.begin());
						if (param != scope->template_params.end() && index < owner.args.size())
						{
							return ApplyPostfix(
								FromText(owner.args[index],
								owner.hint.empty() ? m_hint : owner.hint, 0),
								rest);
						}
					}
				}

				return ApplyPostfix(FromText(found.item->type_text, found.owner, 0), rest);
			}

			// `[]` on a typed value: element of arrays/pointers (same type) and of
			// the standard containers (first or second template argument).
			Resolved ApplyPostfix(Resolved value, std::string_view post) const
			{
				for (const char marker : post)
				{
					if (!value.ok || marker != '[')
					{
						return {};
					}

					if (value.args.empty())
					{
						continue; // raw array / pointer
					}

					const std::size_t which =
						!value.path.empty() && SubscriptYieldsSecond(value.path.back()) ? 1 : 0;
					if (which >= value.args.size())
					{
						return {};
					}

					value =
						FromText(value.args[which], value.hint.empty() ? m_hint : value.hint, 0);
				}

				return value;
			}

			const ParseTree& m_tree;
			std::string_view m_source;
			const std::vector<Token>& m_tokens;
			std::size_t m_offset;
			std::vector<NamespaceAlias> m_aliases;
			ScopeIndex m_local;
			std::unordered_map<std::string, std::vector<const IndexedScope * >> m_by_path;
			std::vector<std::vector<std::string>> m_using;
			std::vector<std::vector<std::string>> m_records; // enclosing classes, innermost first
			std::vector<std::string> m_hint;                 // namespace/class path at the cursor

		public:
			// Find the declarator for a variable by name (in local scope).
			std::size_t DeclaratorOfVariable(std::string_view name) const
			{
				for (std::size_t n = 0; n < m_tree.NodesSoA().size(); ++n)
				{
					if (m_tree.NodesSoA().Kind(n) != GrammarKind::DeclaredName)
					{
						continue;
					}

					const std::size_t token_index = m_tree.NodesSoA().FirstToken(n);
					if (token_index >= m_tokens.size() ||
						m_tree.Text(m_tokens[token_index]) != name)
					{
						continue;
					}

					const LocalInfo local = AnalyzeLocal(m_tree, n);
					if (!local.is_local || local.owner == NoIndex)
					{
						continue;
					}

					const auto[block_start, block_end] = NodeRange(m_tree, local.boundary);
					if (m_offset < block_start || m_offset > block_end)
					{
						continue;
					}

					const Token& name_token = m_tokens[token_index];
					if (name_token.offset + name_token.length > m_offset)
					{
						continue;
					}

					return DeclaratorOf(m_tree, n);
				}

				return NoIndex;
			}

			// Count pointer operators (*) in a declarator.
			int CountPointerOperators(std::size_t declarator) const
			{
				int count             = 0;
				const auto decl       = m_tree.NodesSoA()[declarator];
				const std::size_t end = decl.GetFirstToken() + decl.GetTokenCount();
				for (std::size_t t = decl.GetFirstToken(); t < end && t < m_tokens.size(); ++t)
				{
					const Token& token = m_tokens[t];
					if (token.kind == TokenKind::Punctuation && m_tree.Text(token) == "*")
					{
						++count;
					}
				}

				return count;
			}
		};

		// Next token after `token` that is not whitespace or a comment, or tokens.size().
		std::size_t NextSignificantToken(const std::vector<Token>& tokens, std::size_t token)
		{
			std::size_t i = token + 1;
			while (i < tokens.size() && IsTrivia(tokens[i].kind))
			{
				++i;
			}

			return i;
		}

		// `void S::f() const`: the definition's qualifiers between its parameter
		// list and its body.
		bool IsConstMemberFunction(const ParseTree& tree, std::size_t function)
		{
			if (function == NoIndex)
			{
				return false;
			}

			const auto grammar = tree.NodesSoA()[function];
			const auto& tokens = tree.Tokens();
			const std::size_t end =
				std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
				tokens.size());
			int depth            = 0;
			bool after_params    = false;
			bool const_qualified = false;
			for (std::size_t t = grammar.GetFirstToken(); t < end; ++t)
			{
				const std::string_view text = tree.Text(tokens[t]);
				if (tokens[t].kind == TokenKind::Punctuation)
				{
					if (text == "(")
					{
						++depth;
						const_qualified = false;
					}
					else if (text == ")" && --depth == 0)
					{
						after_params = true;
					}
					else if (depth == 0 && (text == "{" || text == ":" || text == "->"))
					{
						break;
					}

					continue;
				}

				if (after_params && depth == 0 && text == "const")
				{
					const_qualified = true;
				}
			}

			return const_qualified;
		}

		// The object named by the receiver is const: a `const T` variable or
		// reference, a pointer to const through `->`, or `this` in a const member.
		bool ReceiverIsConst(
			const ParseTree& tree,
			std::size_t offset,
			const Chain& chain,
			const Resolved& type,
			std::string_view op)
		{
			if (chain.segments.size() == 1 && chain.segments.front().name == "this" &&
				chain.segments.front().post.empty())
			{
				return IsConstMemberFunction(tree, EnclosingFunction(tree, offset));
			}

			if (chain.segments.size() != 1 ||!chain.segments.front().post.empty())
			{
				return false;
			}

			std::string_view text  = type.text;
			const std::size_t star = text.rfind('*');
			if (op == "->")
			{
				// `const S* p` keeps the `*` in its declarator: the text is the pointee;
				// smart pointers were already dereferenced to their template argument.
				text = text.substr(0, star);
			}
			else if (star != std::string_view::npos)
			{
				return false;
			}

			return HasWord(text, "const");
		}

		void CompleteMember(
			const ParseTree& tree,
			std::size_t offset,
			std::string_view prefix,
			const ScopeIndex* external,
			bool for_hover,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best)
		{
			const std::string_view source    = tree.Source();
			const std::vector<Token>& tokens = tree.Tokens();
			const std::size_t op             = AccessOperatorBefore(source, tokens, offset, prefix);
			if (op >= tokens.size())
			{
				return;
			}

			const std::string_view op_text = TokenText(source, tokens[op]);
			if (op_text != "." && op_text != "->")
			{
				return; // `.*` takes a pointer-to-member, not a member name
			}

			const MemberResolver resolver(tree, external, offset);

			// `T{ .na`: a designator names a member of the class being initialized.
			// The `.` follows `{` or `,` (a member access follows an expression).
			const std::size_t lead = PreviousSignificant(tokens, op, source);
			if (op_text == "." && lead < tokens.size() &&
				tokens[lead].kind == TokenKind::Punctuation &&
				(TokenText(source, tokens[lead]) == "{" || TokenText(source, tokens[lead]) == ","))
			{
				std::size_t open = lead;
				if (TokenText(source, tokens[lead]) == ",")
				{
					int depth = 0;
					for (open = lead; open > 0; --open)
					{
						const Token& token = tokens[open - 1];
						if (token.kind != TokenKind::Punctuation)
						{
							continue;
						}

						const std::string_view p = TokenText(source, token);
						if (p == ")" || p == "]" || p == "}")
						{
							++depth;
						}
						else if ((p == "(" || p == "[" || p == "{") && depth--== 0)
						{
							--open;
							break;
						}
					}

					if (open >= tokens.size() || TokenText(source, tokens[open]) != "{")
					{
						return;
					}
				}

				// Members already designated in this list are not offered again.
				std::unordered_set<std::string> used;
				int nest = 0;
				for (std::size_t i = open; i + 2 < tokens.size(); ++i)
				{
					if (tokens[i].kind != TokenKind::Punctuation)
					{
						continue;
					}

					const std::string_view p = TokenText(source, tokens[i]);
					if (p == "{" || p == "(" || p == "[")
					{
						++nest;
					}
					else if (p == "}" || p == ")" || p == "]")
					{
						if (--nest == 0)
						{
							break;
						}
					}
					else if (p == "." && nest == 1 && i != op)
					{
						const std::size_t name = NextSignificantToken(tokens, i);
						const std::size_t after =
							name < tokens.size() ? NextSignificantToken(tokens, name)
						: tokens.size();
						const std::size_t lead_in = PreviousSignificant(tokens, i, source);
						if (name < tokens.size() && after < tokens.size() &&
							tokens[name].kind == TokenKind::Identifier &&
							(TokenText(source, tokens[after]) == "=" ||
							TokenText(source, tokens[after]) == "{") &&
							lead_in < tokens.size() &&
							(TokenText(source, tokens[lead_in]) == "{" ||
							TokenText(source, tokens[lead_in]) == ","))
						{
							used.emplace(TokenText(source, tokens[name]));
						}
					}
				}

				const Resolved owner = resolver.ResolveBraceOwner(open);
				if (owner.ok)
				{
					resolver.CollectFields(owner.path, prefix, used, interner, best);
				}

				return;
			}

			const Chain chain = resolver.ParseReceiver(op);

			// If using '.' on a raw pointer variable, offer no completions (invalid in C++).
			// Only check when the receiver is a simple variable (single segment, no
			// qualifier/postfix).
			if (op_text == "." && chain.segments.size() == 1)
			{
				const ChainSegment& receiver = chain.segments.front();
				// Receiver must be a simple variable (no qualifier, no postfix).
				if (receiver.qualifier.empty() && receiver.post.empty())
				{
					const std::size_t declarator = resolver.DeclaratorOfVariable(receiver.name);
					if (declarator != NoIndex && resolver.CountPointerOperators(declarator) > 0)
					{
						return; // `.` on raw pointer: invalid
					}
				}
			}

			const Resolved type = resolver.ResolveChain(chain);
			if (!type.ok)
			{
				return;
			}

			MemberFilter filter;
			filter.const_receiver = !for_hover && ReceiverIsConst(tree, offset, chain, type, op_text);
			filter.ignore_access  = for_hover;
			resolver.CollectMembers(type.path, prefix, interner, best, filter, type.args);
		}

		// Unqualified names inside a class body or an out-of-line member definition
		// also find the class's members and its bases' accessible members. An
		// explicit-object member function (`this Self& self`) has no implicit `this`.
		void CompleteImplicitMembers(
			const ParseTree& tree,
			const CursorScopes& cursor,
			std::size_t offset,
			std::string_view prefix,
			const ScopeIndex* external,
			bool for_hover,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best)
		{
			if (cursor.explicit_object)
			{
				return;
			}

			bool member_context = cursor.record != NoIndex;
			if (!member_context && cursor.function != NoIndex)
			{
				const std::size_t declarator =
					FindChild(tree, cursor.function, GrammarKind::Declarator);
				member_context = declarator != NoIndex &&
					FindChild(tree, declarator, GrammarKind::NestedNameSpecifier) != NoIndex;
			}

			if (!member_context)
			{
				return;
			}

			const MemberResolver resolver(tree, external, offset);
			if (resolver.EnclosingRecords().empty())
			{
				return;
			}

			MemberFilter filter;
			filter.include_types = true;
			filter.ignore_access = for_hover;
			resolver.CollectMembers(resolver.EnclosingRecords().front(), prefix, interner, best,
				filter);
		}

		// `virtual void f(int) const = 0` as an override declaration:
		// `void f(int) const override`.
		std::string OverrideLabel(std::string_view detail)
		{
			std::string label(detail);
			if (label.starts_with("virtual "))
			{
				label.erase(0, std::string_view("virtual ").size());
			}

			constexpr std::string_view kTrailers[] = {" override", " final", " = 0", "=0"};
			for (bool trimmed = true; trimmed;)
			{
				trimmed = false;
				while (!label.empty() && label.back() == ' ')
				{
					label.pop_back();
				}

				for (const auto trailer : kTrailers)
				{
					if (label.ends_with(trailer))
					{
						label.resize(label.size() - trailer.size());
						trimmed = true;
					}
				}
			}

			return label + " override";
		}

		// At the start of a member declaration in a class body, suggest overriding
		// the virtual functions the class inherits.
		void CompleteOverrides(
			const ParseTree& tree,
			const CursorScopes& cursor,
			std::size_t offset,
			std::string_view prefix,
			const ScopeIndex* external,
			StringInterner& interner,
			FlatHashMap<CompletionItem>& best)
		{
			const std::size_t record = cursor.record;
			if (record == NoIndex)
			{
				return;
			}

			if (cursor.function != NoIndex &&
				NodeRange(tree, cursor.function).first > NodeRange(tree, record).first)
			{
				return; // inside a member function
			}

			const auto& tokens   = tree.Tokens();
			const std::size_t at = offset - std::min(offset, prefix.size());
			std::size_t before   = TokenBefore(tokens, at);
			if (before < tokens.size() && (tokens[before].kind == TokenKind::Whitespace ||
				tokens[before].kind == TokenKind::LineComment ||
				tokens[before].kind == TokenKind::BlockComment))
			{
				before = PreviousSignificant(tokens, before, tree.Source());
			}

			if (before >= tokens.size())
			{
				return;
			}

			const std::string_view lead = tree.Text(tokens[before]);
			bool declaration_start      = lead == "{" || lead == ";" || lead == "}";
			if (lead == ":")
			{
				const std::size_t label = PreviousSignificant(tokens, before, tree.Source());
				const std::string_view word =
					label < tokens.size() ? tree.Text(tokens[label]) : std::string_view {};
				declaration_start = word == "public" || word == "protected" || word == "private";
			}

			if (!declaration_start)
			{
				return;
			}

			const MemberResolver resolver(tree, external, offset);
			for (const auto& function_item : resolver.Overridable(ScopePath(tree, record)))
			{
				CompletionItem item {OverrideLabel(function_item.detail), CompletionKind::Function,
					"override", function_item.documentation};
				item.filter_text = function_item.label;
				if (MatchesItem(item, prefix))
				{
					InsertItem(interner, best, std::move(item));
				}
			}
		}

		// `{` that opens a namespace, class or block body rather than a braced
		// initializer.
		bool IsScopeBrace(const ParseTree& tree, std::size_t brace)
		{
			const auto& tokens = tree.Tokens();
			for (std::size_t n = 1; n < tree.NodesSoA().size(); ++n)
			{
				const GrammarKind kind = tree.NodesSoA().Kind(n);
				if ((kind == GrammarKind::CompoundStatement || kind == GrammarKind::LambdaExpression) &&
					tree.NodesSoA().FirstToken(n) == brace)
				{
					return true;
				}

				if (kind != GrammarKind::RecordDefinition && kind != GrammarKind::NamespaceDefinition)
				{
					continue;
				}

				const auto grammar = tree.NodesSoA()[n];
				const std::size_t end =
					std::min<std::size_t>(grammar.GetFirstToken() + grammar.GetTokenCount(),
					tokens.size());
				for (std::size_t t = grammar.GetFirstToken(); t < end; ++t)
				{
					if (tokens[t].kind == TokenKind::Punctuation && tree.Text(tokens[t]) == "{")
					{
						if (t == brace)
						{
							return true;
						}

						break;
					}
				}
			}

			return false;
		}

		// The argument list around the cursor: its opening bracket and the index of
		// the argument being typed.
		struct CallSite
		{
			std::size_t open     = NoIndex;
			std::size_t argument = 0;
			// Designators already written (`.c = 3`), with the argument index each holds.
			std::vector<std::pair<std::string, std::size_t>> designators;
			bool ok = false;
		};

		CallSite FindCallSite(const ParseTree& tree, std::size_t offset)
		{
			CallSite site;
			const auto& tokens            = tree.Tokens();
			const std::string_view source = tree.Source();
			std::size_t k                 = TokenBefore(tokens, offset);
			if (k >= tokens.size())
			{
				return site;
			}

			std::vector<std::size_t> commas;
			int depth = 0;
			for (++k; k > 0;)
			{
				--k;
				const Token& token = tokens[k];
				if (token.kind != TokenKind::Punctuation)
				{
					continue;
				}

				const std::string_view text = TokenText(source, token);
				if (text == ")" || text == "]" || text == "}")
				{
					++depth;
				}
				else if (text == "(" || text == "[" || text == "{")
				{
					if (depth > 0)
					{
						--depth;
						continue;
					}

					if (text == "[" ||(text == "{" && IsScopeBrace(tree, k)))
					{
						return site;
					}

					site.open = k;
					break;
				}
				else if (text == ";" && depth == 0)
				{
					return site;
				}
				else if (text == "," && depth == 0)
				{
					commas.push_back(k);
				}
			}

			if (site.open == NoIndex)
			{
				return site;
			}

			site.argument = commas.size();
			// `.name =` at the start of an argument: a designated initializer.
			std::vector<std::size_t> starts {site.open};
			starts.insert(starts.end(), commas.rbegin(), commas.rend());
			for (std::size_t index = 0; index < starts.size(); ++index)
			{
				const std::size_t dot = NextSignificantToken(tokens, starts[index]);
				const std::size_t name =
					dot < tokens.size() ? NextSignificantToken(tokens, dot) : tokens.size();
				if (name < tokens.size() && tokens[dot].offset < offset &&
					TokenText(source, tokens[dot]) == "." &&
					tokens[name].kind == TokenKind::Identifier)
				{
					site.designators.emplace_back(std::string(TokenText(source, tokens[name])),
						index);
				}
			}

			site.ok = true;
			return site;
		}

		// `<...>` before `close` (a `>` or `>>`): index of the opening `<`.
		std::size_t MatchAngleBefore(
			const std::vector<Token>& tokens,
			std::string_view source,
			std::size_t close)
		{
			int depth = 0;
			for (std::size_t k = close + 1; k > 0;)
			{
				--k;
				if (tokens[k].kind != TokenKind::Punctuation)
				{
					continue;
				}

				const std::string_view text = TokenText(source, tokens[k]);
				depth += text == ">" ? 1 : text == ">>" ? 2 : text == "<" ? -1 : 0;
				if (text == ";" || text == "{" || text == "}")
				{
					return NoIndex;
				}

				if (depth <= 0 && text == "<")
				{
					return k;
				}
			}

			return NoIndex;
		}

		// What the argument list belongs to.
		struct Callee
		{
			std::vector<CompletionItem> functions;
			// Constructors / aggregate initialization of this record.
			std::vector<std::string> record;
			bool construct = false;
		};

		Callee ResolveCallee(
			const ParseTree& tree,
			const MemberResolver& resolver,
			std::size_t open,
			std::string_view bracket)
		{
			Callee callee;
			const auto& tokens            = tree.Tokens();
			const std::string_view source = tree.Source();
			std::size_t name              = PreviousSignificant(tokens, open, source);
			if (name < tokens.size() && tokens[name].kind == TokenKind::Punctuation &&
				(TokenText(source, tokens[name]) == ">" || TokenText(source, tokens[name]) == ">>"))
			{
				const std::size_t angle = MatchAngleBefore(tokens, source, name);
				name                    = angle == NoIndex ? tokens.size() : PreviousSignificant(tokens, angle, source);
			}

			const auto construct = [&](std::string_view type_text)
			{
				const Resolved type = resolver.ResolveTypeText(type_text);
				if (type.ok)
				{
					callee.record    = type.path;
					callee.construct = true;
					callee.functions = resolver.FunctionsNamed(type.path, type.path.back(), false);
				}
			};
			const auto text_between = [&](std::size_t first, std::size_t last)
			{
				return std::string_view(source).substr(
					tokens[first].offset, tokens[last].offset + tokens[last].length -
					tokens[first].offset);
			};

			// `S s = {`: the declared variable's type.
			if (name < tokens.size() && bracket == "{" && TokenText(source, tokens[name]) == "=")
			{
				const std::size_t variable = PreviousSignificant(tokens, name, source);
				const std::size_t type =
					variable < tokens.size() ? PreviousSignificant(tokens, variable, source)
				: tokens.size();
				if (type < tokens.size() && tokens[type].kind == TokenKind::Identifier)
				{
					construct(TokenText(source, tokens[type]));
				}

				return callee;
			}

			if (name >= tokens.size() || tokens[name].kind != TokenKind::Identifier)
			{
				return callee;
			}

			const std::string_view word = TokenText(source, tokens[name]);
			std::size_t head            = name;
			std::vector<std::string> qualifier;
			bool global      = false;
			std::size_t lead = PreviousSignificant(tokens, name, source);
			while (lead < tokens.size() && TokenText(source, tokens[lead]) == "::")
			{
				const std::size_t part = PreviousSignificant(tokens, lead, source);
				if (part >= tokens.size() || tokens[part].kind != TokenKind::Identifier)
				{
					global = true;
					lead   = part;
					break;
				}

				qualifier.insert(qualifier.begin(), std::string(TokenText(source, tokens[part])));
				head = part;
				lead = PreviousSignificant(tokens, part, source);
			}

			const std::string_view lead_text =
				lead < tokens.size() ? TokenText(source, tokens[lead]) : std::string_view {};
			const std::string_view spelled = text_between(head, name);

			// `obj.f(` / `ptr->f(`.
			if (qualifier.empty() && !global && (lead_text == "." || lead_text == "->"))
			{
				const Chain chain   = resolver.ParseReceiver(lead);
				const Resolved type = resolver.ResolveChain(chain);
				if (type.ok)
				{
					callee.functions = resolver.FunctionsNamed(type.path, word, true);
				}

				return callee;
			}

			// `new S(` and `S s(` / `S s{` / `const ns::S& s(`: constructors of the type.
			if (lead_text == "new")
			{
				construct(spelled);
				return callee;
			}

			if (qualifier.empty() && !global && lead < tokens.size() &&
				(tokens[lead].kind == TokenKind::Identifier || lead_text == ">" ||
				lead_text == ">>") && !IsKeyword(lead_text))
			{
				std::size_t type_end = lead;
				std::size_t first    = lead;
				if (lead_text == ">" || lead_text == ">>")
				{
					const std::size_t angle = MatchAngleBefore(tokens, source, lead);
					first                   = angle == NoIndex ? lead : PreviousSignificant(tokens, angle, source);
				}

				while (first < tokens.size())
				{
					const std::size_t colons = PreviousSignificant(tokens, first, source);
					if (colons >= tokens.size() || TokenText(source, tokens[colons]) != "::")
					{
						break;
					}

					const std::size_t part = PreviousSignificant(tokens, colons, source);
					if (part >= tokens.size() || tokens[part].kind != TokenKind::Identifier)
					{
						break;
					}

					first = part;
				}

				construct(text_between(first, type_end));
				return callee;
			}

			// `B() : member(` / `, member{`: constructors of the member's type.
			if (qualifier.empty() && !global && (lead_text == ":" || lead_text == ",") &&
				!resolver.EnclosingRecords().empty())
			{
				for (const auto& field : resolver.FieldsInOrder(resolver.EnclosingRecords().front()))
				{
					if (field.label == word && !field.type_text.empty())
					{
						construct(field.type_text);
						return callee;
					}
				}
			}

			// `S(` / `S{` / `ns::S(`: a type names its constructors.
			std::vector<std::string> path = qualifier;
			path.emplace_back(word);
			const Resolved type = resolver.ResolveTypeText(spelled);
			if (type.ok && !type.path.empty() && type.path.back() == word)
			{
				callee.record    = type.path;
				callee.construct = true;
				callee.functions = resolver.FunctionsNamed(type.path, word, false);
				return callee;
			}

			if (qualifier.empty() && !global)
			{
				callee.functions = resolver.VisibleFunctions(word);
			}
			else
			{
				const Resolved scope = resolver.ResolveTypeText(text_between(head, name));
				callee.functions     = resolver.FunctionsNamed(qualifier, word, false);
				if (callee.functions.empty() && scope.ok)
				{
					callee.functions = resolver.FunctionsNamed(
						std::vector<std::string>(scope.path.begin(), scope.path.end() - 1), word,
						false);
				}
			}

			return callee;
		}

		SignatureInfo MakeSignature(
			const CompletionItem& function,
			std::string_view name,
			std::string_view bracket,
			bool construct)
		{
			SignatureInfo signature;
			signature.label = std::string(name);
			signature.label += bracket == "{" ? "{" : "(";
			auto parameters = DetailParameters(function.detail, function.label);
			if (function.explicit_object && !parameters.empty())
			{
				parameters.erase(parameters.begin()); // `this Self& self` is the object
			}

			for (std::size_t i = 0; i < parameters.size(); ++i)
			{
				if (i != 0)
				{
					signature.label += ", ";
				}

				const auto begin = static_cast<std::uint32_t>(signature.label.size());
				signature.label += parameters[i].text;
				signature.parameterNames.emplace_back(parameters[i].name);
				signature.parameters.emplace_back(begin,
					static_cast<std::uint32_t>(signature.label.size()));
			}

			signature.label += bracket == "{" ? "}" : ")";
			if (!construct && !function.type_text.empty())
			{
				signature.label += " -> ";
				signature.label += function.type_text;
			}

			signature.documentation = function.documentation;
			return signature;
		}

		bool IsVariadicSignature(const SignatureInfo& signature)
		{
			if (signature.parameters.empty())
			{
				return false;
			}

			const auto[begin, end] = signature.parameters.back();
			return std::string_view(signature.label).substr(begin, end - begin).find("...") !=
				std::string_view::npos;
		}

		SignatureHelp ComputeSignatures(
			const ParseTree& tree,
			std::size_t offset,
			const ScopeIndex* external)
		{
			SignatureHelp help;
			const CallSite site = FindCallSite(tree, offset);
			if (!site.ok)
			{
				return help;
			}

			const auto& tokens             = tree.Tokens();
			const std::string_view bracket = TokenText(tree.Source(), tokens[site.open]);
			const MemberResolver resolver(tree, external, offset);
			const Callee callee = ResolveCallee(tree, resolver, site.open, bracket);
			const std::string_view name =
				callee.construct ? std::string_view(callee.record.back()) : std::string_view {};

			// One signature per overload; redeclarations keep the one naming its parameters.
			std::vector<CompletionItem> overloads;
			for (const auto& function : callee.functions)
			{
				if (function.deleted)
				{
					continue;
				}

				const std::string key = ParameterTypesKey(function);
				auto same = std::find_if(overloads.begin(), overloads.end(),[&](const auto& other)
					{
						return ParameterTypesKey(other) == key;
					});
				if (same == overloads.end())
				{
					overloads.push_back(function);
				}
				else if (NamedParameterCount(function.detail) > NamedParameterCount(same->detail))
				{
					*same = function;
				}
			}

			for (const auto& function : overloads)
			{
				help.signatures.push_back(MakeSignature(function,
					callee.construct ? name : std::string_view(function.label), bracket,
					callee.construct));
			}

			std::size_t argument = site.argument;
			if (callee.construct && bracket == "{" && callee.functions.empty())
			{
				// Aggregate initialization: one parameter per field, in order.
				const auto fields = resolver.FieldsInOrder(callee.record);
				if (!fields.empty())
				{
					SignatureInfo aggregate;
					aggregate.label = std::string(name) + "{";
					for (std::size_t i = 0; i < fields.size(); ++i)
					{
						if (i != 0)
						{
							aggregate.label += ", ";
						}

						const auto begin = static_cast<std::uint32_t>(aggregate.label.size());
						aggregate.label += fields[i].detail + " " + fields[i].label;
						aggregate.parameterNames.push_back(fields[i].label);
						aggregate.parameters.emplace_back(begin,
							static_cast<std::uint32_t>(aggregate.label.size()));
					}

					aggregate.label += "}";
					help.signatures.push_back(std::move(aggregate));
					if (!site.designators.empty())
					{
						const auto& [field, index] = site.designators.back();
						for (std::size_t i = 0; i < fields.size(); ++i)
						{
							if (fields[i].label == field)
							{
								argument = i +(site.argument - index);
								break;
							}
						}
					}
				}
			}

			if (help.signatures.empty())
			{
				return help;
			}

			// Overloads that cannot take the argument being typed are dropped while
			// another one can.
			const auto viable = [&](const SignatureInfo& signature)
			{
				return argument < signature.parameters.size() || IsVariadicSignature(signature) ||
					(argument == 0 && signature.parameters.empty());
			};
			if (std::any_of(help.signatures.begin(), help.signatures.end(), viable))
			{
				std::erase_if(help.signatures,[&](const SignatureInfo& signature)
					{
						return !viable(signature);
					});
			}

			const SignatureInfo& active = help.signatures.front();
			if (IsVariadicSignature(active) && argument >= active.parameters.size())
			{
				argument = active.parameters.size() - 1;
			}

			help.active_parameter = static_cast<std::uint32_t>(argument);
			help.call_open        = tokens[site.open].offset;
			return help;
		}

		// `auto x = f();` hovers as the type `f` returns instead of `auto`. Only the
		// spelling of `auto` is replaced, so `const auto&` stays `const T&`; a plain
		// `auto` drops the reference and top-level const the function declared.
		void RefineAutoDetail(
			const ParseTree& tree,
			const ScopeIndex* external,
			std::size_t offset,
			CompletionItem& item)
		{
			if (item.kind != CompletionKind::Variable ||!item.has_location ||
				item.type_text.empty())
			{
				return;
			}

			const std::string_view declared = item.type_text;
			const auto at                   = declared.find("auto");
			const auto is_word = [&](std::size_t i)
			{
				return i < declared.size() && IsIdentChar(declared[i]);
			};
			if (at == std::string_view::npos ||(at > 0 && is_word(at - 1)) || is_word(at + 4))
			{
				return;
			}

			const auto& tokens = tree.Tokens();
			const auto token = std::lower_bound(
				tokens.begin(), tokens.end(), item.offset,
				[](const Token& t, std::uint32_t value)
				{
					return t.offset < value;
				});
			if (token == tokens.end() || token->offset != item.offset)
			{
				return;
			}

			const MemberResolver resolver(tree, external, offset);
			std::string deduced =
				resolver.DeducedTypeText(static_cast<std::size_t>(token - tokens.begin()));
			if (deduced.empty())
			{
				return;
			}

			// A deduction that still spells `auto` learned nothing (the
			// initializer resolved back to the declared text): substituting it
			// would duplicate qualifiers (`constexpr constexpr auto`).
			for (std::size_t found = deduced.find("auto"); found != std::string::npos;
				found = deduced.find("auto", found + 4))
			{
				const bool left_ok  = found == 0 ||!IsIdentChar(deduced[found - 1]);
				const bool right_ok = found + 4 >= deduced.size() ||!IsIdentChar(deduced[found + 4]);
				if (left_ok && right_ok)
				{
					return;
				}
			}

			// `auto` never deduces a reference; it keeps the function's const only when
			// the declaration does not add its own (`const auto`) or drop it (plain `auto`).
			const auto strip = [&](std::string_view part, bool front)
			{
				if (front ? deduced.starts_with(part) : deduced.ends_with(part))
				{
					deduced.erase(front ? 0 : deduced.size() - part.size(), part.size());
					return true;
				}

				return false;
			};
			const bool declared_const =
				declared.substr(0, at).find("const") != std::string_view::npos;
			while (strip("&&", false) || strip("&", false) || strip(" ", false) ||
				((declared == "auto" || declared_const) &&
				(strip("const ", true) || strip(" const", false)))) {}

			std::string detail(declared.substr(0, at));
			detail += deduced;
			detail += declared.substr(at + 4);
			item.detail = std::move(detail);
		}

	} // namespace

	namespace
	{

		class IndexedTypeNames final : public TypeNameOracle
		{
		public:
			explicit IndexedTypeNames(
				std::unordered_set<std::string> names,
				std::unordered_map<std::string, std::string> aliases) :
				m_names(std::move(names)), m_aliases(std::move(aliases))
			{
				// Order-independent, so equal indexes have equal fingerprints.
				for (const auto& name : m_names)
				{
					m_fingerprint += std::hash<std::string> {}(name) * 0x9E3779B97F4A7C15ull;
				}

				m_fingerprint ^= m_names.size() << 1 | 1;
			}

			bool IsType(std::string_view name) const noexcept override
			{
				return m_names.find(std::string(name)) != m_names.end();
			}

			std::string_view AliasTarget(std::string_view name) const noexcept override
			{
				const auto found = m_aliases.find(std::string(name));
				return found == m_aliases.end() ? std::string_view {}: found->second;
			}

			std::uint64_t Fingerprint() const noexcept override
			{
				return m_fingerprint;
			}

		private:
			std::unordered_set<std::string> m_names;
			std::unordered_map<std::string, std::string> m_aliases;
			std::uint64_t m_fingerprint = 0;
		};

	} // namespace

	std::shared_ptr<const TypeNameOracle> CompletionEngine::TypeNamesOf(const ScopeIndex& index)
	{
		// A scope entry does not say whether it is a namespace or a record, but its
		// parent lists it as a Namespace member in the first case and as a Type in the
		// second. Records list their members as Types too (nested classes, member
		// aliases): only namespace-level names are visible without a qualifier.
		const auto key_of = [](const std::vector<std::string>& path, std::string_view last)
		{
			std::string key;
			for (const auto& part : path)
			{
				key += part;
				key += "::";
			}

			key += last;
			return key;
		};
		std::unordered_set<std::string> namespaces;
		for (const auto& scope : index)
		{
			for (const auto& member : scope.members)
			{
				if (member.kind == CompletionKind::Namespace)
				{
					namespaces.insert(key_of(scope.path, member.label));
				}
			}
		}

		std::unordered_set<std::string> names;
		std::unordered_map<std::string, std::string> aliases;
		for (const auto& scope : index)
		{
			if (!scope.path.empty() &&
				namespaces.find(key_of({scope.path.begin(), scope.path.end() - 1},
				scope.path.back())) == namespaces.end())
			{
				continue;
			}

			for (const auto& member : scope.members)
			{
				if (member.kind == CompletionKind::Type && !member.label.empty() &&
					member.label.front() != '_')
				{
					names.insert(member.label);
					if (!member.type_text.empty())
					{
						aliases.try_emplace(member.label, member.type_text);
					}
				}
			}
		}

		return std::make_shared<const IndexedTypeNames>(std::move(names), std::move(aliases));
	}

	std::string CompletionEngine::PrefixAt(std::string_view source, std::size_t offset)
	{
		if (offset > source.size())
		{
			offset = source.size();
		}

		std::size_t start = offset;
		while (start > 0 && IsIdentChar(source[start - 1]))
		{
			--start;
		}

		return std::string(source.substr(start, offset - start));
	}

	namespace
	{
		// `call(a, /*na|`: a block comment opened right after `(` or `,` of a call,
		// holding at most the start of one word. Names the callee and the argument.
		struct ParameterComment
		{
			std::string_view callee;
			std::size_t argument = 0;
			std::string_view typed;
			bool ok = false;
		};

		ParameterComment ParameterCommentAt(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t offset)
		{
			ParameterComment result;
			const std::size_t comment = TokenBefore(tokens, offset);
			if (comment >= tokens.size() || tokens[comment].kind != TokenKind::BlockComment)
			{
				return result;
			}

			constexpr std::size_t kOpenerLen = 2;
			const Token& token               = tokens[comment];
			if (offset < token.offset + kOpenerLen)
			{
				return result;
			}

			std::string_view inside =
				source.substr(token.offset + kOpenerLen, offset - token.offset - kOpenerLen);
			while (!inside.empty() && (inside.front() == ' ' || inside.front() == '\t'))
			{
				inside.remove_prefix(1);
			}

			if (!std::all_of(inside.begin(), inside.end(),[](char c)
				{
					return IsIdentChar(c);
				})
			)
			{
				return result;
			}

			std::size_t open     = PreviousSignificant(tokens, comment, source);
			std::size_t argument = 0;
			int depth            = 0;
			while (open < tokens.size())
			{
				if (tokens[open].kind == TokenKind::Punctuation)
				{
					const std::string_view text = TokenText(source, tokens[open]);
					if (text == ")" || text == "]" || text == "}")
					{
						++depth;
					}
					else if (text == "(" || text == "[" || text == "{")
					{
						if (depth == 0)
						{
							break;
						}

						--depth;
					}
					else if (text == "," && depth == 0)
					{
						++argument;
					}
					else if (text == ";" && depth == 0)
					{
						return result;
					}
				}

				open = open == 0 ? tokens.size() : PreviousSignificant(tokens, open, source);
			}

			const std::size_t lead = PreviousSignificant(tokens, comment, source);
			if (open >= tokens.size() || TokenText(source, tokens[open]) != "(" ||
				lead >= tokens.size() ||
				(TokenText(source, tokens[lead]) != "(" && TokenText(source, tokens[lead]) != ","))
			{
				return result;
			}

			const std::size_t callee = PreviousSignificant(tokens, open, source);
			if (callee >= tokens.size() || tokens[callee].kind != TokenKind::Identifier)
			{
				return result;
			}

			result.callee   = TokenText(source, tokens[callee]);
			result.argument = argument;
			result.typed    = inside;
			result.ok       = true;
			return result;
		}

		// `name=*/` for the parameter at the comment's argument position, from every
		// declaration of the callee (overloads are merged by name).
		std::vector<CompletionItem> CompleteParameterComment(
			const ParseTree& tree,
			const ParameterComment& comment,
			const ScopeIndex* external)
		{
			std::vector<CompletionItem> signatures;
			for (std::size_t n = 0; n < tree.NodesSoA().size(); ++n)
			{
				if (tree.NodesSoA().Kind(n) != GrammarKind::DeclaredName ||
					tree.Text(tree.Tokens()[tree.NodesSoA().FirstToken(n)]) != comment.callee ||
					ClassifyDeclaredName(tree, n) != CompletionKind::Function)
				{
					continue;
				}

				signatures.push_back(DescribeDeclared(tree, tree.Source(), tree.Tokens(), n,
					comment.callee, CompletionKind::Function));
			}

			if (external != nullptr)
			{
				for (const auto& scope : *external)
				{
					for (const auto& member : scope.members)
					{
						if (member.kind == CompletionKind::Function && member.label == comment.callee)
						{
							signatures.push_back(member);
						}
					}
				}
			}

			std::vector<CompletionItem> items;
			for (const auto& signature : signatures)
			{
				const auto parameters = DetailParameters(signature.detail, signature.label);
				if (comment.argument >= parameters.size())
				{
					continue;
				}

				const std::string_view name = parameters[comment.argument].name;
				if (name.empty() ||!MatchesPrefix(name, comment.typed))
				{
					continue;
				}

				std::string label = std::string(name) + "=*/";
				const bool seen = std::any_of(items.begin(), items.end(),
					[&](const CompletionItem& item)
					{
						return item.label == label;
					});
				if (seen)
				{
					continue;
				}

				CompletionItem item {std::move(label), CompletionKind::Variable, signature.detail, {}};
				item.filter_text = std::string(name);
				items.push_back(std::move(item));
			}

			std::sort(items.begin(), items.end(),[](const auto& left, const auto& right)
				{
					return left.label < right.label;
				});
			return items;
		}
	} // namespace

	std::vector<CompletionItem> CompletionEngine::Complete(
		std::string_view source,
		std::size_t offset)
	{
		ParserOptions options;
		return Complete(source, options, offset);
	}

	std::vector<CompletionItem> CompletionEngine::Complete(
		std::string_view source,
		const ParserOptions& options,
		std::size_t offset)
	{
		return Complete(source, options, offset, nullptr);
	}

	std::vector<CompletionItem> CompletionEngine::Complete(
		std::string_view source,
		const ParserOptions& options,
		std::size_t offset,
		const ScopeIndex* external)
	{
		if (offset > source.size())
		{
			offset = source.size();
		}

		const std::string prefix        = PrefixAt(source, offset);
		const std::vector<Token> tokens = Lexer(source).Lex();
		const CursorContext context     = ClassifyContext(source, tokens, offset, prefix);

		if (context == CursorContext::Suppressed && !ParameterCommentAt(source, tokens, offset).ok)
			return {};

		// Use flat hash map with interned strings for better cache behavior
		StringInterner interner;
		FlatHashMap<CompletionItem> best;
		best.Reserve(256);

		if (context == CursorContext::Preprocessor)
		{
			for (const auto directive : kDirectives)
			{
				if (prefix.empty() || StartsWith(directive, prefix))
				{
					InsertItem(
						interner, best,
						{std::string(directive), CompletionKind::Directive, "directive", {}});
				}
			}

			// Preprocessor path still uses string map for simplicity (less hot)
			std::unordered_map<std::string, CompletionItem> best_str;
			CollectDefines(source, tokens, best_str, prefix);
			for (const auto& [name, value] : options.Macros())
			{
				if (!prefix.empty() && !StartsWith(name, prefix))
				{
					continue;
				}

				std::string detail = value.empty() ? "macro" : value;
				if (detail.size() > kMaxShortDetailLen)
				{
					detail.resize(kMaxShortDetailLen);
				}

				InsertItem(best_str, {name, CompletionKind::Macro, std::move(detail), {}});
			}

			// Merge string map into flat map
			for (auto& [label, item] : best_str)
			{
				InsertItem(interner, best, std::move(item));
			}
		}
		else
		{
			// The grammar tree carries the scope structure for both unqualified
			// visibility and qualified (`ns::`) member resolution.
			const ParseTree tree = ParseTree::Parse(source, options);
			return Complete(tree, options, offset, external);
		}

		return RankItems(CollectAndSort(best), prefix);
	}

	static std::vector<CompletionItem> CompleteTree(
		const ParseTree& tree,
		const ParserOptions& options,
		std::size_t offset,
		const ScopeIndex* external,
		bool for_hover);

	namespace
	{
		bool RequiresReflectionValue(
			std::string_view source,
			const std::vector<Token>& tokens,
			std::size_t offset)
		{
			struct Frame
			{
				std::string_view close;
				bool splice   = false;
				bool ordinary = false;
			};

			std::vector<Frame> frames;
			std::string_view previous;
			for (std::size_t token = 0; token < tokens.size() && tokens[token].offset < offset; ++token)
			{
				if (IsTrivia(tokens[token].kind))
				{
					continue;
				}

				const auto word = TokenText(source, tokens[token]);
				if (word == "[")
				{
					const auto next = std::find_if(tokens.begin() + token + 1, tokens.end(),
						[](const Token& candidate)
						{
							return !IsTrivia(candidate.kind);
						});
					const bool splice = next != tokens.end() && TokenText(source, *next) == ":";
					frames.emplace_back("]", splice, !splice);
				}
				else if (word == "(" || word == "{")
				{
					const bool argument = word == "{" ||(!previous.empty() &&
						(IsIdentChar(previous.back()) || previous == ")" || previous == ">"));
					frames.emplace_back(word == "(" ? ")" : "}", false, argument);
				}
				else if (!frames.empty() && word == frames.back().close)
				{
					frames.pop_back();
				}
				else if (word == "^^" && !frames.empty())
				{
					frames.back().ordinary = true;
				}

				previous = word;
			}

			bool ordinary = false;
			for (auto frame = frames.rbegin(); frame != frames.rend(); ++frame)
			{
				ordinary = ordinary || frame->ordinary;
				if (frame->splice)
				{
					return !ordinary;
				}
			}

			return false;
		}

		bool IsReflectionValueType(std::string_view text, const MemberResolver& resolver)
		{
			const auto written  = ParseTypeName(text);
			const auto resolved = resolver.ResolveTypeText(text);
			const std::vector<std::string> reflectionPath
			{
				"std", "meta", "info"
			};
			return written.args.empty() && text.find('*') == std::string_view::npos &&
				text.find('[') == std::string_view::npos &&
				(written.path == reflectionPath ||(resolved.ok && resolved.path == reflectionPath));
		}

		void RestrictReflectionItems(
			const ParseTree& tree,
			const ScopeIndex* external,
			std::size_t offset,
			std::vector<CompletionItem>& items)
		{
			const MemberResolver resolver(tree, external, offset);
			std::erase_if(items,[&](const CompletionItem& item)
				{
					if (item.kind == CompletionKind::Namespace ||
					(item.kind == CompletionKind::Variable && !item.has_location && item.type_text.empty()))
					{
						return !resolver.IsReflectionNamespace(item.label);
					}

					if (item.kind != CompletionKind::Variable && item.kind != CompletionKind::Function)
					{
						return true;
					}

					const auto value = resolver.CompletionValueType(item);
					if (resolver.HasReflectionIndirection(value))
					{
						return true;
					}

					const std::vector<std::string> reflectionPath
					{
						"std", "meta", "info"
					};
					const auto& text = value.text.empty() ? item.type_text : value.text;
					return !(IsReflectionValueType(text, resolver) || resolver.HasImplicitReflectionConversion(value) ||
					(value.ok && value.path == reflectionPath && value.args.empty() &&
					text.find('*') == std::string::npos && text.find('[') == std::string::npos));
				});
		}
	}

	std::vector<CompletionItem> CompletionEngine::Complete(
		const ParseTree& tree,
		const ParserOptions& options,
		std::size_t offset,
		const ScopeIndex* external)
	{
		return CompleteTree(tree, options, offset, external, false);
	}

	// Hover looks the hovered word up through completion: it skips the filters
	// that only make sense while typing (new names, access, constness).
	static std::vector<CompletionItem> CompleteTree(
		const ParseTree& tree,
		const ParserOptions& options,
		std::size_t offset,
		const ScopeIndex* external,
		bool for_hover)
	{
		const std::string_view source    = tree.Source();
		const std::vector<Token>& tokens = tree.Tokens();
		if (offset > source.size())
		{
			offset = source.size();
		}

		const std::string prefix    = CompletionEngine::PrefixAt(source, offset);
		const CursorContext context = ClassifyContext(source, tokens, offset, prefix);

		if (!for_hover)
		{
			if (const ParameterComment comment = ParameterCommentAt(source, tokens, offset); comment.ok)
			{
				return CompleteParameterComment(tree, comment, external);
			}
		}

		if (context == CursorContext::Suppressed)
			return {};

		// Use flat hash map with interned strings for better cache behavior
		StringInterner interner;
		FlatHashMap<CompletionItem> best;
		best.Reserve(512);

		if (context == CursorContext::Preprocessor)
		{
			// Preprocessor path uses string map for simplicity (less hot)
			std::unordered_map<std::string, CompletionItem> best_str;
			for (const auto directive : kDirectives)
			{
				if (prefix.empty() || StartsWith(directive, prefix))
				{
					InsertItem(
						best_str,
						{std::string(directive), CompletionKind::Directive, "directive", {}});
				}
			}

			CollectDefines(source, tokens, best_str, prefix);
			for (const auto& [name, value] : options.Macros())
			{
				if (!prefix.empty() && !StartsWith(name, prefix))
				{
					continue;
				}

				std::string detail = value.empty() ? "macro" : value;
				if (detail.size() > kMaxShortDetailLen)
				{
					detail.resize(kMaxShortDetailLen);
				}

				InsertItem(best_str, {name, CompletionKind::Macro, std::move(detail), {}});
			}

			// Merge string map into flat map
			for (auto& [label, item] : best_str)
			{
				InsertItem(interner, best, std::move(item));
			}
		}
		else
		{
			const std::vector<CallableInterval> callables           = BuildCallableIntervals(tree);
			const std::vector<std::vector<std::string>> scope_paths = BuildScopePaths(tree);
			if (context == CursorContext::MemberAccess)
			{
				CompleteMember(tree, offset, prefix, external, for_hover, interner, best);
			}
			else if (context == CursorContext::ScopeAccess)
			{
				const std::vector<ScopeInterval> named_scopes = BuildNamedScopeIntervals(tree);
				CompleteQualified(tree, source, tokens, scope_paths, callables, named_scopes,
					offset, prefix, external, interner, best);
			}
			else
			{
				// `int na|` inside a function body names a new variable.
				if (!for_hover && FindCallable(callables, offset) != NoIndex &&
					NamesNewDeclarator(source, tokens, offset, prefix))
				{
					return {};
				}

				const CursorScopes cursor = FindCursorScopes(tree, offset);
				CompleteExpression(tree, source, tokens, options, callables, scope_paths, offset,
					prefix, external, cursor, interner, best);
				CompleteImplicitMembers(tree, cursor, offset, prefix, external, for_hover, interner,
					best);
				if (!for_hover)
				{
					CompleteOverrides(tree, cursor, offset, prefix, external, interner, best);
				}
			}
		}

		auto items = CollectAndSort(best, interner);
		if (!for_hover && RequiresReflectionValue(source, tokens, offset))
		{
			RestrictReflectionItems(tree, external, offset, items);
		}

		return RankItems(std::move(items), prefix);
	}

	namespace
	{

		// Variables hover with the size/alignment of their declared type, types with
		// that of the type they name. Unknowable layouts leave the item untouched.
		void AttachLayout(
			const ParseTree& tree,
			const ParserOptions& options,
			const ScopeIndex* external,
			CompletionItem& item)
		{
			const bool tag =
				item.kind == CompletionKind::Namespace &&
				(item.detail.starts_with("struct ") || item.detail.starts_with("class ") ||
				item.detail.starts_with("union ") || item.detail.starts_with("enum "));
			if (item.kind != CompletionKind::Variable && item.kind != CompletionKind::Type && !tag)
			{
				return;
			}

			std::string text = item.layout_type;
			if (item.kind == CompletionKind::Variable)
			{
				if (text.empty())
				{
					return;
				}

				// `auto x = f();`: the refined detail holds the deduced specifier.
				const auto at =
					item.type_text.empty() ? std::string::npos : text.find(item.type_text);
				if (at != std::string::npos && item.type_text.find("auto") != std::string::npos &&
					!item.detail.empty())
				{
					text.replace(at, item.type_text.size(), item.detail);
				}
			}

			const ScopeIndex local = BuildScopeIndex(tree, false);
			const TypeLayoutResolver resolver(
				&local,
				external,
				LayoutTarget::FromMacros(
				options.Macros()));
			const auto layout = item.kind == CompletionKind::Variable ? resolver.OfType(text)
			: resolver.OfItem(item);
			if (layout)
			{
				item.has_layout  = true;
				item.size_bytes  = layout->size;
				item.align_bytes = layout->align;
			}

			if (item.kind == CompletionKind::Variable)
			{
				if (const auto offset = resolver.OffsetOfField(item))
				{
					item.has_field_offset = true;
					item.field_offset     = *offset;
				}
			}
			else if (const auto origin = resolver.OriginOf(item))
			{
				item.type_origin = origin->type;
				if (item.documentation.empty())
				{
					item.documentation = origin->documentation;
				}
			}
		}

		// `offsetof(Record, member)` evaluates to the member's offset; hovering the
		// macro name shows it. Only a plain identifier member is understood.
		std::optional<CompletionItem> OffsetofHover(
			const ParseTree& tree,
			const ParserOptions& options,
			const ScopeIndex* external,
			std::size_t end)
		{
			const std::string_view source = tree.Source();
			std::size_t i                 = end;
			while (i < source.size() && (source[i] == ' ' || source[i] == '\t'))
			{
				++i;
			}

			if (i >= source.size() || source[i] != '(')
			{
				return std::nullopt;
			}

			int depth         = 0;
			std::size_t comma = std::string_view::npos;
			std::size_t close = std::string_view::npos;
			for (std::size_t k = i; k < source.size() && close == std::string_view::npos; ++k)
			{
				const char c = source[k];
				if (c == '(' || c == '<' || c == '[')
				{
					++depth;
				}
				else if (c == ')' || c == '>' || c == ']')
				{
					if (--depth == 0)
					{
						close = k;
					}
				}
				else if (c == ',' && depth == 1 && comma == std::string_view::npos)
				{
					comma = k;
				}
				else if (c == ';' || c == '{')
				{
					return std::nullopt;
				}
			}

			if (comma == std::string_view::npos || close == std::string_view::npos)
			{
				return std::nullopt;
			}

			const auto trim = [](std::string_view text)
			{
				while (!text.empty() &&
					(text.front() == ' ' || text.front() == '\t' || text.front() == '\n'))
				{
					text.remove_prefix(1);
				}

				while (!text.empty() &&
					(text.back() == ' ' || text.back() == '\t' || text.back() == '\n'))
				{
					text.remove_suffix(1);
				}

				return text;
			};
			const std::string_view record = trim(source.substr(i + 1, comma - i - 1));
			const std::string_view member = trim(source.substr(comma + 1, close - comma - 1));
			if (record.empty() || member.empty() ||!IsIdentStart(member.front()) ||
				!std::all_of(member.begin(), member.end(),[](char c)
				{
					return IsIdentChar(c);
				}))
			{
				return std::nullopt;
			}

			const ScopeIndex local = BuildScopeIndex(tree, false);
			const TypeLayoutResolver resolver(
				&local,
				external,
				LayoutTarget::FromMacros(
				options.Macros()));
			const auto offset = resolver.OffsetOf(record, member);
			if (!offset)
			{
				return std::nullopt;
			}

			CompletionItem item {
				"offsetof",
				CompletionKind::Macro,
				"offsetof(" + std::string(record) + ", " + std::string(member) + ")", {}
			};
			item.has_field_offset = true;
			item.field_offset     = *offset;
			return item;
		}

	} // namespace

	std::string CompletionEngine::InferredType(
		const ParseTree& tree,
		std::size_t nameToken,
		const ScopeIndex* external)
	{
		if (nameToken >= tree.Tokens().size())
		{
			return {};
		}

		const MemberResolver resolver(tree, external, tree.Tokens()[nameToken].offset);
		return resolver.DeducedTypeText(nameToken);
	}

	std::string CompletionEngine::VariableType(
		const ParseTree& tree,
		std::size_t nameToken,
		const ScopeIndex* external)
	{
		if (nameToken >= tree.Tokens().size())
		{
			return {};
		}

		const MemberResolver resolver(tree, external, tree.Tokens()[nameToken].offset);
		return resolver.VariableTypeText(nameToken);
	}

	std::optional<CompletionItem> CompletionEngine::Hover(
		std::string_view source,
		const ParserOptions& options,
		std::size_t offset,
		const ScopeIndex* external)
	{
		return Hover(ParseTree::Parse(source, options), options, offset, external);
	}

	std::optional<CompletionItem> CompletionEngine::Hover(
		const ParseTree& tree,
		const ParserOptions& options,
		std::size_t offset,
		const ScopeIndex* external)
	{
		const std::string_view source = tree.Source();
		if (offset > source.size())
		{
			offset = source.size();
		}

		if (source.empty())
		{
			return std::nullopt;
		}

		if (source[offset < source.size() ? offset : source.size() - 1] == '^')
		{
			const auto& tokens = tree.Tokens();
			const auto& nodes  = tree.NodesSoA();
			for (std::size_t node = 0; node < nodes.size(); ++node)
			{
				if (nodes.Kind(node) != GrammarKind::UnaryExpression)
				{
					continue;
				}

				const auto first = nodes.FirstToken(node);
				if (first >= tokens.size() || tree.Text(tokens[first]) != "^^" ||
					offset < tokens[first].offset || offset >= tokens[first].offset + tokens[first].length)
				{
					continue;
				}

				const auto last = first + nodes.TokenCount(node);
				if (last <= first + 1 || last > tokens.size())
				{
					return std::nullopt;
				}

				const auto operandStart = tokens[first].offset + tokens[first].length;
				const auto operandEnd   = tokens[last - 1].offset + tokens[last - 1].length;
				const auto operand = CompactWs(std::string(source.substr(operandStart,
					operandEnd - operandStart)), kMaxTypeTextLen);
				CompletionItem hovered {"^^" + operand, CompletionKind::Type,
					"^^" + operand + ": std::meta::info", {}};
				hovered.documentation = "Reflection of `" + operand + "`.";
				if (operandEnd > operandStart &&
					IsIdentChar(source[operandEnd - 1]))
				{
					if (const auto declaration = Hover(tree, options, operandEnd - 1, external))
					{
						const auto description = declaration->detail.find(declaration->label) ==
							std::string::npos
						? declaration->label + ": " + declaration->detail
						: declaration->detail;
						hovered.documentation += "\n\nDeclaration: `" + description + "`.";
					}
				}

				return hovered;
			}
		}

		std::size_t start = offset;
		while (start > 0 && IsIdentChar(source[start - 1]))
		{
			--start;
		}

		std::size_t end = offset;
		while (end < source.size() && IsIdentChar(source[end]))
		{
			++end;
		}

		if (start == end)
		{
			return std::nullopt;
		}

		const std::string word(source.substr(start, end - start));
		if (!IsIdentStart(word.front()) ||(IsKeyword(word) && !IsBuiltinType(word)))
		{
			return std::nullopt;
		}

		if (IsBuiltinType(word))
		{
			CompletionItem hovered {word, CompletionKind::Type, word, {}};
			// Keep multiword specifiers together (`unsigned long long`,
			// `long double`) whichever component the cursor is over.
			const auto& tokens      = tree.Tokens();
			const auto& identifiers = tree.IdentifierTokens();
			const auto found = std::lower_bound(
				identifiers.begin(), identifiers.end(), start,
				[&](std::uint32_t token, std::size_t offset)
				{
					return tokens[token].offset < offset;
				});
			if (found == identifiers.end() || tokens[*found].offset != start)
			{
				return std::nullopt;
			}

			std::size_t first = *found;

			std::size_t last = first;
			while (first < tokens.size())
			{
				const auto previous = PreviousSignificant(tokens, first, source);
				if (previous >= tokens.size() ||!IsBuiltinType(tree.Text(tokens[previous])))
				{
					break;
				}

				first = previous;
			}

			while (last < tokens.size())
			{
				const auto next = NextSignificantToken(tokens, last);
				if (next >= tokens.size() ||!IsBuiltinType(tree.Text(tokens[next])))
				{
					break;
				}

				last = next;
			}

			if (first < tokens.size() && last < tokens.size())
			{
				hovered.detail = CompactWs(
					std::string(source.substr(
					tokens[first].offset,
					tokens[last].offset + tokens[last].length - tokens[first].offset)),
					kMaxTypeTextLen);
			}

			const TypeLayoutResolver resolver(
				nullptr,
				nullptr,
				LayoutTarget::FromMacros(
				options.Macros()));
			if (const auto layout = resolver.OfType(hovered.detail))
			{
				hovered.has_layout  = true;
				hovered.size_bytes  = layout->size;
				hovered.align_bytes = layout->align;
			}

			return hovered;
		}

		if (word == "offsetof")
		{
			if (auto folded = OffsetofHover(tree, options, external, end))
			{
				return folded;
			}
		}

		const auto items = CompleteTree(tree, options, end, external, true);
		for (const auto& item : items)
		{
			if (item.label == word)
			{
				auto hovered = item;
				RefineAutoDetail(tree, external, end, hovered);
				if (hovered.kind == CompletionKind::Function)
				{
					const MemberResolver resolver(tree, external, end);
					resolver.RefineFunctionAt(end, hovered);
				}

				AttachLayout(tree, options, external, hovered);
				return hovered;
			}
		}

		return std::nullopt;
	}

	SignatureHelp CompletionEngine::Signatures(
		std::string_view source,
		const ParserOptions& options,
		std::size_t offset,
		const ScopeIndex* external)
	{
		const ParseTree tree = ParseTree::Parse(source, options);
		return Signatures(tree, options, offset, external);
	}

	SignatureHelp CompletionEngine::Signatures(
		const ParseTree& tree,
		const ParserOptions& options,
		std::size_t offset,
		const ScopeIndex* external)
	{
		(void) options;
		const std::string_view source = tree.Source();
		if (offset > source.size())
		{
			return {};
		}

		// Inside a comment or a string literal there is no call to describe.
		const std::size_t at = TokenBefore(tree.Tokens(), offset);
		if (at < tree.Tokens().size())
		{
			const Token& token = tree.Tokens()[at];
			const bool inside = offset < token.offset + token.length ||
				(token.kind == TokenKind::LineComment) ||
				(token.kind == TokenKind::BlockComment && !TokenText(source, token).ends_with("*/"));
			if (inside && (token.kind == TokenKind::LineComment ||
				token.kind == TokenKind::BlockComment || token.kind == TokenKind::StringLiteral ||
				token.kind == TokenKind::CharacterLiteral ||
				token.kind == TokenKind::RawStringLiteral))
			{
				return {};
			}
		}

		if (InDisabledBranch(source, tree.Tokens(), offset))
		{
			return {};
		}

		return ComputeSignatures(tree, offset, external);
	}

	CompletionContext CompletionEngine::ContextAt(std::string_view source, std::size_t offset)
	{
		CompletionContext result;
		offset                          = std::min(offset, source.size());
		result.prefix                   = PrefixAt(source, offset);
		const std::vector<Token> tokens = Lexer(source).Lex();
		const CursorContext context     = ClassifyContext(source, tokens, offset, result.prefix);
		if (RequiresReflectionValue(source, tokens, offset))
		{
			return result;
		}

		if (context == CursorContext::Expression &&
			!NamesNewDeclarator(source, tokens, offset, result.prefix))
		{
			result.kind = CompletionContextKind::Unqualified;
		}
		else if (context == CursorContext::ScopeAccess)
		{
			const std::size_t scope_op = AccessOperatorBefore(source, tokens, offset, result.prefix);
			const Qualifier qualifier  = QualifierBefore(source, tokens, scope_op);
			if (!qualifier.unresolved)
			{
				result.kind      = CompletionContextKind::Qualified;
				result.qualifier = qualifier.path;
			}
		}

		return result;
	}

} // namespace heimdall
