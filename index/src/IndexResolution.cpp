#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>

#include <Heimdall/IndexSnapshot.hpp>

namespace heimdall::index
{
	namespace
	{
		constexpr std::size_t kQualifierSeparatorLength = 2;
		struct UsingRef
		{
			UsingKind kind;
			std::string_view name;
			std::string_view path;
			std::uint32_t shard;
			std::uint32_t offset;
		};

		struct Candidate
		{
			SymbolHash hash;
			SymbolRef symbol;
		};

		std::string Join(std::string_view prefix, std::string_view suffix)
		{
			std::string result(prefix);
			if (!prefix.empty() && !suffix.empty())
			{
				result += "::";
			}

			result += suffix;
			return result;
		}
	} // namespace

	const ShardResolution& IndexSnapshot::Resolved(std::uint32_t shard) const
	{
		std::call_once(mResolveOnce[shard],
			[&]
			{
				ResolveShard(shard, mResolution[shard]);
			});
		return mResolution[shard];
	}

	std::vector<bool> IndexSnapshot::Visible(std::uint32_t shard) const
	{
		std::vector<bool> visible(mShards.size(), false);
		std::vector<std::uint32_t> pending {shard};
		visible[shard] = true;
		while (!pending.empty())
		{
			const auto current = pending.back();
			pending.pop_back();
			for (auto i = mIncludesBegin[current]; i < mIncludesBegin[current + 1]; ++i)
			{
				if (!visible[mIncluded[i]])
				{
					visible[mIncluded[i]] = true;
					pending.push_back(mIncluded[i]);
				}
			}
		}

		return visible;
	}

	void IndexSnapshot::ResolveShard(std::uint32_t shard, ShardResolution& out) const
	{
		const auto& file   = *mShards[shard];
		const auto& occ    = file.Occurrences();
		const auto visible = Visible(shard);

		std::unordered_map<std::string, std::vector<UsingRef>> usingsAt;
		for (std::uint32_t other = 0; other < mShards.size(); ++other)
		{
			if (!visible[other])
			{
				continue;
			}

			const auto& source = *mShards[other];
			const auto& usings = source.Usings();
			for (std::uint32_t i = 0; i < usings.Size(); ++i)
			{
				const auto level = usings.scope[i] == kInvalid
				? std::string_view {}
				: source.Strings().Text(source.Scopes().qualified[usings.scope[i]]);
				usingsAt[std::string(level)].emplace_back(usings.kind[i],
					source.Strings().Text(usings.name[i]), source.Strings().Text(usings.path[i]),
					other, usings.offset[i]);
			}
		}

		// Whether the occurrence being resolved names a macro: macros only match macros and
		// every other name never matches a macro.
		bool macroUse = false;
		const auto find = [&](std::string_view qualified, std::vector<Candidate>& found)
		{
			const auto key   = HashBytes(qualified);
			const auto range = std::ranges::equal_range(mQualHash, key);
			for (auto at = range.begin() - mQualHash.begin(); at < range.end() - mQualHash.begin(); ++at)
			{
				const auto candidateShard = mQualShard[at];
				const auto symbol         = mQualSymbol[at];
				if (!visible[candidateShard] ||
					(mShards[candidateShard]->Symbols().kind[symbol] == heimdall::SymbolKind::Macro) !=
					macroUse ||
					mShards[candidateShard]->Qualified(symbol) != qualified ||
					(mShards[candidateShard]->Symbols().flags[symbol] &
					(SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0)
				{
					continue;
				}

				const auto hash = mShards[candidateShard]->Symbols().hash[symbol];
				if (std::ranges::none_of(found,[&](const Candidate& c)
					{
						return c.hash == hash && mShards[c.symbol.shard]->Identity(c.symbol.symbol) ==
						mShards[candidateShard]->Identity(symbol);
					}))
				{
					found.push_back({hash, {candidateShard, symbol}});
				}
			}
		};

		const auto fits = [&](const Candidate& candidate, std::uint8_t args)
		{
			const auto& symbols = mShards[candidate.symbol.shard]->Symbols();
			const auto symbol   = candidate.symbol.symbol;
			if (args == kNoArity || symbols.kind[symbol] != heimdall::SymbolKind::Function ||
				symbols.paramMin[symbol] == kNoArity)
			{
				return true;
			}

			return args >= symbols.paramMin[symbol] &&
				(symbols.paramMax[symbol] == kNoArity || args <= symbols.paramMax[symbol]);
		};

		std::function<bool(SymbolRef, std::string_view, std::vector<Candidate>&,
			std::set<SymbolHash>&) > collectMembers;
		const auto lookup = [&](std::uint32_t sourceShard, std::uint32_t scope, std::uint32_t offset,
			std::string_view written,
			std::vector<Candidate>& found, bool inherited = true)
		{
			const auto& source  = *mShards[sourceShard];
			const bool absolute = written.starts_with("::");
			if (absolute)
			{
				find(written.substr(kQualifierSeparatorLength), found);
				return true;
			}

			for (;;)
			{
				const auto here = scope == kInvalid ? std::string_view {}:
				source.Strings().Text(source.Scopes().qualified[scope]);
				find(Join(here, written), found);
				if (inherited && found.empty() && !here.empty())
				{
					for (const auto owner : Named(here))
					{
						if (mShards[owner.shard]->Symbols().kind[owner.symbol] == heimdall::SymbolKind::Class)
						{
							std::set<SymbolHash> active;
							if (!collectMembers(owner, written, found, active))
							{
								return false;
							}

							break;
						}
					}
				}

				if (const auto used = usingsAt.find(std::string(here)); used != usingsAt.end())
				{
					for (const auto& use : used->second)
					{
						if (use.shard == sourceShard && use.offset >= offset)
						{
							continue;
						}

						if (use.kind == UsingKind::Namespace)
						{
							find(Join(use.path, written), found);
							find(Join(Join(here, use.path), written), found);
						}
						else if (use.kind == UsingKind::Declaration && use.name == written)
						{
							find(use.path, found);
						}
					}
				}

				if (!found.empty() || scope == kInvalid)
				{
					break;
				}

				scope = source.Scopes().parent[scope];
			}

			return true;
		};

		collectMembers = [&](SymbolRef owner, std::string_view name, std::vector<Candidate>& found,
			std::set<SymbolHash>& active)
		{
			const auto hash = HashOf(owner);
			if (!active.insert(hash).second)
			{
				return false;
			}

			const auto& declaring = *mShards[owner.shard];
			std::vector<Candidate> own;
			find(Join(declaring.Qualified(owner.symbol), name), own);
			if (!own.empty())
			{
				found.insert(found.end(), own.begin(), own.end());
				active.erase(hash);
				return true;
			}

			bool complete = true;
			std::set<SymbolHash> bases;
			for (const auto declaration : Declarations(hash))
			{
				const auto& baseFile = *mShards[declaration.shard];
				for (const auto relation : baseFile.RelationsTo(hash))
				{
					SymbolRef base;
					if (baseFile.Relations().kind[relation] == RelationKind::BaseOf)
					{
						const auto targets = Declarations(baseFile.Relations().subject[relation]);
						if (!targets.empty())
						{
							base = targets.front();
						}
					}
					else
					{
						const auto occurrence = baseFile.OccurrenceAt(baseFile.Relations().offset[relation]);
						if (occurrence != kInvalid)
						{
							const auto& uses = baseFile.Occurrences();
							const auto qualifier = uses.qualifier[occurrence] == kInvalid ? std::string_view {}:
							baseFile.Strings().Text(uses.qualifier[occurrence]);
							const auto spelling = baseFile.Strings().Text(uses.name[occurrence]);
							std::vector<Candidate> targets;
							lookup(declaration.shard, uses.scope[occurrence], uses.offset[occurrence],
								qualifier == "::" ? "::" + std::string(spelling) : Join(qualifier, spelling), targets, false);
							if (targets.size() == 1 && mShards[targets.front().symbol.shard]->Symbols().kind[targets.front().symbol.symbol] == heimdall::SymbolKind::Class)
							{
								base = targets.front().symbol;
							}
						}
					}

					if (!base.Valid())
					{
						complete = false;
					}
					else if (bases.insert(HashOf(base)).second)
					{
						complete &= collectMembers(base, name, found, active);
					}
				}
			}

			active.erase(hash);
			return complete;
		};

		out.begin.assign(1, 0);
		out.state.reserve(occ.Size());
		for (std::uint32_t i = 0; i < occ.Size(); ++i)
		{
			std::vector<Candidate> found;
			auto state = occ.resolution[i];
			macroUse   = (occ.flags[i] & kOccurrenceMacro) != 0;
			if (occ.symbol[i] != kInvalid)
			{
				found.push_back({file.Symbols().hash[occ.symbol[i]], {shard, occ.symbol[i]}});
				bool classScope = false;
				for (auto scope = occ.scope[i]; scope != kInvalid; scope = file.Scopes().parent[scope])
				{
					for (const auto owner : Named(file.Strings().Text(file.Scopes().qualified[scope])))
					{
						classScope |= mShards[owner.shard]->Symbols().kind[owner.symbol] == heimdall::SymbolKind::Class;
					}
				}

				const auto kind = file.Symbols().kind[occ.symbol[i]];
				if (occ.role[i] == Role::Reference && (kind == heimdall::SymbolKind::Function ||
					(classScope && kind == heimdall::SymbolKind::Variable)))
				{
					std::vector<Candidate> candidates;
					const auto qualifier = occ.qualifier[i] == kInvalid ? std::string_view {}: file.Strings().Text(occ.qualifier[i]);
					const auto name      = file.Strings().Text(occ.name[i]);
					const bool complete = lookup(shard, occ.scope[i], occ.offset[i],
						qualifier == "::" ? "::" + std::string(name) : Join(qualifier, name), candidates);
					found = std::move(candidates);
					state = complete && found.size() == 1 ? Resolution::Resolved :
					complete && !found.empty() ? Resolution::Ambiguous : Resolution::Unresolved;
				}
			}
			else if (state == Resolution::Unresolved)
			{
				const auto name    = file.Strings().Text(occ.name[i]);
				bool receiverKnown = true;
				if ((occ.flags[i] & kOccurrenceMember) != 0)
				{
					std::vector<Candidate> owners;
					bool ownerComplete = true;
					if (occ.receiver[i] != kInvalid)
					{
						ownerComplete = lookup(shard, occ.receiverScope[i], occ.receiverOffset[i],
							file.Strings().Text(occ.receiver[i]), owners);
					}

					receiverKnown = ownerComplete && owners.size() == 1 &&
						mShards[owners.front().symbol.shard]->Symbols().kind[owners.front().symbol.symbol] ==
						heimdall::SymbolKind::Class;
					if (receiverKnown)
					{
						const auto owner = owners.front().symbol;
						std::set<SymbolHash> active;
						receiverKnown = collectMembers(owner, name, found, active);
					}
					else
					{
						const auto range = std::ranges::equal_range(mMemberHash, HashBytes(name));
						for (auto at = range.begin() - mMemberHash.begin();
							at < range.end() - mMemberHash.begin(); ++at)
						{
							const auto s      = mMemberShard[at];
							const auto symbol = mMemberSymbol[at];
							const auto hash   = mShards[s]->Symbols().hash[symbol];
							if (visible[s] && mShards[s]->Name(symbol) == name &&
								std::ranges::none_of(found,[&](const Candidate& c)
								{
									return c.hash == hash && mShards[c.symbol.shard]->Identity(c.symbol.symbol) ==
									mShards[s]->Identity(symbol);
								}))
							{
								found.push_back({hash, {s, symbol}});
							}
						}
					}
				}
				else
				{
					const std::string_view qualifier = occ.qualifier[i] == kInvalid
					? std::string_view {}
					: file.Strings().Text(occ.qualifier[i]);
					receiverKnown = lookup(shard, occ.scope[i], occ.offset[i],
						qualifier == "::" ? "::" + std::string(name) : Join(qualifier, name), found);
				}

				std::erase_if(found,[&](const Candidate& candidate)
					{
						return !fits(candidate, occ.argCount[i]);
					});

				state = found.empty() ||!receiverKnown ? Resolution::Unresolved
				: found.size() == 1 ? Resolution::Resolved
				: Resolution::Ambiguous;
			}

			if (occ.role[i] == Role::Reference && state != Resolution::Dependent)
			{
				std::erase_if(found,[&](const Candidate& candidate)
					{
						return !fits(candidate, occ.argCount[i]);
					});
				if (found.empty())
				{
					state = Resolution::Unresolved;
				}
				else if (state != Resolution::Unresolved)
				{
					state = found.size() == 1 ? Resolution::Resolved : Resolution::Ambiguous;
				}
			}

			if (state == Resolution::Ambiguous && file.ArgumentTypes(i).size() == occ.argCount[i])
			{
				const auto arguments = file.ArgumentTypes(i);
				bool known = std::ranges::none_of(arguments,[](std::uint8_t type)
					{
						return type == kUnknownBuiltin;
					});
				const auto exact = [&](const Candidate& candidate)
				{
					const auto parameters = mShards[candidate.symbol.shard]->ParameterTypes(candidate.symbol.symbol);
					return parameters.size() >= arguments.size() && std::ranges::equal(arguments,
						parameters.first(arguments.size()));
				};
				for (const auto& candidate : found)
				{
					const auto& source    = *mShards[candidate.symbol.shard];
					const auto parameters = source.ParameterTypes(candidate.symbol.symbol);
					known &= source.Symbols().kind[candidate.symbol.symbol] == heimdall::SymbolKind::Function &&
						(source.Symbols().flags[candidate.symbol.symbol] & heimdall::SymbolFlag::Template) == 0 &&
						source.Symbols().paramMax[candidate.symbol.symbol] != kNoArity && parameters.size() >= arguments.size();
					if (parameters.size() >= arguments.size())
					{
						known &= std::ranges::none_of(parameters.first(arguments.size()),[](std::uint8_t type)
							{
								return type == kUnknownBuiltin;
							});
					}
				}

				if (known && std::ranges::count_if(found, exact) == 1)
				{
					std::erase_if(found,[&](const Candidate& candidate)
						{
							return !exact(candidate);
						});
					state = Resolution::Resolved;
				}
			}

			for (const auto& candidate : found)
			{
				out.targets.push_back(candidate.symbol);
			}

			out.state.push_back(state);
			out.begin.push_back(static_cast<std::uint32_t>(out.targets.size()));
		}
	}
} // namespace heimdall::index
