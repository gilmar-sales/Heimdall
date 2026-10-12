#include <Heimdall/ShardBuilder.hpp>

#include <algorithm>
#include <ranges>

namespace heimdall::index
{
	namespace
	{
		enum class Group : char
		{
			Namespace = 1,
			Type = 2,
			Function = 3,
			Value = 4
		};

		Group GroupOf(heimdall::SymbolKind kind) noexcept
		{
			switch (kind)
			{
			case heimdall::SymbolKind::Namespace:
				return Group::Namespace;
			case heimdall::SymbolKind::Class:
			case heimdall::SymbolKind::Enum:
			case heimdall::SymbolKind::TypeAlias:
				return Group::Type;
			case heimdall::SymbolKind::Function:
				return Group::Function;
			default:
				return Group::Value;
			}
		}

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
		ShardBuilderImpl(const SemanticModel& model, std::string path, std::uint64_t configHash) :
		mModel(model), mShard(std::make_unique<FileShard>())
		{
			mShard->mPath        = std::move(path);
			mShard->mConfigHash  = configHash;
			mShard->mContentHash = HashBytes(model.Tree().Source());
		}

		std::shared_ptr<const FileShard> Run()
		{
			CollectSymbols();
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
		void AppendWrittenQualifier(std::uint32_t token, std::string& out) const
		{
			const auto& sig    = mModel.Significant();
			const auto& tokens = mModel.Tree().Tokens();
			auto at            = std::ranges::lower_bound(sig, token);
			if (at == sig.end() || *at != token)
			{
				return;
			}

			std::vector<std::string_view> parts;
			while (at - sig.begin() >= 2 && tokens[at[-1]].tok == Tok::ColonColon &&
				tokens[at[-2]].kind == TokenKind::Identifier)
			{
				parts.push_back(TokenText(at[-2]));
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
					AppendWrittenQualifier(symbols.decl_token[symbol], qualified);
				}

				qualified += name;

				const auto kind  = symbols.kind[symbol];
				const auto scope = mModel.Scopes().kind[symbols.scope[symbol]];
				internal |= scope != heimdall::ScopeKind::Class &&
					(flags& SymbolFlag::Static) != 0 && kind != heimdall::SymbolKind::Namespace;

				std::string key(1, static_cast<char>(GroupOf(kind)));
				key += qualified;
				key += '\0';
				if (kind == heimdall::SymbolKind::Function)
				{
					key.append(reinterpret_cast<const char*>(&symbols.signature[symbol]),
						sizeof(std::uint64_t));
				}

				auto hash = HashBytes(key);
				if (internal)
				{
					hash = HashBytes(mShard->mPath, hash);
				}

				const auto container = symbols.scope[symbol] < mModel.Scopes().Size()
				? mModel.Scopes().owner[symbols.scope[symbol]]
				: kNone;
				const auto tokenInfo = mModel.Tree().Tokens()[symbols.decl_token[symbol]];

				out.hash.push_back(hash);
				out.nameMask.push_back(CharacterMask(name));
				out.name.push_back(mShard->mStrings.Intern(name));
				out.qualified.push_back(mShard->mStrings.Intern(qualified));
				out.container.push_back(container == kNone ? kInvalid : mLocal[container]);
				out.flags.push_back(flags);
				out.nameOffset.push_back(tokenInfo.offset);
				out.nameLength.push_back(tokenInfo.length);
				out.kind.push_back(kind);
				out.role.push_back(RoleOf(symbol));
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
			}
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
					}
					else if (bases.name[slot] != kNone)
					{
						out.subject.push_back(
							HashBytes(mModel.Names().Text(bases.name[slot])));
						out.kind.push_back(RelationKind::BaseOfByName);
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
				if (directive.kind != DirectiveKind::Include ||
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
					mShard->mIncludes.push_back(
						mShard->mStrings.Intern(line.substr(open + 1, close - open - 1)));
				}
			}
		}

		const SemanticModel& mModel;
		std::unique_ptr<FileShard> mShard;
		std::vector<std::uint32_t> mLocal; // global SymbolId -> shard-local index
		std::vector<SymbolId> mKept;       // shard-local index -> global SymbolId
	};

	std::shared_ptr<const FileShard> ShardBuilder::Build(
		const SemanticModel& model, std::string path, std::uint64_t configHash)
	{
		return ShardBuilderImpl(model, std::move(path), configHash).Run();
	}
} // namespace heimdall::index
