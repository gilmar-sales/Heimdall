#include <Heimdall/SyntaxHighlight.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <Heimdall/SymbolOutline.hpp>

namespace heimdall
{

	namespace
	{

		constexpr std::array<std::string_view, static_cast<std::size_t>(HighlightKind::Count) >
		kKindNames = {"keyword", "number", "string", "comment", "operator", "macro",
			"namespace", "type", "class", "struct", "enum", "enumMember",
			"function", "method", "property", "variable", "parameter"};

		constexpr std::size_t kTypeKeywordCount = 15;
		constexpr std::size_t kTokensPerSpan    = 2;
		constexpr std::size_t kMaxDirectiveGap  = 2;
		constexpr std::array<std::string_view, kTypeKeywordCount> kTypeKeywords = {
			"auto", "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float",
			"int", "long", "short", "signed", "unsigned", "void", "wchar_t"
		};

		struct NameKind
		{
			HighlightKind kind = HighlightKind::Variable;
			bool ambiguous     = false;
		};

		bool IsTrivia(TokenKind kind) noexcept
		{
			return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
				kind == TokenKind::BlockComment;
		}

		bool IsWordStart(char c) noexcept
		{
			return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') || c == '_';
		}

		bool IsCallable(HighlightKind kind) noexcept
		{
			return kind == HighlightKind::Function || kind == HighlightKind::Method;
		}

		bool IsTypeKeyword(std::string_view text) noexcept
		{
			return std::ranges::find(kTypeKeywords, text) != kTypeKeywords.end();
		}

		bool IsOperatorText(std::string_view text) noexcept
		{
			return !(text == "{" || text == "}" || text == "[" || text == "]" || text == "(" ||
				text == ")" || text == ";" || text == "," || text == "." || text == ":" ||
				text == "::" || text == "#" || text == "##" || text == "\\" || text == "...");
		}

		// Kind of a declared name, or nullopt when the name does not classify uses.
		std::optional<HighlightKind> DeclaredKind(OutlineKind kind) noexcept
		{
			switch (kind)
			{
			case OutlineKind::Namespace:
				return HighlightKind::Namespace;
			case OutlineKind::Class:
				return HighlightKind::Class;
			case OutlineKind::Struct:
			case OutlineKind::Union:
				return HighlightKind::Struct;
			case OutlineKind::Enum:
				return HighlightKind::Enum;
			case OutlineKind::EnumMember:
				return HighlightKind::EnumMember;
			case OutlineKind::Function:
				return HighlightKind::Function;
			case OutlineKind::Method:
				return HighlightKind::Method;
			case OutlineKind::Field:
				return HighlightKind::Property;
			case OutlineKind::Variable:
				return HighlightKind::Variable;
			case OutlineKind::TypeAlias:
			case OutlineKind::Concept:
				return HighlightKind::Type;
			case OutlineKind::Constructor:
			case OutlineKind::Destructor:
			case OutlineKind::Operator:
				return std::nullopt;
			}

			return std::nullopt;
		}

		// Types are named by the first identifier of a possibly decorated name
		// (`Foo<int>`); everything else by the last one (`Widget::Run`).
		bool NamedByFirstIdentifier(OutlineKind kind) noexcept
		{
			return kind == OutlineKind::Namespace || kind == OutlineKind::Class ||
				kind == OutlineKind::Struct || kind == OutlineKind::Union ||
				kind == OutlineKind::Enum || kind == OutlineKind::TypeAlias ||
				kind == OutlineKind::Concept;
		}

		class Highlighter
		{
		public:
			Highlighter(const ParseTree& tree, const HighlightContext* context) :
			mSource(tree.Source()), mTokens(tree.Tokens()), mContext(context) {}

			std::vector<HighlightSpan> Run(const ParseTree& tree)
			{
				mSpans.reserve(mTokens.size() / kTokensPerSpan);
				CollectDeclarations(SymbolOutline::Extract(tree));
				mTypeNames = tree.TypeNames();
				Walk();
				return std::move(mSpans);
			}

		private:
			std::string_view Text(std::size_t index) const noexcept
			{
				const Token& token = mTokens[index];
				return mSource.substr(token.offset, token.length);
			}

			// True when `offset` lies in a group excluded by conditional compilation. Offsets
			// are queried in non-decreasing order, so one cursor walks the ranges once.
			bool Inactive(std::size_t offset)
			{
				if (mContext == nullptr || mContext->activeRanges.empty())
				{
					return false;
				}

				const auto ranges = mContext->activeRanges;
				while (mActiveCursor < ranges.size() &&
					ranges[mActiveCursor].offset + ranges[mActiveCursor].length <= offset)
				{
					++mActiveCursor;
				}

				return mActiveCursor == ranges.size() || offset < ranges[mActiveCursor].offset;
			}

			void Emit(
				std::size_t offset,
				std::size_t length,
				HighlightKind kind,
				std::uint8_t modifiers = 0)
			{
				if (length != 0)
				{
					// Conditional directive lines (`#if`/`#else`/`#endif`, ...) are
					// always compiled text (as in MacroScanner): never dim them,
					// even when the standalone preprocessor leaves them out of the
					// active ranges.
					if (!mLineConditional && Inactive(offset))
					{
						modifiers |= kHighlightInactive;
					}

					mSpans.push_back({offset, length, kind, modifiers});
				}
			}

			std::size_t LowerBound(std::size_t offset) const noexcept
			{
				const auto it = std::ranges::lower_bound(
					mTokens, offset, {},
					[](const Token& token)->std::size_t
					{
						return token.offset;
				});
				return static_cast<std::size_t>(it - mTokens.begin());
			}

			bool IsIdentifier(std::size_t index) const noexcept
			{
				return mTokens[index].kind == TokenKind::Identifier &&
					mTokens[index].tok == Tok::None;
			}

			void CollectDeclarations(const std::vector<OutlineSymbol>& symbols)
			{
				for (const OutlineSymbol& symbol : symbols)
				{
					const auto kind = DeclaredKind(symbol.kind);
					if (!kind)
					{
						continue;
					}

					const std::size_t end = symbol.nameOffset + symbol.nameLength;
					std::optional<std::size_t> chosen;
					for (std::size_t i = LowerBound(symbol.nameOffset);
						i < mTokens.size() && mTokens[i].offset < end; ++i)
					{
						if (!IsIdentifier(i))
						{
							continue;
						}

						chosen = i;
						if (NamedByFirstIdentifier(symbol.kind))
						{
							break;
						}
					}

					if (!chosen)
					{
						continue;
					}

					mDeclarations.emplace(*chosen, *kind);
					Record(Text(*chosen), *kind);
				}
			}

			void Record(std::string_view name, HighlightKind kind)
			{
				const auto[it, inserted] = mNames.try_emplace(name, NameKind {kind, false});
				if (inserted || it->second.kind == kind)
				{
					return;
				}

				if (IsCallable(it->second.kind) && IsCallable(kind))
				{
					it->second.kind = HighlightKind::Function;
					return;
				}

				it->second.ambiguous = true;
			}

			// Index of the next token that is neither whitespace nor a comment.
			std::size_t NextSignificant(std::size_t index) const noexcept
			{
				std::size_t next = index + 1;
				while (next < mTokens.size() && IsTrivia(mTokens[next].kind))
				{
					++next;
				}

				return next;
			}

			bool NextIs(std::size_t index, std::string_view text) const noexcept
			{
				const std::size_t next = NextSignificant(index);
				return next < mTokens.size() && Text(next) == text;
			}

			bool IsCall(std::size_t index) const noexcept
			{
				const auto next = NextSignificant(index);
				if (next >= mTokens.size() || Text(next) != "<")
				{
					return NextIs(index, "(");
				}

				std::size_t depth           = 1;
				std::size_t expressionDepth = 0;
				for (auto i = NextSignificant(next); i < mTokens.size(); i = NextSignificant(i))
				{
					const auto text = Text(i);
					if (text == ";")
					{
						return false;
					}

					if (text == "(" || text == "[" || text == "{")
					{
						++expressionDepth;
					}
					else if (text == ")" || text == "]" || text == "}")
					{
						if (expressionDepth == 0)
						{
							return false;
						}

						--expressionDepth;
					}
					else if (expressionDepth == 0 && text == "<")
					{
						++depth;
					}
					else if (expressionDepth == 0 && (text == ">" || text == ">>"))
					{
						const std::size_t closing = text.size();
						if (closing > depth)
						{
							return false;
						}

						depth -= closing;
						if (depth == 0)
						{
							return NextIs(i, "(");
						}
					}
				}

				return false;
			}

			std::optional<HighlightKind> ClassifyIdentifier(std::size_t index) const
			{
				const std::string_view text = Text(index);
				if (mMacros.contains(text))
				{
					return HighlightKind::Macro;
				}

				const bool call = IsCall(index);
				auto previous   = mPrevSignificant;
				if (previous && Text(*previous) == "template")
				{
					while (*previous > 0)
					{
						-- * previous;
						if (!IsTrivia(mTokens[*previous].kind))
						{
							break;
						}
					}
				}

				if (previous && (Text(*previous) == "." || Text(*previous) == "->"))
				{
					return call ? HighlightKind::Method : HighlightKind::Property;
				}

				if (const auto it = mNames.find(text); it != mNames.end() && !it->second.ambiguous)
				{
					return it->second.kind;
				}

				if (NextIs(index, "::"))
				{
					const bool lower = text[0] >= 'a' && text[0] <= 'z';
					return lower ? HighlightKind::Namespace : HighlightKind::Type;
				}

				if (mTypeNames && mTypeNames->IsType(text))
				{
					return HighlightKind::Type;
				}

				if (call && !FollowsTypeName())
				{
					return HighlightKind::Function;
				}

				return std::nullopt;
			}

			// `Type name(` declares a variable; it is not a call.
			bool FollowsTypeName() const
			{
				if (!mPrevSignificant)
				{
					return false;
				}

				const Token& previous = mTokens[*mPrevSignificant];
				return (previous.kind == TokenKind::Identifier && previous.tok == Tok::None) ||
					(previous.kind == TokenKind::Identifier&& IsTypeKeyword(Text(*mPrevSignificant)));
			}

			// Handles the token after `#`; returns the index of the last token consumed.
			std::size_t Directive(std::size_t hash)
			{
				const std::size_t name = NextSignificant(hash);
				if (name >= mTokens.size() || mTokens[name].kind != TokenKind::Identifier ||
					name - hash > kMaxDirectiveGap)
				{
					Emit(mTokens[hash].offset, mTokens[hash].length, HighlightKind::Macro);
					return hash;
				}

				const std::string_view word = Text(name);
				mLineConditional = word == "if" || word == "ifdef" || word == "ifndef" ||
					word == "elif" || word == "elifdef" || word == "elifndef" ||
					word == "else" || word == "endif";
				Emit(mTokens[hash].offset, mTokens[name].offset + mTokens[name].length -
					mTokens[hash].offset, HighlightKind::Macro);
				mExpectMacroName = word == "define" || word == "undef" || word == "ifdef" ||
					word == "ifndef";
				mExpectDefine = word == "define";
				mExpectHeader = word == "include" || word == "import";
				return name;
			}

			// `<...>` after #include is one string; returns the last token consumed.
			std::size_t HeaderName(std::size_t open)
			{
				std::size_t last = open;
				for (std::size_t i = open + 1; i < mTokens.size(); ++i)
				{
					if (mTokens[i].kind == TokenKind::Whitespace &&
						Text(i).find('\n') != std::string_view::npos)
					{
						break;
					}

					last = i;
					if (Text(i) == ">")
					{
						break;
					}
				}

				Emit(mTokens[open].offset,
					mTokens[last].offset + mTokens[last].length - mTokens[open].offset,
					HighlightKind::String);
				return last;
			}

			void Walk()
			{
				bool lineStart = true;
				for (std::size_t i = 0; i < mTokens.size(); ++i)
				{
					const Token& token = mTokens[i];
					switch (token.kind)
					{
					case TokenKind::Whitespace:
						if (Text(i).find('\n') != std::string_view::npos)
						{
							lineStart = true;
							mExpectMacroName = mExpectDefine = mExpectHeader = false;
							mLineConditional = false;
						}

						continue;
					case TokenKind::LineComment:
					case TokenKind::BlockComment:
						Emit(token.offset, token.length, HighlightKind::Comment);
						continue;
					case TokenKind::Number:
						Emit(token.offset, token.length, HighlightKind::Number);
						break;
					case TokenKind::StringLiteral:
					case TokenKind::CharacterLiteral:
					case TokenKind::RawStringLiteral:
						Emit(token.offset, token.length, HighlightKind::String);
						break;
					case TokenKind::Punctuation:
						if (lineStart && (token.tok == Tok::Hash || token.tok == Tok::DigraphHash))
						{
							i                = Directive(i);
							lineStart        = false;
							mPrevSignificant = i;
							continue;
						}

						if (mExpectHeader && Text(i) == "<")
						{
							i             = HeaderName(i);
							mExpectHeader = false;
						}
						else if (IsOperatorText(Text(i)))
						{
							Emit(token.offset, token.length, HighlightKind::Operator);
						}

						break;
					case TokenKind::Identifier:
						Identifier(i);
						break;
					case TokenKind::Unknown:
						break;
					}

					lineStart        = false;
					mPrevSignificant = i;
				}
			}

			void Identifier(std::size_t index)
			{
				const Token& token = mTokens[index];
				if (token.tok != Tok::None)
				{
					if (IsWordStart(Text(index)[0]))
					{
						Emit(token.offset, token.length, HighlightKind::Keyword);
					}

					return;
				}

				if (mExpectMacroName)
				{
					const std::string_view name = Text(index);
					if (mExpectDefine)
					{
						mMacros.insert(name);
					}

					mExpectMacroName = false;
					Emit(token.offset, token.length, HighlightKind::Macro,
						mExpectDefine ? kHighlightDeclaration : 0);
					return;
				}

				if (mContext != nullptr && mContext->findMacro &&
					mContext->findMacro(Text(index), token.offset) != nullptr)
				{
					Emit(token.offset, token.length, HighlightKind::Macro);
					return;
				}

				if (const auto declared = mDeclarations.find(index); declared != mDeclarations.end())
				{
					Emit(token.offset, token.length, declared->second, kHighlightDeclaration);
					return;
				}

				if (const auto kind = ClassifyIdentifier(index))
				{
					Emit(token.offset, token.length, *kind);
					return;
				}

				// Identifiers without a reliable kind (locals, parameters) stay
				// unclassified in live code so the client's TextMate grammar applies.
				// In inactive groups emit a generic span so the client can dim the
				// whole region instead of leaving holes in the highlighting.
				if (mContext != nullptr && !mContext->activeRanges.empty() &&
					Inactive(token.offset))
				{
					Emit(token.offset, token.length, HighlightKind::Variable);
				}
			}

			std::string_view mSource;
			const std::vector<Token>& mTokens;
			std::vector<HighlightSpan> mSpans;
			std::unordered_map<std::size_t, HighlightKind> mDeclarations;
			std::unordered_map<std::string_view, NameKind> mNames;
			std::unordered_set<std::string_view> mMacros;
			std::shared_ptr<const TypeNameOracle> mTypeNames;
			std::optional<std::size_t> mPrevSignificant;
			bool mExpectMacroName            = false;
			bool mExpectDefine               = false;
			bool mExpectHeader               = false;
			bool mLineConditional            = false;
			const HighlightContext* mContext = nullptr;
			std::size_t mActiveCursor        = 0;
		};

	} // namespace

	std::string_view HighlightKindName(HighlightKind kind) noexcept
	{
		const auto index = static_cast<std::size_t>(kind);
		return index < kKindNames.size() ? kKindNames[index] : std::string_view {};
	}

	std::vector<HighlightSpan> SyntaxHighlight::Extract(const ParseTree& tree)
	{
		Highlighter highlighter(tree, nullptr);
		return highlighter.Run(tree);
	}

	std::vector<HighlightSpan> SyntaxHighlight::Extract(
		const ParseTree& tree,
		const HighlightContext& context)
	{
		Highlighter highlighter(tree, &context);
		return highlighter.Run(tree);
	}

} // namespace heimdall
