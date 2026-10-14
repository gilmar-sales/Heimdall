#include <Heimdall/MacroScanner.hpp>

#include <algorithm>
#include <cstdint>

#include <Heimdall/MacroExpander.hpp>

namespace heimdall
{

	namespace
	{

		constexpr std::size_t kMaxExpressionDepth     = 128;
		constexpr std::size_t kAttributeBracketLength = 2; // `[[`
		constexpr long long kValueBits                = 64;
		constexpr unsigned kDecimal                   = 10;
		constexpr unsigned kHexadecimal               = 16;
		constexpr unsigned kBinary                    = 2;
		constexpr unsigned kOctal                     = 8;
		constexpr unsigned kNotADigit                 = 99;
		constexpr std::size_t kEscapeOffset           = 2; // quote, backslash, then the escape letter
		constexpr std::size_t kBasePrefixLength       = 2; // `0x`, `0b`
		constexpr std::size_t kDelimiterPair          = 2; // opening and closing delimiter

		bool IsBlank(char c) noexcept
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
		}

		bool IsIdentChar(char c) noexcept
		{
			return (c >= 'a' && c <= 'z') ||(c >= 'A' && c <= 'Z') || c == '_' ||
				(c >= '0' && c <= '9');
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

		std::string_view ReadWord(std::string_view& text) noexcept
		{
			text          = Trim(text);
			std::size_t n = 0;
			while (n < text.size() && IsIdentChar(text[n]))
			{
				++n;
			}

			const std::string_view word = text.substr(0, n);
			text.remove_prefix(n);
			return word;
		}

		bool EndsWithBackslash(std::string_view line) noexcept
		{
			return !line.empty() && line.back() == '\\';
		}

		// Advances the block-comment state across `line`; true when code other than blanks and
		// comments appears on it.
		bool ScanLine(std::string_view line, bool& inBlock) noexcept
		{
			bool code = false;
			for (std::size_t i = 0; i < line.size(); ++i)
			{
				const char c = line[i];
				if (inBlock)
				{
					if (c == '*' && i + 1 < line.size() && line[i + 1] == '/')
					{
						inBlock = false;
						++i;
					}

					continue;
				}

				if (c == '/' && i + 1 < line.size() && line[i + 1] == '*')
				{
					inBlock = true;
					++i;
				}
				else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
				{
					break;
				}
				else if (c == '"' || c == '\'')
				{
					code = true;
					for (++i; i < line.size() && line[i] != c; ++i)
					{
						if (line[i] == '\\')
						{
							++i;
						}
					}
				}
				else if (!IsBlank(c))
				{
					code = true;
				}
			}

			return code;
		}

		DirectiveKind KindOfDirective(std::string_view name) noexcept
		{
			if (name == "include" || name == "include_next" || name == "import")
			{
				return DirectiveKind::Include;
			}

			if (name == "define")
			{
				return DirectiveKind::Define;
			}

			if (name == "undef")
			{
				return DirectiveKind::Undef;
			}

			if (name == "if")
			{
				return DirectiveKind::If;
			}

			if (name == "ifdef")
			{
				return DirectiveKind::Ifdef;
			}

			if (name == "ifndef")
			{
				return DirectiveKind::Ifndef;
			}

			if (name == "elif" || name == "elifdef" || name == "elifndef")
			{
				return DirectiveKind::Elif;
			}

			if (name == "else")
			{
				return DirectiveKind::Else;
			}

			if (name == "endif")
			{
				return DirectiveKind::Endif;
			}

			return name == "pragma" ? DirectiveKind::Pragma : DirectiveKind::Other;
		}

		struct Frame
		{
			bool parentActive = true;
			bool active       = true;
			bool branchTaken  = false;
			bool sawElse      = false;
		};

		// Feature-test values of the standard attributes: the year and month the attribute
		// reached the language.
		constexpr long long kAttributeCpp11 = 200809;
		constexpr long long kAttributeCpp14 = 201309;
		constexpr long long kAttributeCpp17 = 201603;
		constexpr long long kAttributeCpp20 = 201803;
		constexpr long long kAttributeCpp23 = 202207;
		constexpr long long kAttributeCpp26 = 202403;

		std::optional<long long> BuiltinFeature(
			std::string_view feature,
			std::string_view operand)
		{
			if (feature == "__has_cpp_attribute")
			{
				struct Known
				{
					std::string_view name;
					long long value;
				};

				constexpr Known kAttributes[] = {
					{
						"noreturn", kAttributeCpp11
					}, {"carries_dependency", kAttributeCpp11},
					{"deprecated", kAttributeCpp14}, {"fallthrough", kAttributeCpp17},
					{"maybe_unused", kAttributeCpp17}, {"nodiscard", kAttributeCpp17},
					{"likely", kAttributeCpp20}, {"unlikely", kAttributeCpp20},
					{"no_unique_address", kAttributeCpp20}, {"assume", kAttributeCpp23},
					{"indeterminate", kAttributeCpp26}
				};
				std::string_view name = operand;
				if (name.starts_with("[["))
				{
					name.remove_prefix(kAttributeBracketLength);
				}

				for (const Known& known : kAttributes)
				{
					if (known.name == name)
					{
						return known.value;
					}
				}
			}

			return std::nullopt;
		}

		class Condition
		{
		public:
			Condition(
				const std::vector<ExpandedToken>& tokens,
				const MacroTable& table,
				const MacroScanCallbacks& callbacks) :
				mTokens(tokens), mTable(table), mCallbacks(callbacks) {}

			bool Evaluate()
			{
				const long long value = Ternary();
				return !mError && mPos == mTokens.size() && value != 0;
			}

			[[nodiscard]] const std::string& Error() const noexcept
			{
				return mMessage;
			}

		private:
			using Value = long long;

			static Value Wrap(std::uint64_t value) noexcept
			{
				return static_cast<Value>(value);
			}

			static std::uint64_t Bits(Value value) noexcept
			{
				return static_cast<std::uint64_t>(value);
			}

			bool Peek(std::string_view text) const noexcept
			{
				return mPos < mTokens.size() && mTokens[mPos].kind == TokenKind::Punctuation &&
					mTokens[mPos].text == text;
			}

			bool Accept(std::string_view text) noexcept
			{
				if (Peek(text))
				{
					++mPos;
					return true;
				}

				return false;
			}

			void Fail(std::string message)
			{
				if (!mError)
				{
					mError   = true;
					mMessage = std::move(message);
				}
			}

			Value Ternary()
			{
				if (++mDepth > kMaxExpressionDepth)
				{
					Fail("maximum preprocessor expression nesting exceeded");
					--mDepth;
					return 0;
				}

				const Value condition = LogicalOr();
				Value result          = condition;
				if (Accept("?"))
				{
					const Value yes = Ternary();
					if (!Accept(":"))
					{
						Fail("expected ':' in conditional expression");
					}

					const Value no = Ternary();
					result         = condition != 0 ? yes : no;
				}

				--mDepth;
				return result;
			}

			Value LogicalOr()
			{
				Value left = LogicalAnd();
				while (Accept("||"))
				{
					const Value right = LogicalAnd();
					left              = (left != 0 || right != 0) ? 1 : 0;
				}

				return left;
			}

			Value LogicalAnd()
			{
				Value left = BitOr();
				while (Accept("&&"))
				{
					const Value right = BitOr();
					left              = (left != 0 && right != 0) ? 1 : 0;
				}

				return left;
			}

			Value BitOr()
			{
				Value left = BitXor();
				while (Peek("|"))
				{
					++mPos;
					left = Wrap(Bits(left) | Bits(BitXor()));
				}

				return left;
			}

			Value BitXor()
			{
				Value left = BitAnd();
				while (Accept("^"))
				{
					left = Wrap(Bits(left) ^ Bits(BitAnd()));
				}

				return left;
			}

			Value BitAnd()
			{
				Value left = Equality();
				while (Peek("&"))
				{
					++mPos;
					left = Wrap(Bits(left) & Bits(Equality()));
				}

				return left;
			}

			Value Equality()
			{
				Value left = Relational();
				while (true)
				{
					if (Accept("=="))
					{
						left = left == Relational() ? 1 : 0;
					}
					else if (Accept("!="))
					{
						left = left != Relational() ? 1 : 0;
					}
					else
					{
						return left;
					}
				}
			}

			Value Relational()
			{
				Value left = Shift();
				while (true)
				{
					if (Accept("<="))
					{
						left = left <= Shift() ? 1 : 0;
					}
					else if (Accept(">="))
					{
						left = left >= Shift() ? 1 : 0;
					}
					else if (Accept("<"))
					{
						left = left < Shift() ? 1 : 0;
					}
					else if (Accept(">"))
					{
						left = left > Shift() ? 1 : 0;
					}
					else
					{
						return left;
					}
				}
			}

			Value Shift()
			{
				Value left = Additive();
				while (true)
				{
					const bool left_shift = Peek("<<");
					if (!left_shift && !Peek(">>"))
					{
						return left;
					}

					++mPos;
					const Value amount = Additive();
					if (amount < 0 || amount >= kValueBits)
					{
						left = 0;
					}
					else
					{
						left = left_shift ? Wrap(Bits(left) << amount) : left >> amount;
					}
				}
			}

			Value Additive()
			{
				Value left = Multiplicative();
				while (true)
				{
					if (Peek("+"))
					{
						++mPos;
						left = Wrap(Bits(left) + Bits(Multiplicative()));
					}
					else if (Peek("-"))
					{
						++mPos;
						left = Wrap(Bits(left) - Bits(Multiplicative()));
					}
					else
					{
						return left;
					}
				}
			}

			Value Multiplicative()
			{
				Value left = Unary();
				while (true)
				{
					const bool multiply = Peek("*");
					const bool divide   = Peek("/");
					const bool modulo   = Peek("%");
					if (!multiply && !divide && !modulo)
					{
						return left;
					}

					++mPos;
					const Value right = Unary();
					if (multiply)
					{
						left = Wrap(Bits(left) * Bits(right));
					}
					else if (right == 0)
					{
						Fail("division by zero in preprocessor expression");
						left = 0;
					}
					else if (right == -1)
					{
						left = divide ? Wrap(0 - Bits(left)) : 0;
					}
					else
					{
						left = divide ? left / right : left % right;
					}
				}
			}

			Value Unary()
			{
				if (Accept("!"))
				{
					return Unary() == 0 ? 1 : 0;
				}

				if (Accept("~"))
				{
					return Wrap(~Bits(Unary()));
				}

				if (Accept("-"))
				{
					return Wrap(0 - Bits(Unary()));
				}

				if (Accept("+"))
				{
					return Unary();
				}

				return Primary();
			}

			static std::optional<Value> ParseNumber(std::string_view text)
			{
				std::string digits;
				for (const char c : text)
				{
					if (c != '\'')
					{
						digits += c;
					}
				}

				while (!digits.empty() && (digits.back() == 'u' || digits.back() == 'U' ||
					digits.back() == 'l' || digits.back() == 'L' ||
					digits.back() == 'z' || digits.back() == 'Z'))
				{
					digits.pop_back();
				}

				unsigned base = kDecimal;
				std::size_t i = 0;
				if (digits.size() > 1 && digits[0] == '0')
				{
					if (digits[1] == 'x' || digits[1] == 'X')
					{
						base = kHexadecimal;
						i    = kBasePrefixLength;
					}
					else if (digits[1] == 'b' || digits[1] == 'B')
					{
						base = kBinary;
						i    = kBasePrefixLength;
					}
					else
					{
						base = kOctal;
						i    = 1;
					}
				}

				if (i >= digits.size())
				{
					return digits.size() == 1 && digits[0] == '0' ? std::optional<Value>(0)
					: std::nullopt;
				}

				std::uint64_t value = 0;
				for (; i < digits.size(); ++i)
				{
					const char c   = digits[i];
					unsigned digit = kNotADigit;
					if (c >= '0' && c <= '9')
					{
						digit = static_cast<unsigned>(c - '0');
					}
					else if (c >= 'a' && c <= 'f')
					{
						digit = static_cast<unsigned>(c - 'a') + kDecimal;
					}
					else if (c >= 'A' && c <= 'F')
					{
						digit = static_cast<unsigned>(c - 'A') + kDecimal;
					}

					if (digit >= base)
					{
						return std::nullopt;
					}

					value = value * base + digit;
				}

				return Wrap(value);
			}

			static Value CharValue(std::string_view text)
			{
				const std::size_t open = text.find('\'');
				if (open == std::string_view::npos || open + 1 >= text.size())
				{
					return 0;
				}

				const char c = text[open + 1];
				if (c != '\\' || open + kEscapeOffset >= text.size())
				{
					return static_cast<unsigned char>(c);
				}

				switch (text[open + kEscapeOffset])
				{
				case 'n':
					return '\n';
				case 't':
					return '\t';
				case 'r':
					return '\r';
				case '0':
					return 0;
				case 'a':
					return '\a';
				case 'b':
					return '\b';
				case 'f':
					return '\f';
				case 'v':
					return '\v';
				default:
					return static_cast<unsigned char>(text[open + kEscapeOffset]);
				}
			}

			// The operand text of a `__has_*(...)` call: tokens joined as written.
			std::string Operand()
			{
				std::string text;
				if (!Accept("("))
				{
					Fail("expected '(' after feature test");
					return text;
				}

				std::size_t level = 1;
				while (mPos < mTokens.size())
				{
					const ExpandedToken& token = mTokens[mPos++];
					if (token.kind == TokenKind::Punctuation && token.text == "(")
					{
						++level;
					}
					else if (token.kind == TokenKind::Punctuation && token.text == ")" &&
						--level == 0)
					{
						return text;
					}

					if (!text.empty() && token.spaceBefore)
					{
						text += ' ';
					}

					text += token.text;
				}

				Fail("unterminated feature test");
				return text;
			}

			Value Primary()
			{
				if (mPos >= mTokens.size())
				{
					Fail("expected an expression");
					return 0;
				}

				const ExpandedToken token = mTokens[mPos];
				if (Accept("("))
				{
					const Value inner = Ternary();
					if (!Accept(")"))
					{
						Fail("expected ')'");
					}

					return inner;
				}

				++mPos;
				if (token.kind == TokenKind::Number)
				{
					if (const auto value = ParseNumber(token.text))
					{
						return *value;
					}

					Fail("invalid integer literal '" + std::string(token.text) + "'");
					return 0;
				}

				if (token.kind == TokenKind::CharacterLiteral)
				{
					return CharValue(token.text);
				}

				if (token.kind != TokenKind::Identifier)
				{
					Fail("unexpected token '" + std::string(token.text) + "' in expression");
					return 0;
				}

				return Identifier(token.text);
			}

			Value Identifier(std::string_view name)
			{
				if (name == "defined")
				{
					const bool paren = Accept("(");
					if (mPos >= mTokens.size() || mTokens[mPos].kind != TokenKind::Identifier)
					{
						Fail("operator 'defined' requires an identifier");
						return 0;
					}

					const bool defined = mTable.Find(mTokens[mPos++].text) != nullptr;
					if (paren && !Accept(")"))
					{
						Fail("missing ')' after 'defined'");
					}

					return defined ? 1 : 0;
				}

				if (name == "__has_include" || name == "__has_include_next")
				{
					const std::string operand = Operand();
					IncludeRequest request;
					request.next = name == "__has_include_next";
					if (operand.size() >= kDelimiterPair && (operand.front() == '"' || operand.front() == '<'))
					{
						request.angled = operand.front() == '<';
						request.header = operand.substr(1, operand.size() - kDelimiterPair);
					}
					else
					{
						request.header = operand;
					}

					return mCallbacks.hasInclude && mCallbacks.hasInclude(request) ? 1 : 0;
				}

				if (name.starts_with("__has_"))
				{
					const std::string operand = Operand();
					if (mCallbacks.feature)
					{
						if (const auto value = mCallbacks.feature(name, operand))
						{
							return *value;
						}
					}

					return BuiltinFeature(name, operand).value_or(0);
				}

				if (name == "true")
				{
					return 1;
				}

				return 0;
			}

			const std::vector<ExpandedToken>& mTokens;
			const MacroTable& mTable;
			const MacroScanCallbacks& mCallbacks;
			std::size_t mPos   = 0;
			std::size_t mDepth = 0;
			bool mError        = false;
			std::string mMessage;
		};

		// The operand of an #include after macro expansion.
		std::optional<IncludeRequest> ParseInclude(
			std::string_view rest,
			const MacroTable& table,
			bool next,
			std::size_t offset)
		{
			rest = Trim(rest);
			IncludeRequest request;
			request.next   = next;
			request.offset = offset;
			std::string expandedText;
			if (rest.empty() ||(rest.front() != '"' && rest.front() != '<'))
			{
				const ExpansionResult expanded = MacroExpander(table).Expand(rest);
				for (const ExpandedToken& token : expanded.tokens)
				{
					if (!expandedText.empty() && token.spaceBefore)
					{
						expandedText += ' ';
					}

					expandedText += token.text;
				}

				rest = expandedText;
			}

			if (rest.size() < kDelimiterPair)
			{
				return std::nullopt;
			}

			const char close = rest.front() == '<' ? '>' : '"';
			if (rest.front() != '<' && rest.front() != '"')
			{
				return std::nullopt;
			}

			const std::size_t end = rest.find(close, 1);
			if (end == std::string_view::npos)
			{
				return std::nullopt;
			}

			request.angled = rest.front() == '<';
			request.header = std::string(rest.substr(1, end - 1));
			return request;
		}

	} // namespace

	bool MacroScanner::Evaluate(
		std::string_view expression,
		const MacroTable& table,
		const MacroScanCallbacks& callbacks,
		std::vector<PreprocessorDiagnostic>* diagnostics)
	{
		const ExpansionResult expanded = MacroExpander(table).Expand(expression, true);
		Condition condition(expanded.tokens, table, callbacks);
		const bool result = condition.Evaluate();
		if (diagnostics != nullptr && !condition.Error().empty())
		{
			diagnostics->push_back({0, condition.Error()});
		}

		return result;
	}

	MacroScanResult MacroScanner::Scan(
		std::string_view text,
		std::string_view file,
		MacroTable& table,
		const MacroScanCallbacks& callbacks,
		std::stop_token stop)
	{
		MacroScanResult result;
		std::vector<Frame> stack;
		bool active        = true;
		bool inBlock       = false;
		std::size_t offset = 0;
		// Include guard tracking: the first code line must be `#ifndef X` and nothing but
		// blanks and comments may follow its `#endif`.
		bool sawCode        = false;
		bool guardCandidate = false;
		bool guardClosed    = false;
		bool guardBroken    = false;
		std::string guardName;

		while (offset < text.size())
		{
			if (stop.stop_requested())
			{
				result.cancelled = true;
				return result;
			}

			const std::size_t lineStart = offset;
			std::size_t end             = text.find('\n', offset);
			end                         = end == std::string_view::npos ? text.size() : end + 1;
			std::string_view line       = text.substr(lineStart, end - lineStart);
			std::string_view body       = line;
			while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
			{
				body.remove_suffix(1);
			}

			std::string joined;
			auto trimmed = Trim(body);
			if (!inBlock && !trimmed.empty() && trimmed.front() == '#' && EndsWithBackslash(body))
			{
				joined.assign(body.substr(0, body.size() - 1));
				while (end < text.size())
				{
					std::size_t nextEnd    = text.find('\n', end);
					nextEnd                = nextEnd == std::string_view::npos ? text.size() : nextEnd + 1;
					std::string_view piece = text.substr(end, nextEnd - end);
					end                    = nextEnd;
					while (!piece.empty() && (piece.back() == '\n' || piece.back() == '\r'))
					{
						piece.remove_suffix(1);
					}

					const bool more = EndsWithBackslash(piece);
					joined += ' ';
					joined.append(piece.substr(0, more ? piece.size() - 1 : piece.size()));
					if (!more)
					{
						break;
					}
				}

				line    = text.substr(lineStart, end - lineStart);
				trimmed = Trim(joined);
			}

			offset                   = end;
			const bool lineInComment = inBlock;
			const bool hasCode       = ScanLine(line, inBlock);
			const bool directive     = !lineInComment && !trimmed.empty() && trimmed.front() == '#';
			const auto record =[&](bool counts)
			{
				if (!counts)
				{
					return;
				}

				if (!result.activeRanges.empty() &&
					result.activeRanges.back().offset + result.activeRanges.back().length ==
					lineStart)
				{
					result.activeRanges.back().length += line.size();
				}
				else
				{
					result.activeRanges.push_back({lineStart, line.size()});
				}
			};

			if (hasCode)
			{
				if (guardClosed)
				{
					guardBroken = true;
				}

				if (!directive && !sawCode)
				{
					guardBroken = true;
				}
			}

			if (!directive)
			{
				record(active);
				if (hasCode)
				{
					sawCode = true;
				}

				continue;
			}

			trimmed.remove_prefix(1);
			trimmed                     = Trim(trimmed);
			const std::string_view name = ReadWord(trimmed);
			const std::string_view rest = Trim(trimmed);
			if (callbacks.recordDirectives)
			{
				result.directives.push_back(
					{KindOfDirective(name), lineStart, line.size(), active});
			}

			const bool opensGroup = name == "if" || name == "ifdef" || name == "ifndef";
			// Conditional directives are always part of the compiled text; the others only
			// count inside an active group.
			record(active || opensGroup || name == "elif" || name == "elifdef" ||
				name == "elifndef" || name == "else" || name == "endif");
			if (!sawCode)
			{
				sawCode        = true;
				guardCandidate = name == "ifndef";
				if (guardCandidate)
				{
					std::string_view operand = rest;
					guardName                = std::string(ReadWord(operand));
				}
				else
				{
					guardBroken = true;
				}
			}

			if (opensGroup)
			{
				bool condition = false;
				if (active)
				{
					if (name == "if")
					{
						condition = Evaluate(rest, table, callbacks, &result.diagnostics);
					}
					else
					{
						std::string_view operand = rest;
						condition                = table.Find(ReadWord(operand)) != nullptr;
						condition                = name == "ifndef" ? !condition : condition;
					}
				}

				stack.push_back({active, active&& condition, active&& condition, false});
				active = stack.back().active;
			}
			else if (name == "elif" || name == "elifdef" || name == "elifndef")
			{
				if (stack.empty())
				{
					result.diagnostics.push_back({lineStart, "#elif without matching #if"});
					continue;
				}

				Frame& frame   = stack.back();
				bool condition = false;
				if (frame.parentActive && !frame.branchTaken)
				{
					if (name == "elif")
					{
						condition = Evaluate(rest, table, callbacks, &result.diagnostics);
					}
					else
					{
						std::string_view operand = rest;
						condition                = table.Find(ReadWord(operand)) != nullptr;
						condition                = name == "elifndef" ? !condition : condition;
					}
				}

				frame.active = condition;
				frame.branchTaken |= condition;
				active = frame.active;
			}
			else if (name == "else")
			{
				if (stack.empty())
				{
					result.diagnostics.push_back({lineStart, "#else without matching #if"});
					continue;
				}

				Frame& frame      = stack.back();
				frame.sawElse     = true;
				frame.active      = frame.parentActive && !frame.branchTaken;
				frame.branchTaken = true;
				active            = frame.active;
			}
			else if (name == "endif")
			{
				if (stack.empty())
				{
					result.diagnostics.push_back({lineStart, "#endif without matching #if"});
					continue;
				}

				active = stack.back().parentActive;
				stack.pop_back();
				guardClosed = stack.empty() && guardCandidate;
			}
			else if (!active)
			{
				continue;
			}
			else if (name == "define")
			{
				auto parsed = MacroTable::ParseDefinition(rest);
				if (!parsed)
				{
					result.diagnostics.push_back({lineStart, std::move(parsed.error())});
					continue;
				}

				if (callbacks.recordDirectives)
				{
					result.events.push_back({true, lineStart, std::string(rest)});
				}

				parsed->origin = MacroOrigin::File;
				parsed->file   = std::string(file);
				parsed->offset = static_cast<std::uint32_t>(lineStart);
				table.Define(std::move(*parsed));
				if (callbacks.checkpoint)
				{
					callbacks.checkpoint(end);
				}
			}
			else if (name == "undef")
			{
				std::string_view operand         = rest;
				const std::string_view undefined = ReadWord(operand);
				if (callbacks.recordDirectives)
				{
					result.events.push_back({false, lineStart, std::string(undefined)});
				}

				table.Undef(undefined);
				if (callbacks.checkpoint)
				{
					callbacks.checkpoint(end);
				}
			}
			else if (name == "include" || name == "include_next" || name == "import")
			{
				const auto request = ParseInclude(rest, table, name == "include_next", lineStart);
				if (!request)
				{
					result.diagnostics.push_back({lineStart, "malformed #include"});
				}
				else if (!callbacks.include ||!callbacks.include(*request))
				{
					result.diagnostics.push_back(
						{lineStart, "'" + request->header + "' file not found"});
				}

				if (callbacks.checkpoint)
				{
					callbacks.checkpoint(end);
				}
			}
			else if (name == "pragma")
			{
				std::string_view operand = rest;
				if (ReadWord(operand) == "once")
				{
					result.pragmaOnce = true;
				}
			}
		}

		if (!stack.empty())
		{
			result.diagnostics.push_back({text.size(), "unterminated conditional directive"});
		}

		if (guardCandidate && guardClosed && !guardBroken)
		{
			result.guard = guardName;
		}

		return result;
	}

} // namespace heimdall
