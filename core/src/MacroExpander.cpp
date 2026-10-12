#include <Heimdall/MacroExpander.hpp>

#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace heimdall
{

	namespace
	{

		constexpr std::size_t kStopCheckInterval = 1024;
		constexpr unsigned kHalfWord = 32;
		constexpr unsigned kPointerAlignmentBits = 4;
		constexpr std::size_t kParenthesizedDefinedTokens = 3; // `(`, name, `)`
		constexpr std::size_t kVaOptOpen = 2; // `__VA_OPT__` and its `(`
		constexpr std::string_view kVaOpt        = "__VA_OPT__";

		struct Storage
		{
			std::deque<std::string> pool;
			std::vector<MacroPtr> used;
		};

		struct Item
		{
			std::string_view text;
			TokenKind kind           = TokenKind::Unknown;
			Tok tok                  = Tok::None;
			bool space               = false;
			bool placemarker         = false;
			std::uint32_t spelled    = kNoToken;
			std::uint32_t invocation = kNoToken;
			std::uint32_t hideSet    = 0;
		};

		using Items = std::vector<Item>;

		bool IsTrivia(TokenKind kind) noexcept
		{
			return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
				kind == TokenKind::BlockComment;
		}

		bool IsLiteral(TokenKind kind) noexcept
		{
			return kind == TokenKind::StringLiteral || kind == TokenKind::CharacterLiteral ||
				kind == TokenKind::RawStringLiteral;
		}

		class Run
		{
		public:
			Run(
				const MacroTable& table,
				const ExpansionLimits& limits,
				bool protectDefined,
				std::stop_token stop) :
				mTable(table), mLimits(limits), mProtectDefined(protectDefined),
				mStop(std::move(stop)), mStorage(std::make_shared<Storage>())
			{
				mHideSets.emplace_back();
				mHideSetIds.emplace(HideSet {}, 0);
			}

			ExpansionResult Execute(std::string_view text, std::span<const Token> tokens)
			{
				Items input;
				input.reserve(tokens.size());
				bool space = false;
				for (std::size_t i = 0; i < tokens.size(); ++i)
				{
					const Token& token = tokens[i];
					if (IsTrivia(token.kind))
					{
						space = true;
						continue;
					}

					Item item;
					item.text    = text.substr(token.offset, token.length);
					item.kind    = token.kind;
					item.tok     = token.tok;
					item.space   = space;
					item.spelled = static_cast<std::uint32_t>(i);
					input.push_back(item);
					space = false;
				}

				Items out = ExpandSequence(std::move(input), 0);
				ExpansionResult result;
				result.tokens.reserve(out.size());
				for (const Item& item : out)
				{
					if (!item.placemarker)
					{
						result.tokens.push_back({item.text, item.kind, item.tok, item.space,
								item.spelled, item.invocation});
					}
				}

				result.diagnostics = std::move(mDiagnostics);
				result.truncated   = mTruncated;
				result.storage     = std::move(mStorage);
				return result;
			}

		private:
			using HideSet = std::vector<const MacroDef*>;

			std::string_view Intern(std::string text)
			{
				mStorage->pool.push_back(std::move(text));
				return mStorage->pool.back();
			}

			std::uint32_t HideSetId(HideSet set)
			{
				const auto[it, inserted] =
					mHideSetIds.try_emplace(std::move(set), static_cast<std::uint32_t>(mHideSets.size()));
				if (inserted)
				{
					mHideSets.push_back(it->first);
				}

				return it->second;
			}

			bool Hidden(std::uint32_t id, const MacroDef* macro) const
			{
				return std::ranges::binary_search(mHideSets[id], macro);
			}

			static std::uint64_t PairKey(std::uint32_t a, std::uint32_t b) noexcept
			{
				return (static_cast<std::uint64_t>(a) << kHalfWord) | b;
			}

			std::uint32_t Union(std::uint32_t a, std::uint32_t b)
			{
				if (a == b || b == 0)
				{
					return a;
				}

				if (a == 0)
				{
					return b;
				}

				// The same few sets meet again and again; remember what they combine to.
				const auto [slot, fresh] = mUnions.try_emplace(PairKey(a, b), 0);
				if (!fresh)
				{
					return slot->second;
				}

				HideSet merged;
				std::ranges::set_union(mHideSets[a], mHideSets[b], std::back_inserter(merged));
				return slot->second = HideSetId(std::move(merged));
			}

			std::uint32_t Intersect(std::uint32_t a, std::uint32_t b)
			{
				if (a == b)
				{
					return a;
				}

				const auto [slot, fresh] = mIntersections.try_emplace(PairKey(a, b), 0);
				if (!fresh)
				{
					return slot->second;
				}

				HideSet common;
				std::ranges::set_intersection(mHideSets[a], mHideSets[b],
					std::back_inserter(common));
				return slot->second = HideSetId(std::move(common));
			}

			std::uint32_t Add(std::uint32_t id, const MacroDef* macro)
			{
				const auto key = PairKey(id, static_cast<std::uint32_t>(
					reinterpret_cast<std::uintptr_t>(macro) >> kPointerAlignmentBits));
				const auto [slot, fresh] = mAdditions.try_emplace(key, std::make_pair(macro, 0u));
				if (!fresh && slot->second.first == macro)
				{
					return slot->second.second;
				}

				HideSet set = mHideSets[id];
				set.insert(std::ranges::lower_bound(set, macro), macro);
				slot->second = { macro, HideSetId(std::move(set)) };
				return slot->second.second;
			}

			void Diagnose(const Item& at, std::string message)
			{
				const std::size_t where = at.invocation != kNoToken ? at.invocation
				: at.spelled != kNoToken ? at.spelled
				: 0;
				mDiagnostics.push_back({where, std::move(message)});
			}

			bool ShouldStop(std::size_t produced)
			{
				if (mTruncated)
				{
					return true;
				}

				if (produced > mLimits.maxTokens)
				{
					mDiagnostics.push_back({0, "macro expansion exceeds the token limit"});
					mTruncated = true;
				}
				else if (++mSteps % kStopCheckInterval == 0 && mStop.stop_requested())
				{
					mTruncated = true;
				}

				return mTruncated;
			}

			static bool IsPunct(const Item& item, std::string_view text) noexcept
			{
				return item.kind == TokenKind::Punctuation && item.text == text;
			}

			Items ExpandSequence(Items input, std::size_t depth)
			{
				if (depth > mLimits.maxDepth)
				{
					mDiagnostics.push_back({0, "macro argument nesting is too deep"});
					mTruncated = true;
					return input;
				}

				Items stack(input.rbegin(), input.rend());
				Items out;
				while (!stack.empty())
				{
					if (ShouldStop(out.size() + stack.size()))
					{
						break;
					}

					Item token = std::move(stack.back());
					stack.pop_back();
					if (token.kind != TokenKind::Identifier || token.placemarker)
					{
						out.push_back(std::move(token));
						continue;
					}

					if (mProtectDefined &&
						(token.text == "defined" || token.text.starts_with("__has_")))
					{
						const bool call = token.text != "defined";
						out.push_back(std::move(token));
						if (call)
						{
							CopyCallOperand(stack, out);
						}
						else
						{
							CopyDefinedOperand(stack, out);
						}

						continue;
					}

					const MacroDef* found = mTable.Find(token.text);
					if (found == nullptr || Hidden(token.hideSet, found))
					{
						out.push_back(std::move(token));
						continue;
					}

					const MacroPtr macro = mTable.Lookup(token.text);

					if (!macro->functionLike)
					{
						Remember(macro);
						Items body = Substitute(*macro, {}, token, Add(token.hideSet, macro.get()),
							depth);
						Push(stack, std::move(body));
						continue;
					}

					if (stack.empty() ||!IsPunct(stack.back(), "("))
					{
						out.push_back(std::move(token));
						continue;
					}

					std::vector<Items> arguments;
					Item closing;
					std::size_t end = 0;
					if (!CollectArguments(*macro, stack, arguments, closing, end))
					{
						Diagnose(token, "unterminated argument list invoking macro '" +
							macro->name + "'");
						out.push_back(std::move(token));
						continue;
					}

					if (!FitArguments(*macro, arguments))
					{
						Diagnose(token, "macro '" + macro->name + "' passed " +
							std::to_string(arguments.size()) + " arguments, but takes " +
							std::to_string(macro->params.size()));
						out.push_back(std::move(token));
						continue;
					}

					stack.resize(end);
					Remember(macro);
					const std::uint32_t hideSet =
						Add(Intersect(token.hideSet, closing.hideSet), macro.get());
					Items body = Substitute(*macro, arguments, token, hideSet, depth);
					Push(stack, std::move(body));
				}

				return out;
			}

			void Remember(const MacroPtr& macro)
			{
				if (mRemembered.insert(macro.get()).second)
				{
					mStorage->used.push_back(macro);
				}
			}

			static void Push(Items& stack, Items sequence)
			{
				stack.insert(stack.end(), std::make_move_iterator(sequence.rbegin()),
					std::make_move_iterator(sequence.rend()));
			}

			// `defined X` and `defined(X)` keep their operand unexpanded.
			static void CopyDefinedOperand(Items& stack, Items& out)
			{
				if (stack.empty())
				{
					return;
				}

				const bool parenthesized = IsPunct(stack.back(), "(");
				const std::size_t count  = parenthesized ? kParenthesizedDefinedTokens : 1;
				for (std::size_t i = 0; i < count && !stack.empty(); ++i)
				{
					out.push_back(std::move(stack.back()));
					stack.pop_back();
				}
			}

			// Splits the argument list that starts at the top of `stack` without consuming it;
			// `end` is the index of the closing parenthesis.
			// `__has_include(<x>)` and friends keep their whole parenthesized operand.
			static void CopyCallOperand(Items& stack, Items& out)
			{
				if (stack.empty() ||!IsPunct(stack.back(), "("))
				{
					return;
				}

				std::size_t level = 0;
				while (!stack.empty())
				{
					Item item = std::move(stack.back());
					stack.pop_back();
					if (IsPunct(item, "("))
					{
						++level;
					}
					else if (IsPunct(item, ")") && --level == 0)
					{
						out.push_back(std::move(item));
						return;
					}

					out.push_back(std::move(item));
				}
			}

			bool CollectArguments(
				const MacroDef& macro,
				const Items& stack,
				std::vector<Items>& arguments,
				Item& closing,
				std::size_t& end) const
			{
				std::size_t level = 0;
				bool closed       = false;
				for (std::size_t i = stack.size(); i-- > 0;)
				{
					if (IsPunct(stack[i], "("))
					{
						++level;
					}
					else if (IsPunct(stack[i], ")") && --level == 0)
					{
						end    = i;
						closed = true;
						break;
					}
				}

				if (!closed)
				{
					return false;
				}

				arguments.emplace_back();
				level = 1;
				for (std::size_t i = stack.size() - 1; i-- > end + 1;)
				{
					const Item& item = stack[i];
					if (IsPunct(item, "("))
					{
						++level;
					}
					else if (IsPunct(item, ")"))
					{
						--level;
					}
					else if (IsPunct(item, ",") && level == 1 &&
						(!macro.variadic || arguments.size() < macro.params.size()))
					{
						arguments.emplace_back();
						continue;
					}

					arguments.back().push_back(item);
				}

				closing = stack[end];
				return true;
			}

			static bool FitArguments(const MacroDef& macro, std::vector<Items>& arguments)
			{
				const std::size_t want = macro.params.size();
				if (want == 0)
				{
					return arguments.size() == 1 && arguments.front().empty()
					? (arguments.clear(), true)
					: false;
				}

				if (macro.variadic && arguments.size() + 1 == want)
				{
					arguments.emplace_back();
				}

				return arguments.size() == want;
			}

			std::optional<std::size_t> ParamIndex(const MacroDef& macro,
				std::string_view name) const
			{
				for (std::size_t i = 0; i < macro.params.size(); ++i)
				{
					if (macro.params[i] == name)
					{
						return i;
					}
				}

				return std::nullopt;
			}

			Item Stringify(const Items& argument, const Item& origin)
			{
				std::string text = "\"";
				bool first       = true;
				for (const Item& item : argument)
				{
					if (item.placemarker)
					{
						continue;
					}

					if (!first && item.space)
					{
						text += ' ';
					}

					first = false;
					if (IsLiteral(item.kind))
					{
						for (const char c : item.text)
						{
							if (c == '"' || c == '\\')
							{
								text += '\\';
							}

							text += c;
						}
					}
					else
					{
						text += item.text;
					}
				}

				text += '"';
				Item result;
				result.text       = Intern(std::move(text));
				result.kind       = TokenKind::StringLiteral;
				result.space      = origin.space;
				result.invocation = InvocationOf(origin);
				return result;
			}

			static std::uint32_t InvocationOf(const Item& token) noexcept
			{
				return token.invocation != kNoToken ? token.invocation : token.spelled;
			}

			// `left ## right` as one token, or both unchanged when the paste is invalid.
			void Paste(Items& out, Item right)
			{
				Item left = std::move(out.back());
				out.pop_back();
				if (left.placemarker)
				{
					out.push_back(std::move(right));
					return;
				}

				if (right.placemarker)
				{
					out.push_back(std::move(left));
					return;
				}

				const std::string_view joined  = Intern(std::string(left.text) + std::string(right.text));
				const std::vector<Token> lexed = Lexer(joined).Lex();
				if (lexed.size() == 1 && !IsTrivia(lexed[0].kind) && lexed[0].length == joined.size())
				{
					Item pasted = left;
					pasted.text = joined;
					pasted.kind = lexed[0].kind;
					pasted.tok  = lexed[0].tok;
					out.push_back(std::move(pasted));
					return;
				}

				Diagnose(left, "pasting \"" + std::string(left.text) + "\" and \"" +
					std::string(right.text) + "\" does not give a valid preprocessing token");
				out.push_back(std::move(left));
				out.push_back(std::move(right));
			}

			Items Substitute(
				const MacroDef& macro,
				const std::vector<Items>& arguments,
				const Item& invocation,
				std::uint32_t hideSet,
				std::size_t depth)
			{
				std::vector<std::optional<Items>> expanded(arguments.size());
				const std::uint32_t outermost = InvocationOf(invocation);
				Items out;
				Process(macro, arguments, expanded, invocation, outermost, 0, macro.tokens.size(),
					depth, out);
				Items result;
				result.reserve(out.size());
				for (Item& item : out)
				{
					item.hideSet = Union(item.hideSet, hideSet);
					if (item.invocation == kNoToken)
					{
						item.invocation = outermost;
					}

					result.push_back(std::move(item));
				}

				if (!result.empty())
				{
					result.front().space = invocation.space;
				}

				return result;
			}

			Item BodyItem(const MacroDef& macro, std::size_t index) const
			{
				const MacroBodyToken& body = macro.tokens[index];
				Item item;
				item.text = std::string_view(macro.body)
				.substr(body.token.offset, body.token.length);
				item.kind  = body.token.kind;
				item.tok   = body.token.tok;
				item.space = body.spaceBefore;
				return item;
			}

			static Item Placemarker()
			{
				Item item;
				item.placemarker = true;
				return item;
			}

			Items RawArgument(const Items& argument) const
			{
				if (argument.empty())
				{
					return {Placemarker()};
				}

				return argument;
			}

			const Items& ExpandedArgument(
				const std::vector<Items>& arguments,
				std::vector<std::optional<Items>>& cache,
				std::size_t index,
				std::size_t depth)
			{
				if (!cache[index])
				{
					cache[index] = ExpandSequence(arguments[index], depth + 1);
				}

				return *cache[index];
			}

			bool BodyIs(const MacroDef& macro, std::size_t index, std::string_view text) const
			{
				return index < macro.tokens.size() &&
					std::string_view(macro.body).substr(macro.tokens[index].token.offset,
					macro.tokens[index].token.length) == text;
			}

			void Process(
				const MacroDef& macro,
				const std::vector<Items>& arguments,
				std::vector<std::optional<Items>>& cache,
				const Item& invocation,
				std::uint32_t outermost,
				std::size_t begin,
				std::size_t end,
				std::size_t depth,
				Items& out)
			{
				for (std::size_t i = begin; i < end; ++i)
				{
					if (mTruncated)
					{
						return;
					}

					const Item body = BodyItem(macro, i);
					if (macro.functionLike && body.text == "#" && i + 1 < end)
					{
						if (const auto param = ParamIndex(macro, BodyItem(macro, i + 1).text))
						{
							out.push_back(Stringify(arguments[*param], invocation));
							++i;
							continue;
						}
					}

					if (body.text == "##" && i > begin && i + 1 < end)
					{
						++i;
						Items right;
						const Item rhs = BodyItem(macro, i);
						if (const auto param = macro.functionLike ? ParamIndex(macro, rhs.text)
							: std::nullopt)
						{
							right = RawArgument(arguments[*param]);
						}
						else
						{
							right.push_back(rhs);
						}

						if (out.empty())
						{
							out.push_back(Placemarker());
						}

						Paste(out, right.front());
						out.insert(out.end(), right.begin() + 1, right.end());
						continue;
					}

					if (macro.variadic && body.text == kVaOpt && BodyIs(macro, i + 1, "("))
					{
						std::size_t level = 0;
						std::size_t close = i + 1;
						for (; close < end; ++close)
						{
							if (BodyIs(macro, close, "("))
							{
								++level;
							}
							else if (BodyIs(macro, close, ")") && --level == 0)
							{
								break;
							}
						}

						if (close < end)
						{
							const Items& variadic = arguments.back();
							if (!variadic.empty())
							{
								Process(macro, arguments, cache, invocation, outermost, i + kVaOptOpen,
									close, depth, out);
							}

							i = close;
							continue;
						}
					}

					if (const auto param = macro.functionLike ? ParamIndex(macro, body.text)
						: std::nullopt;
						param && body.kind == TokenKind::Identifier)
					{
						const bool pasteNext = BodyIs(macro, i + 1, "##");
						Items argument = pasteNext
						? RawArgument(arguments[*param])
						: ExpandedArgument(arguments, cache, *param, depth);
						if (!argument.empty())
						{
							argument.front().space = body.space;
						}

						out.insert(out.end(), std::make_move_iterator(argument.begin()),
							std::make_move_iterator(argument.end()));
						continue;
					}

					out.push_back(body);
				}
			}

			const MacroTable& mTable;
			ExpansionLimits mLimits;
			bool mProtectDefined;
			std::stop_token mStop;
			std::shared_ptr<Storage> mStorage;
			std::vector<HideSet> mHideSets;
			std::map<HideSet, std::uint32_t> mHideSetIds;
			std::unordered_map<std::uint64_t, std::uint32_t> mUnions;
			std::unordered_map<std::uint64_t, std::uint32_t> mIntersections;
			std::unordered_map<std::uint64_t, std::pair<const MacroDef*, std::uint32_t>> mAdditions;
			std::unordered_set<const MacroDef*> mRemembered;
			std::vector<PreprocessorDiagnostic> mDiagnostics;
			std::size_t mSteps = 0;
			bool mTruncated    = false;
		};

	} // namespace

	ExpansionResult MacroExpander::Expand(std::string_view text, bool protectDefined,
		std::stop_token stop) const
	{
		const std::vector<Token> tokens = Lexer(text).Lex();
		return Expand(text, tokens, protectDefined, std::move(stop));
	}

	ExpansionResult MacroExpander::Expand(
		std::string_view text,
		std::span<const Token> tokens,
		bool protectDefined,
		std::stop_token stop) const
	{
		Run run(
			mTable,
			mLimits,
			protectDefined,
			std::move(stop));
		return run.Execute(text, tokens);
	}

} // namespace heimdall
