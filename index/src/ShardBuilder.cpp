#include <Heimdall/ShardBuilder.hpp>
#include <Heimdall/TypeModel.hpp>

#include <algorithm>
#include <ranges>
#include <string>
#include <unordered_map>

namespace heimdall::index
{
	namespace
	{
		constexpr std::size_t kMemberChildCount = 2;
		bool IsIndexed(const SemanticModel& model, SymbolId symbol)
		{
			const auto& symbols = model.Symbols();
			switch (symbols.kind[symbol])
			{
			case heimdall::SymbolKind::Parameter:
			case heimdall::SymbolKind::TemplateParameter:
				return false;
			default:
				break;
			}

			const auto scope = symbols.scope[symbol];
			if (scope >= model.Scopes().Size())
			{
				return false;
			}

			const auto kind = model.Scopes().kind[scope];
			return kind == heimdall::ScopeKind::TranslationUnit ||
				kind == heimdall::ScopeKind::Namespace || kind == heimdall::ScopeKind::Class;
		}

	} // namespace

	class ShardBuilderImpl
	{
	public:
		ShardBuilderImpl(const SemanticModel& model, std::string path, std::uint64_t configHash,
			ShardBuilder::IncludeResolver resolveInclude) :
		mModel(model), mShard(std::make_unique<FileShard>()), mResolveInclude(std::move(resolveInclude))
		{
			mShard->mPath        = std::move(path);
			mShard->mConfigHash  = configHash;
			mShard->mContentHash = HashBytes(model.Tree().Source());
			mShard->mSyntaxComplete = !model.Tree().Cancelled() && model.Tree().Diagnostics().empty();
		}

		std::shared_ptr<const FileShard> Run()
		{
			CollectSymbols();
			CollectForwardDeclarations();
			CollectScopes();
			CollectUsings();
			CollectReceivers();
			CollectCallTypes();
			CollectOccurrences();
			CollectRelations();
			CollectIncludes();
			mShard->Finalize();
			return std::shared_ptr<const FileShard>(std::move(mShard));
		}

	private:
		std::string_view TokenText(std::uint32_t token) const
		{
			return mModel.Tree().Text(mModel.Tree().Tokens()[token]);
		}

		// Names of the namespaces/classes that enclose `scope`, outermost first.
		void AppendScopePath(ScopeId scope, std::string& out, bool& internal) const
		{
			const auto& scopes = mModel.Scopes();
			std::vector<SymbolId> owners;
			for (std::size_t steps = 0; scope != kNone && scope < scopes.Size() &&
				scope != SemanticModel::TranslationUnitScope && steps < scopes.Size();
				++steps)
			{
				if (scopes.owner[scope] != kNone)
				{
					owners.push_back(scopes.owner[scope]);
				}

				scope = scopes.parent[scope];
			}

			for (const auto owner : std::views::reverse(owners))
			{
				const auto name = mModel.Names().Text(mModel.Symbols().name[owner]);
				internal |= name.empty();
				out += name.empty() ? std::string_view("(anonymous)") : name;
				out += "::";
			}
		}

		// `A::B::name` written at a qualified declaration, outermost first.
		void AppendWrittenQualifier(std::uint32_t token, std::string& out,
			std::vector<std::uint32_t>* names = nullptr) const
		{
			const auto& sig    = mModel.Significant();
			const auto& tokens = mModel.Tree().Tokens();
			auto at            = std::ranges::lower_bound(sig, token);
			if (at == sig.end() || *at != token)
			{
				return;
			}

			if (at != sig.begin() && tokens[at[-1]].tok == Tok::Tilde)
			{
				--at;
			}

			std::vector<std::string_view> parts;
			while (at - sig.begin() >= 2 && tokens[at[-1]].tok == Tok::ColonColon &&
				tokens[at[-2]].kind == TokenKind::Identifier)
			{
				parts.push_back(TokenText(at[-2]));
				if (names != nullptr)
				{
					names->push_back(at[-2]);
				}

				at -= 2;
			}

			for (const auto part : std::views::reverse(parts))
			{
				out += part;
				out += "::";
			}
		}

		void CollectSymbols()
		{
			const auto& symbols = mModel.Symbols();
			auto& out           = mShard->mSymbols;
			mLocal.assign(symbols.Size(), kInvalid);
			std::vector<SymbolId> kept;
			for (SymbolId i = 0; i < symbols.Size(); ++i)
			{
				if (IsIndexed(mModel, i) && symbols.decl_token[i] < mModel.Tree().Tokens().size())
				{
					mLocal[i] = static_cast<std::uint32_t>(kept.size());
					kept.push_back(i);
				}
			}

			out.hash.reserve(kept.size());
			for (const auto symbol : kept)
			{
				const auto name  = TokenText(symbols.decl_token[symbol]);
				const auto flags = symbols.flags[symbol];
				bool internal    = false;
				std::string qualified;
				AppendScopePath(symbols.scope[symbol], qualified, internal);
				if ((flags& SymbolFlag::Qualified) != 0)
				{
					AppendWrittenQualifier(symbols.decl_token[symbol], qualified, &mExtraUses);
				}

				qualified += name;

				const auto kind  = symbols.kind[symbol];
				const auto scope = mModel.Scopes().kind[symbols.scope[symbol]];
				internal |= scope != heimdall::ScopeKind::Class &&
					(flags& SymbolFlag::Static) != 0 && kind != heimdall::SymbolKind::Namespace;

				std::string key(1, static_cast<char>(kind));
				key += qualified;
				key += '\0';
				if (kind == heimdall::SymbolKind::Function)
				{
					key += mModel.Names().Text(symbols.signatureText[symbol]);
				}

				if (internal)
				{
					key += '\0';
					key += mShard->mPath;
				}
				const auto hash = HashBytes(key);

				const auto container = symbols.scope[symbol] < mModel.Scopes().Size()
				? mModel.Scopes().owner[symbols.scope[symbol]]
				: kNone;
				const auto tokenInfo = mModel.Tree().Tokens()[symbols.decl_token[symbol]];

				out.hash.push_back(hash);
				out.nameMask.push_back(CharacterMask(name));
				out.name.push_back(mShard->mStrings.Intern(name));
				out.qualified.push_back(mShard->mStrings.Intern(qualified));
				out.identity.push_back(mShard->mStrings.Intern(key));
				out.signature.push_back(mShard->mStrings.Intern(
					mModel.Names().Text(symbols.signatureText[symbol])));
				out.container.push_back(container == kNone ? kInvalid : mLocal[container]);
				out.flags.push_back(flags);
				out.nameOffset.push_back(tokenInfo.offset);
				out.nameLength.push_back(tokenInfo.length);
				out.kind.push_back(kind);
				out.role.push_back(RoleOf(symbol));
				const auto arity = ArityOf(symbol);
				out.paramMin.push_back(arity.first);
				out.paramMax.push_back(arity.second);
				CollectParameterTypes(symbol);
			}

			mKept = std::move(kept);
		}

		// A class or enum has a body when `{` comes before `;` after its name.
		bool HasBody(SymbolId symbol) const
		{
			const auto& sig    = mModel.Significant();
			const auto& tokens = mModel.Tree().Tokens();
			auto at            = std::ranges::lower_bound(sig, mModel.Symbols().decl_token[symbol]);
			for (; at != sig.end(); ++at)
			{
				if (tokens[*at].tok == Tok::LBrace)
				{
					return true;
				}

				if (tokens[*at].tok == Tok::Semi)
				{
					return false;
				}
			}

			return false;
		}

		Role RoleOf(SymbolId symbol) const
		{
			const auto& symbols = mModel.Symbols();
			const auto kind     = symbols.kind[symbol];
			const bool defined = (symbols.flags[symbol] & SymbolFlag::Definition) != 0 ||
				((kind == heimdall::SymbolKind::Class || kind == heimdall::SymbolKind::Enum) &&
				HasBody(symbol));
			return defined ? Role::Definition : Role::Declaration;
		}

		struct Pending
		{
			std::uint32_t offset;
			std::uint32_t length;
			std::uint32_t symbol;
			StringId name;
			Role role;
			Resolution resolution;
		}

		;

		struct Forward
		{
			std::uint32_t token;
			std::uint32_t symbol;
		};

		struct OpenScope
		{
			std::vector<std::string_view> names;
			bool block = false;
		}

		;

		// `struct X;`, `class X;` and `enum [class] X [: T];` create no binder symbol.
		void CollectForwardDeclarations()
		{
			const auto& sig     = mModel.Significant();
			const auto& tokens  = mModel.Tree().Tokens();
			const auto& symbols = mModel.Symbols();
			std::vector<std::uint32_t> modelTokens;
			for (const auto symbol : mKept)
			{
				modelTokens.push_back(symbols.decl_token[symbol]);
			}

			std::ranges::sort(modelTokens);
			std::vector<OpenScope> stack;
			OpenScope pending;
			bool hasPending = false;
			const auto isName =[&](std::size_t at)
			{
				return at < sig.size() && tokens[sig[at]].kind == TokenKind::Identifier &&
					tokens[sig[at]].tok == Tok::None;
			};
			for (std::size_t i = 0; i < sig.size(); ++i)
			{
				const auto& token = tokens[sig[i]];
				switch (token.tok)
				{
				case Tok::LBrace:
					if (hasPending)
					{
						stack.push_back(std::move(pending));
					}
					else
					{
						stack.push_back({{}, !(i >= 2 && tokens[sig[i - 1]].kind ==
								TokenKind::StringLiteral && tokens[sig[i - 2]].tok == Tok::KwExtern)});
					}

					pending    = {};
					hasPending = false;
					continue;
				case Tok::RBrace:
					if (!stack.empty())
					{
						stack.pop_back();
					}

					hasPending = false;
					continue;
				case Tok::Semi:
				case Tok::Eq:
				case Tok::LParen:
					hasPending = false;
					pending    = {};
					continue;
				case Tok::KwNamespace:
				{
					pending        = {};
					std::size_t at = i + 1;
					while (isName(at))
					{
						pending.names.push_back(TokenText(sig[at]));
						++at;
						if (at < sig.size() && tokens[sig[at]].tok == Tok::ColonColon)
						{
							++at;
						}
					}

					if (pending.names.empty())
					{
						pending.names.push_back("(anonymous)");
					}

					hasPending = at < sig.size() && tokens[sig[at]].tok == Tok::LBrace;
					continue;
				}
				case Tok::KwClass:
				case Tok::KwStruct:
				case Tok::KwUnion:
				case Tok::KwEnum:
					break;
				default:
					continue;
				}

				const bool isEnum   = token.tok == Tok::KwEnum;
				const bool friended = i > 0 && tokens[sig[i - 1]].tok == Tok::KwFriend;
				std::size_t at      = i + 1;
				if (isEnum && at < sig.size() &&
					(tokens[sig[at]].tok == Tok::KwClass || tokens[sig[at]].tok == Tok::KwStruct))
				{
					++at;
				}

				if (!isName(at))
				{
					continue;
				}

				const auto nameToken = sig[at];
				std::size_t after    = at + 1;
				if (isEnum && after < sig.size() && tokens[sig[after]].tok == Tok::Colon)
				{
					while (after < sig.size() && tokens[sig[after]].tok != Tok::Semi &&
						tokens[sig[after]].tok != Tok::LBrace)
					{
						++after;
					}
				}

				const bool forward = after < sig.size() && tokens[sig[after]].tok == Tok::Semi;
				if (!isEnum)
				{
					pending = {};
					pending.names.push_back(TokenText(nameToken));
					hasPending = !forward;
				}
				else
				{
					hasPending = false;
				}

				if (forward && friended)
				{
					mExtraUses.push_back(nameToken);
				}

				if (!forward || friended ||
					std::ranges::binary_search(modelTokens, nameToken) ||
					std::ranges::any_of(stack, &OpenScope::block))
				{
					continue;
				}

				AddForward(stack, nameToken, isEnum);
			}
		}

		void AddForward(const std::vector<OpenScope>& stack, std::uint32_t nameToken, bool isEnum)
		{
			const auto name = TokenText(nameToken);
			bool internal   = false;
			std::string qualified;
			for (const auto& scope : stack)
			{
				for (const auto part : scope.names)
				{
					internal |= part == "(anonymous)";
					qualified += part;
					qualified += "::";
				}
			}

			qualified += name;
			std::string key(1, static_cast<char>(isEnum ? SymbolKind::Enum : SymbolKind::Class));
			key += qualified;
			key += '\0';
			if (internal)
			{
				key += '\0';
				key += mShard->mPath;
			}
			const auto hash = HashBytes(key);

			auto& out           = mShard->mSymbols;
			const auto existing = std::ranges::find(out.hash, hash);
			std::uint32_t local;
			if (existing != out.hash.end())
			{
				local = static_cast<std::uint32_t>(existing - out.hash.begin());
			}
			else
			{
				local            = static_cast<std::uint32_t>(out.hash.size());
				const auto& info = mModel.Tree().Tokens()[nameToken];
				out.hash.push_back(hash);
				out.nameMask.push_back(CharacterMask(name));
				out.name.push_back(mShard->mStrings.Intern(name));
				out.qualified.push_back(mShard->mStrings.Intern(qualified));
				out.identity.push_back(mShard->mStrings.Intern(key));
				out.signature.push_back(mShard->mStrings.Intern({}));
				out.container.push_back(kInvalid);
				out.flags.push_back(0);
				out.nameOffset.push_back(info.offset);
				out.nameLength.push_back(info.length);
				out.kind.push_back(isEnum ? heimdall::SymbolKind::Enum : heimdall::SymbolKind::Class);
				out.role.push_back(Role::Declaration);
				out.paramMin.push_back(kNoArity);
				out.paramMax.push_back(kNoArity);
				out.paramTypeBegin.push_back(0);
				out.paramTypeCount.push_back(0);
			}

			mForward.push_back({nameToken, local});
		}

		// A base specifier names a type; the last written identifier is the class.
		void CollectBaseUses(std::vector<Pending>& pending)
		{
			const auto& symbols = mModel.Symbols();
			const auto& bases   = mModel.Bases();
			const auto& sig     = mModel.Significant();
			const auto& tokens  = mModel.Tree().Tokens();
			for (std::size_t slot = 0; slot < bases.token.size(); ++slot)
			{
				auto at = std::ranges::lower_bound(sig, bases.token[slot]);
				if (at == sig.end() || *at != bases.token[slot])
				{
					continue;
				}

				auto last = *at;
				while (at + 2 < sig.end() && tokens[at[1]].tok == Tok::ColonColon &&
					tokens[at[2]].kind == TokenKind::Identifier)
				{
					at += 2;
					last = *at;
				}

				const auto& info = tokens[last];
				if (info.kind != TokenKind::Identifier || info.tok != Tok::None)
				{
					continue;
				}

				const auto target = bases.target[slot];
				const bool local  = target != kNone && mLocal[target] != kInvalid;
				pending.push_back({static_cast<std::uint32_t>(info.offset),
						static_cast<std::uint32_t>(info.length), local ? mLocal[target] : kInvalid,
						mShard->mStrings.Intern(TokenText(last)), Role::Reference,
						local ? Resolution::Resolved : Resolution::Unresolved});
			}

			(void) symbols;
		}

		void CollectTypeUses(std::vector<Pending>& pending, const std::vector<std::uint32_t>&)
		{
			const auto& tree   = mModel.Tree();
			const auto& soa    = tree.NodesSoA();
			const auto& tokens = tree.Tokens();
			std::vector<std::uint32_t> taken;
			for (const auto& item : pending)
			{
				taken.push_back(item.offset);
			}

			std::ranges::sort(taken);
			for (const auto token : mExtraUses)
			{
				const auto& current = tokens[token];
				if (current.kind != TokenKind::Identifier || current.tok != Tok::None ||
					std::ranges::binary_search(taken, static_cast<std::uint32_t>(current.offset)))
				{
					continue;
				}

				pending.push_back({static_cast<std::uint32_t>(current.offset),
						static_cast<std::uint32_t>(current.length), kInvalid,
						mShard->mStrings.Intern(TokenText(token)), Role::Reference,
						Resolution::Unresolved});
				taken.insert(
					std::ranges::lower_bound(taken, static_cast<std::uint32_t>(current.offset)),
					static_cast<std::uint32_t>(current.offset));
			}

			for (std::uint32_t node = 0; node < soa.size(); ++node)
			{
				if (soa.Kind(node) != GrammarKind::TypeSpecifier)
				{
					continue;
				}

				const auto first = soa.FirstToken(node);
				const auto last  = std::min<std::size_t>(first + soa.TokenCount(node), tokens.size());
				for (auto token = first; token < last; ++token)
				{
					const auto& current = tokens[token];
					if (current.kind != TokenKind::Identifier || current.tok != Tok::None ||
						std::ranges::binary_search(taken, static_cast<std::uint32_t>(current.offset)))
					{
						continue;
					}

					const auto text = TokenText(static_cast<std::uint32_t>(token));
					const auto name = mModel.Names().Find(text);
					auto before     = token;
					while (before > first && tokens[before - 1].kind == TokenKind::Whitespace)
					{
						--before;
					}

					const bool member = before > first &&
						(tokens[before - 1].tok == Tok::ColonColon ||
						tokens[before - 1].tok == Tok::Dot || tokens[before - 1].tok == Tok::Arrow);
					SymbolId target = kNone;
					if (name != kNone && !member)
					{
						target = mModel.Lookup(
							mModel.ScopeOfNode(node), name, static_cast<std::uint32_t>(token));
					}

					const bool local = target != kNone && mLocal[target] != kInvalid;
					pending.push_back({static_cast<std::uint32_t>(current.offset),
							static_cast<std::uint32_t>(current.length), local ? mLocal[target] : kInvalid,
							mShard->mStrings.Intern(text), Role::Reference,
							local ? Resolution::Resolved : Resolution::Unresolved});
					taken.insert(std::ranges::lower_bound(taken, static_cast<std::uint32_t>(current.offset)),
						static_cast<std::uint32_t>(current.offset));
				}
			}

			const auto& sig = mModel.Significant();
			for (std::size_t i = 0; i + 1 < sig.size(); ++i)
			{
				const auto cast = tokens[sig[i]].tok;
				if ((cast != Tok::KwStaticCast && cast != Tok::KwDynamicCast &&
					cast != Tok::KwConstCast && cast != Tok::KwReinterpretCast) ||
					tokens[sig[i + 1]].tok != Tok::Lt)
				{
					continue;
				}

				int depth = 0;
				for (auto at = i + 1; at < sig.size(); ++at)
				{
					const auto& current = tokens[sig[at]];
					depth += current.tok == Tok::Lt;
					depth -= current.tok == Tok::Gt;
					if (depth == 0 || current.tok == Tok::Semi)
					{
						break;
					}

					if (current.kind != TokenKind::Identifier || current.tok != Tok::None ||
						std::ranges::binary_search(taken, static_cast<std::uint32_t>(current.offset)))
					{
						continue;
					}

					const auto before = tokens[sig[at - 1]].tok;
					if (before == Tok::Dot || before == Tok::Arrow)
					{
						continue;
					}

					pending.push_back({static_cast<std::uint32_t>(current.offset),
							static_cast<std::uint32_t>(current.length), kInvalid,
							mShard->mStrings.Intern(TokenText(sig[at])), Role::Reference,
							Resolution::Unresolved});
					taken.insert(
						std::ranges::lower_bound(taken, static_cast<std::uint32_t>(current.offset)),
						static_cast<std::uint32_t>(current.offset));
				}
			}
		}

		struct Counted
		{
			std::size_t count    = 0;
			std::size_t required = 0;
			bool variadic        = false;
			bool valid           = false;
		}

		;

		// Top-level comma-separated items between the `(` at significant index `open`.
		Counted CountList(std::size_t open) const
		{
			const auto& sig    = mModel.Significant();
			const auto& tokens = mModel.Tree().Tokens();
			Counted result;
			int depth              = 0;
			bool any               = false;
			bool defaulted         = false;
			std::size_t itemTokens = 0;
			std::size_t firstItem  = 0;
			for (auto at = open + 1; at < sig.size(); ++at)
			{
				const auto tok = tokens[sig[at]].tok;
				if (depth == 0 && tok == Tok::RParen)
				{
					if (any && !(itemTokens == 1 && tokens[sig[firstItem]].tok == Tok::Ellipsis))
					{
						++result.count;
						result.required +=!defaulted;
						if (itemTokens == 1 && tokens[sig[firstItem]].tok == Tok::KwVoid &&
							result.count == 1)
						{
							result.count    = 0;
							result.required = 0;
						}
					}

					result.valid = true;
					return result;
				}

				if (tok == Tok::LParen || tok == Tok::LBracket || tok == Tok::LBrace)
				{
					++depth;
				}
				else if (tok == Tok::RParen || tok == Tok::RBracket || tok == Tok::RBrace)
				{
					--depth;
				}
				else if (tok == Tok::Semi)
				{
					return result;
				}

				if (depth == 0 && tok == Tok::Comma)
				{
					if (!(itemTokens == 1 && tokens[sig[firstItem]].tok == Tok::Ellipsis))
					{
						++result.count;
						result.required +=!defaulted;
					}

					defaulted  = false;
					itemTokens = 0;
					continue;
				}

				if (depth == 0 && tok == Tok::Eq)
				{
					defaulted = true;
				}

				if (depth == 0 && tok == Tok::Ellipsis)
				{
					result.variadic = true;
				}

				if (itemTokens == 0)
				{
					firstItem = at;
				}

				++itemTokens;
				any = true;
			}

			return result;
		}

		std::pair<std::uint8_t, std::uint8_t> ArityOf(SymbolId symbol) const
		{
			const auto& symbols = mModel.Symbols();
			if (symbols.kind[symbol] != heimdall::SymbolKind::Function)
			{
				return {kNoArity, kNoArity};
			}

			const auto& sig  = mModel.Significant();
			const auto& toks = mModel.Tree().Tokens();
			const auto at = static_cast<std::size_t>(
				std::ranges::lower_bound(sig, symbols.decl_token[symbol]) - sig.begin());
			if (at + 1 >= sig.size() || toks[sig[at + 1]].tok != Tok::LParen)
			{
				return {kNoArity, kNoArity};
			}

			const auto counted = CountList(at + 1);
			if (!counted.valid || counted.count >= kNoArity)
			{
				return {kNoArity, kNoArity};
			}

			return {static_cast<std::uint8_t>(counted.required),
				counted.variadic ? kNoArity : static_cast<std::uint8_t>(counted.count)};
		}

		// Extents of function/block scopes, used to ignore block-local `using`.
		bool InsideBody(std::uint32_t offset) const
		{
			const auto& scopes = mModel.Scopes();
			for (ScopeId scope = 0; scope < scopes.Size(); ++scope)
			{
				if ((scopes.kind[scope] == heimdall::ScopeKind::Function ||
					scopes.kind[scope] == heimdall::ScopeKind::Block))
				{
					const auto range = ScopeRange(scope);
					if (offset >= range.first && offset < range.second)
					{
						return true;
					}
				}
			}

			return false;
		}

		std::pair<std::uint32_t, std::uint32_t> ScopeRange(ScopeId scope) const
		{
			const auto& soa    = mModel.Tree().NodesSoA();
			const auto& tokens = mModel.Tree().Tokens();
			const auto node    = mModel.Scopes().node[scope];
			if (node >= soa.size() || soa.TokenCount(node) == 0 ||
				soa.FirstToken(node) >= tokens.size())
			{
				return {0, 0};
			}

			const auto first = soa.FirstToken(node);
			const auto last  = std::min<std::size_t>(first + soa.TokenCount(node), tokens.size()) - 1;
			return {static_cast<std::uint32_t>(tokens[first].offset),
				static_cast<std::uint32_t>(tokens[last].offset + tokens[last].length)};
		}

		// Namespace/class extents plus the bodies of out-of-line members (`void A::f() {}`).
		void CollectScopes()
		{
			const auto& scopes  = mModel.Scopes();
			const auto& symbols = mModel.Symbols();
			auto& out           = mShard->mScopes;
			std::unordered_map<std::uint32_t, SymbolId> qualifiedBody;
			for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				if (symbols.kind[symbol] == heimdall::SymbolKind::Function &&
					(symbols.flags[symbol] & SymbolFlag::Qualified) != 0 &&
					symbols.decl_token[symbol] < mModel.Tree().Tokens().size())
				{
					qualifiedBody.emplace(symbols.decl_node[symbol], symbol);
				}
			}

			struct Entry
			{
				std::uint32_t begin;
				std::uint32_t end;
				ScopeId scope;
				std::string qualified;
			};

			std::vector<Entry> entries;
			std::vector<std::string> byScope(scopes.Size());
			const auto qualifiedOf =[&](auto&& self, ScopeId scope)->std::string
			{
				if (scope == kNone || scope >= scopes.Size() ||
					scope == SemanticModel::TranslationUnitScope)
				{
					return {};
				}

				if (!byScope[scope].empty())
				{
					return byScope[scope];
				}

				std::string result = self(self, scopes.parent[scope]);
				if (scopes.owner[scope] != kNone)
				{
					const auto name = mModel.Names().Text(symbols.name[scopes.owner[scope]]);
					result += result.empty() ? "" : "::";
					result += name.empty() ? std::string_view("(anonymous)") : name;
				}

				byScope[scope] = result;
				return result;
			};
			for (ScopeId scope = 0; scope < scopes.Size(); ++scope)
			{
				if (scope == SemanticModel::TranslationUnitScope)
				{
					continue;
				}

				const auto range = ScopeRange(scope);
				if (range.second <= range.first)
				{
					continue;
				}

				if (scopes.kind[scope] == heimdall::ScopeKind::Namespace ||
					scopes.kind[scope] == heimdall::ScopeKind::Class)
				{
					if (scopes.owner[scope] != kNone)
					{
						entries.push_back({range.first, range.second, scope, qualifiedOf(qualifiedOf, scope)});
					}
				}
				else if (scopes.kind[scope] == heimdall::ScopeKind::Function)
				{
					const auto found = qualifiedBody.find(scopes.node[scope]);
					if (found == qualifiedBody.end())
					{
						continue;
					}

					std::string path = qualifiedOf(qualifiedOf, symbols.scope[found->second]);
					std::string written;
					AppendWrittenQualifier(symbols.decl_token[found->second], written);
					while (written.ends_with("::"))
					{
						written.resize(written.size() - 2);
					}

					const auto& sig  = mModel.Significant();
					const auto after = std::ranges::upper_bound(sig, symbols.decl_token[found->second]);
					const auto begin = after == sig.end()
					? range.first
					: std::max(range.first,
						static_cast<std::uint32_t>(mModel.Tree().Tokens()[*after].offset));
					if (!written.empty())
					{
						entries.push_back({begin, range.second, scope,
								path.empty() ? written : path + "::" + written});
					}
				}
			}

			std::ranges::stable_sort(entries,
				[](const Entry& a, const Entry& b)
				{
					return a.begin != b.begin ? a.begin < b.begin : a.end > b.end;
			});
			std::vector<std::uint32_t> open;
			for (std::uint32_t i = 0; i < entries.size(); ++i)
			{
				while (!open.empty() && entries[open.back()].end <= entries[i].begin)
				{
					open.pop_back();
				}

				out.begin.push_back(entries[i].begin);
				out.end.push_back(entries[i].end);
				out.parent.push_back(open.empty() ? kInvalid : open.back());
				out.qualified.push_back(mShard->mStrings.Intern(entries[i].qualified));
				open.push_back(i);
			}
		}

		std::uint32_t ScopeAtOffset(std::uint32_t offset) const
		{
			const auto& scopes = mShard->mScopes;
			auto next          = std::ranges::upper_bound(scopes.begin, offset);
			if (next == scopes.begin.begin())
			{
				return kInvalid;
			}

			auto index = static_cast<std::uint32_t>(next - scopes.begin.begin()) - 1;
			while (index != kInvalid && scopes.end[index] <= offset)
			{
				index = scopes.parent[index];
			}

			return index;
		}

		// `using namespace a::b;`, `using a::B;` and `using X = a::B...;` outside bodies.
		void CollectUsings()
		{
			const auto& sig    = mModel.Significant();
			const auto& tokens = mModel.Tree().Tokens();
			auto& out          = mShard->mUsings;
			const auto isName =[&](std::size_t at)
			{
				return at < sig.size() && tokens[sig[at]].kind == TokenKind::Identifier &&
					tokens[sig[at]].tok == Tok::None;
			};
			for (std::size_t i = 0; i + 2 < sig.size(); ++i)
			{
				if (tokens[sig[i]].tok != Tok::KwUsing ||
					InsideBody(static_cast<std::uint32_t>(tokens[sig[i]].offset)))
				{
					continue;
				}

				auto at   = i + 1;
				auto kind = UsingKind::Declaration;
				std::string name;
				if (tokens[sig[at]].tok == Tok::KwNamespace)
				{
					kind = UsingKind::Namespace;
					++at;
				}
				else if (isName(at) && at + 1 < sig.size() && tokens[sig[at + 1]].tok == Tok::Eq)
				{
					kind = UsingKind::Alias;
					name = std::string(TokenText(sig[at]));
					at += 2;
				}

				std::string path;
				std::vector<std::uint32_t> pathTokens;
				if (at < sig.size() && tokens[sig[at]].tok == Tok::ColonColon)
				{
					path = "::";
					++at;
				}

				std::string last;
				while (isName(at))
				{
					last = std::string(TokenText(sig[at]));
					path += last;
					pathTokens.push_back(sig[at]);
					++at;
					if (at < sig.size() && tokens[sig[at]].tok == Tok::ColonColon)
					{
						path += "::";
						++at;
						continue;
					}

					break;
				}

				if (path.empty() || path.ends_with("::"))
				{
					continue;
				}

				if (kind == UsingKind::Declaration)
				{
					name = last;
				}

				mExtraUses.insert(mExtraUses.end(), pathTokens.begin(), pathTokens.end());
				out.offset.emplace_back(static_cast<std::uint32_t>(tokens[sig[i]].offset));
				out.scope.push_back(ScopeAtOffset(static_cast<std::uint32_t>(tokens[sig[i]].offset)));
				out.kind.push_back(kind);
				out.name.push_back(mShard->mStrings.Intern(name));
				out.path.push_back(mShard->mStrings.Intern(path));
			}
		}

		std::string WrittenReceiver(std::uint32_t node, bool arrow) const
		{
			const auto& tree = mModel.Tree();
			const auto& soa  = tree.NodesSoA();
			if (soa.Kind(node) != GrammarKind::IdentifierExpression || soa.TokenCount(node) != 1)
			{
				return {};
			}

			const auto symbol = mModel.ResolveToken(soa.FirstToken(node));
			if (symbol == kNone)
			{
				return {};
			}

			const auto& symbols    = mModel.Symbols();
			const auto declaration = symbols.decl_node[symbol];
			if (declaration >= soa.size())
			{
				return {};
			}

			std::string name;
			bool needsName         = true;
			std::uint32_t pointers = 0;
			for (auto token = soa.FirstToken(declaration); token < symbols.decl_token[symbol]; ++token)
			{
				const auto& current = tree.Tokens()[token];
				if (current.kind == TokenKind::Whitespace || current.kind == TokenKind::LineComment ||
					current.kind == TokenKind::BlockComment || current.tok == Tok::KwConst ||
					current.tok == Tok::KwVolatile)
				{
					continue;
				}

				if (current.kind == TokenKind::Identifier && current.tok == Tok::None && needsName)
				{
					const auto bound = mModel.Lookup(mModel.ScopeOfNode(declaration),
						mModel.Names().Find(TokenText(token)), token);
					if (bound != kNone && symbols.kind[bound] != SymbolKind::Class &&
						symbols.kind[bound] != SymbolKind::Namespace)
					{
						return {};
					}

					name += TokenText(token);
					needsName = false;
				}
				else if (current.tok == Tok::ColonColon && (name.empty() ||!needsName))
				{
					name += "::";
					needsName = true;
				}
				else if (!needsName && current.tok == Tok::Star)
				{
					++pointers;
				}
				else if (!needsName && (current.tok == Tok::Amp || current.tok == Tok::AmpAmp))
				{
					continue;
				}
				else
				{
					return {};
				}
			}

			return !needsName && pointers ==(arrow ? 1u : 0u) ? name : std::string {};
		}

		const TypeModel& Types()
		{
			if (!mTypes)
			{
				mTypes.emplace(Typer::Type(mModel));
			}
			return *mTypes;
		}

		std::uint8_t BuiltinCode(TypeId type, bool parameter)
		{
			const auto& table = Types().Types();
			if (parameter && (table.Kind(type) == TypeKind::LRef || table.Kind(type) == TypeKind::RRef))
			{
				return kUnknownBuiltin;
			}
			type = table.Strip(type);
			return table.Kind(type) == TypeKind::Builtin ? static_cast<std::uint8_t>(table.Arg(type)) : kUnknownBuiltin;
		}

		void CollectParameterTypes(SymbolId symbol)
		{
			auto& out = mShard->mSymbols;
			const auto begin = static_cast<std::uint32_t>(mShard->mParameterTypes.size());
			out.paramTypeBegin.push_back(begin);
			if (mModel.Symbols().kind[symbol] == SymbolKind::Function)
			{
				const auto declaration = mModel.Symbols().decl_node[symbol];
				const auto& soa = mModel.Tree().NodesSoA();
				for (auto node = declaration + 1; node < soa.SubtreeEnd(declaration); ++node)
				{
					if (soa.Kind(node) == GrammarKind::CompoundStatement)
					{
						break;
					}
					if (soa.Kind(node) == GrammarKind::ParameterDeclaration)
					{
						const auto code = BuiltinCode(Types().ParameterType(node), true);
						if (code != static_cast<std::uint8_t>(BuiltinType::Void))
						{
							mShard->mParameterTypes.push_back(code);
						}
						node = static_cast<std::uint32_t>(soa.SubtreeEnd(node)) - 1;
					}
				}
			}
			out.paramTypeCount.push_back(static_cast<std::uint32_t>(mShard->mParameterTypes.size()) - begin);
		}

		void CollectCallTypes()
		{
			const auto& tree = mModel.Tree();
			const auto& soa = tree.NodesSoA();
			for (std::uint32_t node = 0; node < soa.size(); ++node)
			{
				if (soa.Kind(node) != GrammarKind::CallExpression)
				{
					continue;
				}
				const auto children = mModel.ChildrenOf(node);
				if (children.empty())
				{
					continue;
				}
				auto callee = children.front();
				if (soa.Kind(callee) == GrammarKind::MemberExpression)
				{
					const auto members = mModel.ChildrenOf(callee);
					if (members.size() != kMemberChildCount)
					{
						continue;
					}
					callee = members.back();
				}
				if (soa.Kind(callee) != GrammarKind::IdentifierExpression || soa.TokenCount(callee) != 1)
				{
					continue;
				}
				const auto begin = static_cast<std::uint32_t>(mShard->mArgumentTypes.size());
				for (const auto argument : children.subspan(1))
				{
					mShard->mArgumentTypes.push_back(BuiltinCode(Types().NodeType(argument), false));
				}
				mCalls.emplace(static_cast<std::uint32_t>(tree.Tokens()[soa.FirstToken(callee)].offset),
					std::pair(begin, static_cast<std::uint32_t>(children.size() - 1)));
			}
		}

		// `this`, `*this`, or a variable declared `auto[&|*] name = [*]this;`.
		bool ReceiverIsThis(std::uint32_t node) const
		{
			const auto& tree   = mModel.Tree();
			const auto& soa    = tree.NodesSoA();
			const auto& tokens = tree.Tokens();
			if (soa.Kind(node) != GrammarKind::IdentifierExpression || soa.TokenCount(node) != 1)
			{
				return false;
			}

			const auto first = soa.FirstToken(node);
			if (tokens[first].tok == Tok::KwThis)
			{
				return true;
			}

			const auto symbol = mModel.ResolveToken(first);
			if (symbol == kNone || mModel.Symbols().kind[symbol] != heimdall::SymbolKind::Variable)
			{
				return false;
			}

			const auto declaration = mModel.Symbols().decl_node[symbol];
			const auto declToken   = mModel.Symbols().decl_token[symbol];
			if (declaration >= soa.size())
			{
				return false;
			}

			bool isAuto = false;
			for (auto token = soa.FirstToken(declaration); token < declToken; ++token)
			{
				isAuto |= tokens[token].tok == Tok::KwAuto;
			}

			const auto& sig = mModel.Significant();
			auto at         = std::ranges::upper_bound(sig, declToken);
			if (!isAuto || at == sig.end() || tokens[*at].tok != Tok::Eq)
			{
				return false;
			}

			++at;
			if (at != sig.end() && tokens[*at].tok == Tok::Star)
			{
				++at;
			}

			return at != sig.end() && tokens[*at].tok == Tok::KwThis;
		}

		// "::A" for the class whose member function contains `offset`; empty when unknown.
		std::string EnclosingClassName(std::uint32_t offset) const
		{
			const auto scope = ScopeAtOffset(offset);
			if (scope == kInvalid)
			{
				return {};
			}

			const auto qualified = mShard->mStrings.Text(mShard->mScopes.qualified[scope]);
			return qualified.empty() ? std::string {} : "::" + std::string(qualified);
		}

		void CollectReceivers()
		{
			const auto& tree = mModel.Tree();
			const auto& soa  = tree.NodesSoA();
			bool hasMembers  = false;
			for (std::uint32_t node = 0; node < soa.size(); ++node)
			{
				hasMembers |= soa.Kind(node) == GrammarKind::MemberExpression;
			}

			if (!hasMembers)
			{
				return;
			}

			const auto& types  = Types();
			const auto& table = types.Types();
			for (std::uint32_t node = 0; node < soa.size(); ++node)
			{
				if (soa.Kind(node) != GrammarKind::MemberExpression)
				{
					continue;
				}

				const auto children = mModel.ChildrenOf(node);
				if (children.size() != kMemberChildCount)
				{
					continue;
				}

				const auto token        = soa.FirstToken(children[1]);
				const auto& significant = mModel.Significant();
				const auto at           = std::ranges::lower_bound(significant, token);
				if (at == significant.begin() || at == significant.end())
				{
					continue;
				}

				const bool arrow = tree.Tokens()[at[-1]].tok == Tok::Arrow;
				auto type        = table.Strip(types.NodeType(children[0]));
				if (arrow && type != TypeTable::Unknown)
				{
					if (table.Kind(type) != TypeKind::Pointer)
					{
						continue;
					}

					type = table.Strip(table.Arg(type));
				}

				std::string name;
				if (table.Kind(type) == TypeKind::Class)
				{
					const auto symbol = table.Arg(type);
					if (symbol < mLocal.size() && mLocal[symbol] != kInvalid)
					{
						name = "::" + std::string(mShard->Qualified(mLocal[symbol]));
					}
				}
				else if (table.Kind(type) == TypeKind::External)
				{
					name = types.Spell(type);
					if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_:") != std::string::npos)
					{
						name.clear();
					}
				}
				else if (type == TypeTable::Unknown)
				{
					name = WrittenReceiver(children[0], arrow);
					if (name.empty() && ReceiverIsThis(children[0]))
					{
						name = EnclosingClassName(static_cast<std::uint32_t>(tree.Tokens()[token].offset));
					}
				}

				if (!name.empty())
				{
					const auto offset = static_cast<std::uint32_t>(tree.Tokens()[token].offset);
					auto typeOffset   = offset;
					if (soa.Kind(children[0]) == GrammarKind::IdentifierExpression)
					{
						const auto symbol = mModel.ResolveToken(soa.FirstToken(children[0]));
						if (symbol != kNone)
						{
							typeOffset = static_cast<std::uint32_t>(
								tree.Tokens()[mModel.Symbols().decl_token[symbol]].offset);
						}
					}

					mReceivers.emplace(static_cast<std::uint32_t>(tree.Tokens()[token].offset),
						Receiver {mShard->mStrings.Intern(name), ScopeAtOffset(typeOffset), typeOffset});
				}
			}
		}

		// Written qualifier, member access and call arity of the identifier at `offset`.
		void Annotate(std::uint32_t offset, OccurrenceColumns& out) const
		{
			const auto& sig    = mModel.Significant();
			const auto& tokens = mModel.Tree().Tokens();
			out.scope.push_back(ScopeAtOffset(offset));
			const auto call = mCalls.find(offset);
			out.argTypeBegin.push_back(call == mCalls.end() ? 0 : call->second.first);
			out.argTypeCount.push_back(call == mCalls.end() ? 0 : call->second.second);
			const auto receiver = mReceivers.find(offset);
			out.receiver.push_back(receiver == mReceivers.end() ? kInvalid : receiver->second.name);
			out.receiverScope.push_back(receiver == mReceivers.end() ? kInvalid : receiver->second.scope);
			out.receiverOffset.push_back(receiver == mReceivers.end() ? offset : receiver->second.offset);
			auto at = std::ranges::lower_bound(sig, offset, {},
				[&](std::uint32_t token)
				{
					return tokens[token].offset;
			});
			if (at == sig.end() || tokens[*at].offset != offset)
			{
				out.qualifier.push_back(kInvalid);
				out.argCount.push_back(kNoArity);
				out.flags.push_back(0);
				return;
			}

			const auto index = static_cast<std::size_t>(at - sig.begin());
			auto first       = index;
			while (first >= 2 && tokens[sig[first - 1]].tok == Tok::ColonColon &&
				tokens[sig[first - 2]].kind == TokenKind::Identifier &&
				tokens[sig[first - 2]].tok == Tok::None)
			{
				first -= 2;
			}

			std::string qualifier;
			if (first >= 1 && tokens[sig[first - 1]].tok == Tok::ColonColon)
			{
				qualifier = "::";
			}

			for (auto part = first; part < index; part += 2)
			{
				qualifier += TokenText(sig[part]);
				qualifier += part + 2 < index ? "::" : "";
			}

			const bool written = first != index ||!qualifier.empty();
			out.qualifier.push_back(written ? mShard->mStrings.Intern(qualifier) : kInvalid);
			std::uint8_t flags = 0;
			if (index > 0 && (tokens[sig[index - 1]].tok == Tok::Dot ||
				tokens[sig[index - 1]].tok == Tok::Arrow))
			{
				flags |= kOccurrenceMember;
			}

			out.flags.push_back(flags);
			auto next = index + 1;
			if (next < sig.size() && tokens[sig[next]].tok == Tok::Lt)
			{
				int depth = 0;
				for (; next < sig.size(); ++next)
				{
					depth += tokens[sig[next]].tok == Tok::Lt;
					depth -= tokens[sig[next]].tok == Tok::Gt;
					if (depth == 0 || tokens[sig[next]].tok == Tok::Semi)
					{
						break;
					}
				}

				++next;
			}

			std::uint8_t args = kNoArity;
			if (next < sig.size() && tokens[sig[next]].tok == Tok::LParen)
			{
				const auto counted = CountList(next);
				if (counted.valid && !counted.variadic && counted.count < kNoArity)
				{
					args = static_cast<std::uint8_t>(counted.count);
				}
			}

			out.argCount.push_back(args);
		}

		void CollectOccurrences()
		{
			const auto& symbols = mModel.Symbols();
			const auto& tokens  = mModel.Tree().Tokens();
			std::vector<Pending> pending;
			std::vector<std::uint32_t> declTokens;
			for (std::uint32_t local = 0; local < mKept.size(); ++local)
			{
				const auto token = symbols.decl_token[mKept[local]];
				declTokens.push_back(token);
				pending.push_back({tokens[token].offset, tokens[token].length, local,
						mShard->mSymbols.name[local], mShard->mSymbols.role[local],
						Resolution::Resolved});
			}

			std::ranges::sort(declTokens);
			const auto& refs = mModel.Refs();
			for (std::size_t i = 0; i < refs.token.size(); ++i)
			{
				const auto token = refs.token[i];
				if (token >= tokens.size() || tokens[token].kind != TokenKind::Identifier ||
					std::ranges::binary_search(declTokens, token))
				{
					continue;
				}

				const auto target = refs.target[i];
				if (target != kNone && mLocal[target] == kInvalid)
				{
					continue; // a local or parameter: not visible across files
				}

				const auto resolution = target != kNone ? Resolution::Resolved
				: refs.state[i] == RefState::Dependent ? Resolution::Dependent
				: Resolution::Unresolved;
				pending.push_back({tokens[token].offset, tokens[token].length,
						target == kNone ? kInvalid : mLocal[target],
						mShard->mStrings.Intern(TokenText(token)), Role::Reference, resolution});
			}

			for (const auto& item : mForward)
			{
				const auto& info = tokens[item.token];
				pending.push_back({static_cast<std::uint32_t>(info.offset),
						static_cast<std::uint32_t>(info.length), item.symbol,
						mShard->mStrings.Intern(TokenText(item.token)), Role::Declaration,
						Resolution::Resolved});
			}

			CollectBaseUses(pending);
			CollectTypeUses(pending, declTokens);

			std::ranges::stable_sort(pending,
				[](const Pending& a, const Pending& b)
				{
					return a.offset < b.offset;
			});
			auto& out = mShard->mOccurrences;
			out.offset.reserve(pending.size());
			for (const auto& item : pending)
			{
				out.offset.push_back(item.offset);
				out.length.push_back(item.length);
				out.symbol.push_back(item.symbol);
				out.name.push_back(item.name);
				out.role.push_back(item.role);
				out.resolution.push_back(item.resolution);
				Annotate(item.offset, out);
			}
		}

		std::uint32_t BaseNameOffset(std::size_t slot) const
		{
			const auto& sig    = mModel.Significant();
			const auto& tokens = mModel.Tree().Tokens();
			auto at            = std::ranges::lower_bound(sig, mModel.Bases().token[slot]);
			if (at == sig.end())
			{
				return kInvalid;
			}

			while (at + 2 < sig.end() && tokens[at[1]].tok == Tok::ColonColon &&
				tokens[at[2]].kind == TokenKind::Identifier)
			{
				at += 2;
			}

			return static_cast<std::uint32_t>(tokens[*at].offset);
		}

		void CollectRelations()
		{
			const auto& symbols = mModel.Symbols();
			const auto& bases   = mModel.Bases();
			auto& out           = mShard->mRelations;
			for (std::uint32_t local = 0; local < mKept.size(); ++local)
			{
				const auto klass = mKept[local];
				if (symbols.kind[klass] != heimdall::SymbolKind::Class)
				{
					continue;
				}

				const auto derived = mShard->mSymbols.hash[local];
				for (std::uint32_t i = 0; i < symbols.base_count[klass]; ++i)
				{
					const auto slot   = symbols.first_base[klass] + i;
					const auto target = bases.target[slot];
					if (target != kNone && mLocal[target] != kInvalid)
					{
						out.subject.push_back(mShard->mSymbols.hash[mLocal[target]]);
						out.kind.push_back(RelationKind::BaseOf);
						out.offset.push_back(kInvalid);
					}
					else if (bases.name[slot] != kNone)
					{
						out.subject.push_back(
							HashBytes(mModel.Names().Text(bases.name[slot])));
						out.kind.push_back(RelationKind::BaseOfByName);
						out.offset.push_back(BaseNameOffset(slot));
					}
					else
					{
						continue;
					}

					out.object.push_back(derived);
				}
			}
		}

		void CollectIncludes()
		{
			const auto source = mModel.Tree().Source();
			for (const auto& directive : mModel.Tree().Directives())
			{
				if (directive.kind != DirectiveKind::Include || !directive.active ||
					directive.offset + directive.length > source.size())
				{
					continue;
				}

				const auto line = source.substr(directive.offset, directive.length);
				const auto open = line.find_first_of("\"<");
				const auto close =
					open == std::string_view::npos
				? std::string_view::npos
				: line.find(line[open] == '<' ? '>' : '"', open + 1);
				if (close != std::string_view::npos)
				{
					const auto spelling = line.substr(open + 1, close - open - 1);
					mShard->mIncludes.push_back(
						mShard->mStrings.Intern(spelling));
					mShard->mIncludeOffsets.emplace_back(static_cast<std::uint32_t>(directive.offset));
					const auto target = mResolveInclude ? mResolveInclude(spelling, line[open] == '"',
						static_cast<std::uint32_t>(directive.offset)) : std::nullopt;
					mShard->mIncludeTargets.push_back(target && !target->empty() ?
						mShard->mStrings.Intern(*target) : kInvalid);
					mShard->mExactIncludes &= target.has_value() && !target->empty();
				}
				else
				{
					mShard->mExactIncludes = false;
				}
			}
		}

		const SemanticModel& mModel;
		std::unique_ptr<FileShard> mShard;
		ShardBuilder::IncludeResolver mResolveInclude;
		std::optional<TypeModel> mTypes;
		std::unordered_map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> mCalls;
		std::vector<std::uint32_t> mLocal; // global SymbolId -> shard-local index
		std::vector<SymbolId> mKept;       // shard-local index -> global SymbolId
		std::vector<Forward> mForward;
		struct Receiver
		{
			StringId name;
			std::uint32_t scope;
			std::uint32_t offset;
		};

		std::unordered_map<std::uint32_t, Receiver> mReceivers;
		std::vector<std::uint32_t> mExtraUses; // path names of `using`, `friend` class names
	};

	std::shared_ptr<const FileShard> ShardBuilder::Build(
		const SemanticModel& model, std::string path, std::uint64_t configHash, IncludeResolver resolveInclude)
	{
		return ShardBuilderImpl(model, std::move(path), configHash, std::move(resolveInclude)).Run();
	}

	std::uint64_t ShardBuilder::ConfigurationHash(const ParserOptions& options, const CompileCommand* command)
	{
		std::uint64_t hash = HashBytes(std::to_string(static_cast<unsigned>(options.standard)));
		const auto append = [&](std::string_view text)
		{
			hash = HashBytes(std::to_string(text.size()) + ':', hash);
			hash = HashBytes(text, hash);
		};
		std::vector<std::pair<std::string_view, std::string_view>> macros;
		macros.reserve(options.Macros().size());
		for (const auto& [name, value] : options.Macros())
		{
			macros.emplace_back(name, value);
		}

		std::ranges::sort(macros);
		for (const auto& [name, value] : macros)
		{
			append(name);
			append(value);
		}

		append(std::to_string(options.type_names ? options.type_names->Fingerprint() : 0));
		if (command != nullptr)
		{
			append(command->directory.lexically_normal().generic_string());
			for (const auto& argument : command->arguments)
			{
				append(argument);
			}

			for (const auto& path : command->quote_directories)
			{
				append(path.lexically_normal().generic_string());
			}
			append("include-directories");
			for (const auto& path : command->include_directories)
			{
				append(path.lexically_normal().generic_string());
			}
		}
		return hash;
	}
} // namespace heimdall::index
