#include <Heimdall/IndexSnapshot.hpp>

#include <algorithm>
#include <cctype>
#include <numeric>
#include <ranges>
#include <tuple>
#include <unordered_map>

namespace heimdall::index
{
	namespace
	{
		bool PathEndsWith(std::string_view path, std::string_view spelling)
		{
			if (spelling.empty() || path.size() < spelling.size() ||
				path.substr(path.size() - spelling.size()) != spelling)
			{
				return false;
			}

			return path.size() == spelling.size() || path[path.size() - spelling.size() - 1] == '/';
		}

		std::string_view FileName(std::string_view path)
		{
			const auto slash = path.rfind('/');
			return slash == std::string_view::npos ? path : path.substr(slash + 1);
		}

		// 0 = no match; higher is better.
		int Score(std::string_view name, std::string_view query)
		{
			const auto lower =[](char c)
			{
				return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			};

			if (query.empty())
			{
				return 1;
			}

			std::size_t matched = 0;
			for (const char c : name)
			{
				if (matched < query.size() && lower(c) == lower(query[matched]))
				{
					++matched;
				}
			}

			if (matched != query.size())
			{
				return 0;
			}

			const auto equal =[&](std::size_t from)
			{
				for (std::size_t i = 0; i < query.size(); ++i)
				{
					if (lower(name[from + i]) != lower(query[i]))
					{
						return false;
					}
				}

				return true;
			};

			if (name.size() == query.size() && equal(0))
			{
				return 4;
			}

			if (name.size() >= query.size() && equal(0))
			{
				return 3;
			}

			for (std::size_t from = 1; from + query.size() <= name.size(); ++from)
			{
				if (equal(from))
				{
					return 2;
				}
			}

			return 1;
		}
	} // namespace

	std::shared_ptr<const IndexSnapshot> IndexSnapshot::Build(
		std::vector<std::shared_ptr<const FileShard>> shards, std::uint64_t revision)
	{
		auto snapshot       = std::shared_ptr<IndexSnapshot>(new IndexSnapshot());
		snapshot->mShards   = std::move(shards);
		snapshot->mRevision = revision;
		auto& self          = *snapshot;

		std::vector<std::uint32_t> order;
		for (std::uint32_t shard = 0; shard < self.mShards.size(); ++shard)
		{
			const auto& symbols = self.mShards[shard]->Symbols();
			for (std::uint32_t symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				self.mDirHash.push_back(symbols.hash[symbol]);
				self.mDirShard.push_back(shard);
				self.mDirSymbol.push_back(symbol);
			}
		}

		order.resize(self.mDirHash.size());
		std::iota(order.begin(), order.end(), 0u);
		std::ranges::stable_sort(order,
			[&](std::uint32_t a, std::uint32_t b)
			{
				return self.mDirHash[a] < self.mDirHash[b];
		});
		const auto permute =[&](auto& column)
		{
			auto copy = column;
			for (std::size_t i = 0; i < order.size(); ++i)
			{
				column[i] = copy[order[i]];
			}
		};
		permute(self.mDirHash);
		permute(self.mDirShard);
		permute(self.mDirSymbol);

		// Name-matched uses.
		std::vector<std::uint32_t> unresolvedOrder;
		for (std::uint32_t shard = 0; shard < self.mShards.size(); ++shard)
		{
			const auto& file = *self.mShards[shard];
			const auto& occ  = file.Occurrences();
			for (std::uint32_t i = 0; i < occ.Size(); ++i)
			{
				if (occ.role[i] == Role::Reference)
				{
					self.mUnresolvedName.push_back(HashBytes(file.Strings().Text(occ.name[i])));
					self.mUnresolvedShard.push_back(shard);
					self.mUnresolvedOccurrence.push_back(i);
				}
			}
		}

		unresolvedOrder.resize(self.mUnresolvedName.size());
		std::iota(unresolvedOrder.begin(), unresolvedOrder.end(), 0u);
		std::ranges::stable_sort(unresolvedOrder,
			[&](std::uint32_t a, std::uint32_t b)
			{
				return self.mUnresolvedName[a] < self.mUnresolvedName[b];
		});
		order = std::move(unresolvedOrder);
		permute(self.mUnresolvedName);
		permute(self.mUnresolvedShard);
		permute(self.mUnresolvedOccurrence);

		// Paths and the reverse include graph.
		self.mByPath.resize(self.mShards.size());
		std::iota(self.mByPath.begin(), self.mByPath.end(), 0u);
		std::ranges::sort(self.mByPath,
			[&](std::uint32_t a, std::uint32_t b)
			{
				return self.mShards[a]->Path() < self.mShards[b]->Path();
		});

		std::unordered_map<std::string_view, std::vector<std::uint32_t>> byFileName;
		for (std::uint32_t shard = 0; shard < self.mShards.size(); ++shard)
		{
			byFileName[FileName(self.mShards[shard]->Path())].push_back(shard);
		}

		std::vector<std::pair<std::uint32_t, std::uint32_t>> edges; // (included, includer)
		for (std::uint32_t shard = 0; shard < self.mShards.size(); ++shard)
		{
			const auto& file = *self.mShards[shard];
			for (std::size_t include = 0; include < file.Includes().size(); ++include)
			{
				if (file.IncludeTargets()[include] != kInvalid)
				{
					const auto target = self.ShardFor(file.Strings().Text(file.IncludeTargets()[include]));
					if (target != kInvalid && target != shard)
					{
						edges.emplace_back(target, shard);
					}
					continue;
				}

				const auto spelling = file.Includes()[include];
				const auto text  = file.Strings().Text(spelling);
				const auto found = byFileName.find(FileName(text));
				if (found == byFileName.end())
				{
					continue;
				}

				for (const auto other : found->second)
				{
					if (other != shard && PathEndsWith(self.mShards[other]->Path(), text))
					{
						edges.emplace_back(other, shard);
					}
				}
			}
		}

		std::ranges::sort(edges);
		self.mIncludersBegin.assign(self.mShards.size() + 1, 0);
		for (const auto& edge : edges)
		{
			++self.mIncludersBegin[edge.first + 1];
			self.mIncluders.push_back(edge.second);
		}

		std::partial_sum(self.mIncludersBegin.begin(), self.mIncludersBegin.end(),
			self.mIncludersBegin.begin());

		auto forward = edges;
		for (auto& edge : forward)
		{
			std::swap(edge.first, edge.second);
		}

		std::ranges::sort(forward);
		self.mIncludesBegin.assign(self.mShards.size() + 1, 0);
		for (const auto& edge : forward)
		{
			++self.mIncludesBegin[edge.first + 1];
			self.mIncluded.push_back(edge.second);
		}

		std::partial_sum(self.mIncludesBegin.begin(), self.mIncludesBegin.end(),
			self.mIncludesBegin.begin());

		std::vector<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> qualified;
		std::vector<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> members;
		for (std::uint32_t shard = 0; shard < self.mShards.size(); ++shard)
		{
			const auto& file = *self.mShards[shard];
			for (std::uint32_t symbol = 0; symbol < file.Symbols().Size(); ++symbol)
			{
				qualified.emplace_back(HashBytes(file.Qualified(symbol)), shard, symbol);
				if (file.Symbols().container[symbol] != kInvalid)
				{
					members.emplace_back(HashBytes(file.Name(symbol)), shard, symbol);
				}
			}
		}

		const auto store =[](auto& rows, auto& hash, auto& shard, auto& symbol)
		{
			std::ranges::sort(rows);
			for (const auto& [key, s, y] : rows)
			{
				hash.push_back(key);
				shard.push_back(s);
				symbol.push_back(y);
			}
		};
		store(qualified, self.mQualHash, self.mQualShard, self.mQualSymbol);
		store(members, self.mMemberHash, self.mMemberShard, self.mMemberSymbol);
		self.mResolution.resize(self.mShards.size());
		self.mResolveOnce = std::make_unique<std::once_flag[] >(self.mShards.size());
		return snapshot;
	}

	std::uint32_t IndexSnapshot::ShardFor(std::string_view path) const noexcept
	{
		const auto found = std::ranges::lower_bound(mByPath, path, {},
			[&](std::uint32_t shard)->std::string_view
			{
				return mShards[shard]->Path();
		});
		return found != mByPath.end() && mShards[*found]->Path() == path ? *found : kInvalid;
	}

	SymbolRef IndexSnapshot::SymbolAt(std::uint32_t shard, std::uint32_t offset) const noexcept
	{
		if (shard >= mShards.size())
		{
			return {};
		}

		const auto symbol = mShards[shard]->SymbolAt(offset);
		return symbol == kInvalid ? SymbolRef {}: SymbolRef
		{
			shard, symbol
		};
	}

	SymbolHash IndexSnapshot::HashOf(SymbolRef symbol) const noexcept
	{
		return symbol.Valid() ? mShards[symbol.shard]->Symbols().hash[symbol.symbol] : 0;
	}

	std::vector<SymbolRef> IndexSnapshot::Declarations(SymbolHash hash) const
	{
		std::vector<SymbolRef> result;
		const auto range = std::ranges::equal_range(mDirHash, hash);
		std::string_view identity;
		for (auto i = static_cast<std::size_t>(range.begin() - mDirHash.begin());
			i < static_cast<std::size_t>(range.end() - mDirHash.begin()); ++i)
		{
			const auto key = mShards[mDirShard[i]]->Identity(mDirSymbol[i]);
			if (!result.empty() && key != identity)
			{
				return {};
			}

			identity = key;
			result.push_back({mDirShard[i], mDirSymbol[i]});
		}

		return result;
	}

	std::vector<SymbolRef> IndexSnapshot::Named(std::string_view qualified) const
	{
		std::vector<SymbolRef> result;
		const auto range = std::ranges::equal_range(mQualHash, HashBytes(qualified));
		for (auto i = range.begin() - mQualHash.begin(); i < range.end() - mQualHash.begin(); ++i)
		{
			if (mShards[mQualShard[i]]->Qualified(mQualSymbol[i]) == qualified)
			{
				result.emplace_back(mQualShard[i], mQualSymbol[i]);
			}
		}

		return result;
	}

	std::vector<SymbolRef> IndexSnapshot::Definitions(SymbolHash hash) const
	{
		auto result = Declarations(hash);
		std::erase_if(result,
			[&](SymbolRef ref)
			{
				return mShards[ref.shard]->Symbols().role[ref.symbol] != Role::Definition;
		});
		return result;
	}

	std::vector<SymbolRef> IndexSnapshot::Candidates(
		std::uint32_t shard, std::uint32_t offset) const
	{
		std::vector<SymbolRef> result;
		if (shard >= mShards.size())
		{
			return result;
		}

		const auto& file      = *mShards[shard];
		const auto& occ       = file.Occurrences();
		const auto occurrence = file.OccurrenceAt(offset);
		if (occurrence == kInvalid || occ.resolution[occurrence] == Resolution::Resolved)
		{
			return result;
		}

		const auto resolved = Resolved(shard).Targets(occurrence);
		if (!resolved.empty())
		{
			result.assign(resolved.begin(), resolved.end());
			return result;
		}

		const auto spelling = file.Strings().Text(occ.name[occurrence]);
		for (const auto found : Search(spelling, mDirHash.size()))
		{
			const auto& other = *mShards[found.shard];
			if (other.Name(found.symbol) != spelling || found.shard == shard)
			{
				continue;
			}

			std::vector<bool> reaches(mShards.size(), false);
			std::vector<std::uint32_t> pending {found.shard};
			reaches[found.shard] = true;
			while (!pending.empty() && !reaches[shard])
			{
				const auto current = pending.back();
				pending.pop_back();
				for (auto i = mIncludersBegin[current]; i < mIncludersBegin[current + 1]; ++i)
				{
					if (!reaches[mIncluders[i]])
					{
						reaches[mIncluders[i]] = true;
						pending.push_back(mIncluders[i]);
					}
				}
			}

			if (reaches[shard])
			{
				result.push_back(found);
			}
		}

		return result;
	}

	std::vector<Location> IndexSnapshot::References(SymbolHash hash) const
	{
		std::vector<Location> result;
		const auto declarations = Declarations(hash);
		if (declarations.empty())
		{
			return result;
		}

		std::vector<bool> declaring(mShards.size(), false);
		for (const auto ref : declarations)
		{
			declaring[ref.shard] = true;
			const auto& file     = *mShards[ref.shard];
			for (const auto index : file.OccurrencesOf(ref.symbol))
			{
				const auto& occ = file.Occurrences();
				if (occ.role[index] == Role::Reference)
				{
					continue;
				}
				result.push_back({ref.shard, occ.offset[index], occ.length[index], occ.role[index],
						Resolution::Resolved});
			}
		}

		// Shards that can see a declaring shard through `#include`, transitively.
		std::vector<bool> visible(mShards.size(), false);
		std::vector<std::uint32_t> pending;
		for (std::uint32_t shard = 0; shard < mShards.size(); ++shard)
		{
			if (declaring[shard])
			{
				visible[shard] = true;
				pending.push_back(shard);
			}
		}

		while (!pending.empty())
		{
			const auto current = pending.back();
			pending.pop_back();
			for (auto i = mIncludersBegin[current]; i < mIncludersBegin[current + 1]; ++i)
			{
				if (!visible[mIncluders[i]])
				{
					visible[mIncluders[i]] = true;
					pending.push_back(mIncluders[i]);
				}
			}
		}

		const auto& first     = *mShards[declarations.front().shard];
		const auto spelling   = first.Name(declarations.front().symbol);
		const auto nameHash   = HashBytes(spelling);
		const auto candidates = std::ranges::equal_range(mUnresolvedName, nameHash);
		const auto begin      = static_cast<std::size_t>(candidates.begin() - mUnresolvedName.begin());
		const auto end        = static_cast<std::size_t>(candidates.end() - mUnresolvedName.begin());
		for (auto i = begin; i < end; ++i)
		{
			const auto shard = mUnresolvedShard[i];
			const auto& file = *mShards[shard];
			const auto& occ  = file.Occurrences();
			const auto index = mUnresolvedOccurrence[i];
			if (!visible[shard] || file.Strings().Text(occ.name[index]) != spelling)
			{
				continue;
			}

			const auto& resolved = Resolved(shard);
			const auto targets   = resolved.Targets(index);
			if (targets.empty())
			{
				result.push_back({shard, occ.offset[index], occ.length[index], occ.role[index],
						resolved.state[index]});
				continue;
			}

			if (std::ranges::any_of(targets,
				[&](const SymbolRef& target)
				{
					return HashOf(target) == hash;
			}))
			{
				result.push_back({shard, occ.offset[index], occ.length[index], occ.role[index],
						resolved.state[index]});
			}
		}

		return result;
	}

	std::vector<RelatedClass> IndexSnapshot::Subtypes(SymbolHash hash) const
	{
		std::vector<RelatedClass> result;
		const auto declarations = Declarations(hash);
		const auto nameHash =
			declarations.empty() ? 0
		: HashBytes(mShards[declarations.front().shard]->Name(
			declarations.front().symbol));
		std::vector<SymbolHash> seen;
		for (std::uint32_t shard = 0; shard < mShards.size(); ++shard)
		{
			const auto& file = *mShards[shard];
			const auto& rel  = file.Relations();
			const auto collect =[&](SymbolHash subject, RelationKind kind, bool exact)
			{
				const auto[begin, end] = file.RelationsFrom(subject);
				for (auto i = begin; i < end; ++i)
				{
					if (rel.kind[i] != kind || std::ranges::find(seen, rel.object[i]) != seen.end())
					{
						continue;
					}

					bool exactHere = exact;
					if (kind == RelationKind::BaseOfByName && rel.offset[i] != kInvalid)
					{
						const auto occurrence = file.OccurrenceAt(rel.offset[i]);
						const auto targets =
							occurrence == kInvalid ? std::span<const SymbolRef> {}
						: Resolved(shard).Targets(occurrence);
						if (!targets.empty())
						{
							if (std::ranges::none_of(targets,
								[&](const SymbolRef& target)
								{
									return HashOf(target) == hash;
							}))
							{
								continue;
							}

							exactHere = true;
						}
					}

					for (const auto derived : Declarations(rel.object[i]))
					{
						if (mShards[derived.shard]->Symbols().role[derived.symbol] ==
							Role::Definition)
						{
							result.push_back({derived, exactHere});
							seen.push_back(rel.object[i]);
							break;
						}
					}
				}
			};

			collect(hash, RelationKind::BaseOf, true);
			if (!declarations.empty())
			{
				collect(nameHash, RelationKind::BaseOfByName, false);
			}
		}

		return result;
	}

	std::vector<RelatedClass> IndexSnapshot::Supertypes(SymbolHash derived) const
	{
		std::vector<RelatedClass> result;
		for (const auto& declaration : Declarations(derived))
		{
			if (mShards[declaration.shard]->Symbols().role[declaration.symbol] != Role::Definition)
			{
				continue;
			}

			const auto& file = *mShards[declaration.shard];
			const auto& rel  = file.Relations();
			for (const auto index : file.RelationsTo(derived))
			{
				if (rel.kind[index] == RelationKind::BaseOf)
				{
					for (const auto base : Declarations(rel.subject[index]))
					{
						if (mShards[base.shard]->Symbols().role[base.symbol] == Role::Definition)
						{
							result.push_back({base, true});
						}
					}
				}
				else if (const auto occurrence = rel.offset[index] == kInvalid
					? kInvalid
					: file.OccurrenceAt(rel.offset[index]);
					occurrence != kInvalid && !Resolved(declaration.shard).Targets(occurrence).empty())
				{
					for (const auto& target : Resolved(declaration.shard).Targets(occurrence))
					{
						for (const auto base : Definitions(HashOf(target)))
						{
							result.push_back({base, true});
						}
					}
				}
				else
				{
					// By name: any indexed class whose unqualified name hashes the same.
					for (std::uint32_t shard = 0; shard < mShards.size(); ++shard)
					{
						const auto& other = *mShards[shard];
						const auto& syms  = other.Symbols();
						for (std::uint32_t s = 0; s < syms.Size(); ++s)
						{
							if (syms.kind[s] == heimdall::SymbolKind::Class &&
								syms.role[s] == Role::Definition &&
								HashBytes(other.Name(s)) == rel.subject[index])
							{
								result.push_back({
										{
											shard, s
										}, false});
							}
						}
					}
				}
			}

			break;
		}

		return result;
	}

	std::vector<SymbolRef> IndexSnapshot::Search(std::string_view query, std::size_t limit) const
	{
		struct Hit
		{
			SymbolRef symbol;
			int score;
		};

		std::vector<Hit> hits;
		const auto queryMask = CharacterMask(query);
		for (std::uint32_t shard = 0; shard < mShards.size(); ++shard)
		{
			const auto& file    = *mShards[shard];
			const auto& symbols = file.Symbols();
			for (std::uint32_t s = 0; s < symbols.Size(); ++s)
			{
				if ((symbols.nameMask[s] & queryMask) != queryMask)
				{
					continue;
				}

				if (const auto score = Score(file.Name(s), query); score > 0)
				{
					hits.push_back({
							{
								shard, s
							}, score});
				}
			}
		}

		std::ranges::stable_sort(hits,
			[](const Hit& a, const Hit& b)
			{
				return a.score > b.score;
		});
		std::vector<SymbolRef> result;
		for (const auto& hit : hits | std::views::take(limit))
		{
			result.push_back(hit.symbol);
		}

		return result;
	}

	std::size_t IndexSnapshot::StorageBytes() const noexcept
	{
		std::size_t bytes = mDirHash.size() * (sizeof(SymbolHash) + 2 * sizeof(std::uint32_t)) +
			mUnresolvedName.size() * (sizeof(std::uint64_t) + 2 * sizeof(std::uint32_t)) +
			(mIncludersBegin.size() + mIncluders.size() + mByPath.size()) * sizeof(std::uint32_t);
		for (const auto& shard : mShards)
		{
			bytes += shard->StorageBytes();
		}

		return bytes;
	}
} // namespace heimdall::index
