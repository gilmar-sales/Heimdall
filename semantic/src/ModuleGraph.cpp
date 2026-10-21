#include <Heimdall/ModuleGraph.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <queue>
#include <utility>

namespace heimdall
{

	namespace
	{

		constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

		struct Edge
		{
			/// Index into the unit's imports, or `kNoIndex` for the implicit interface import
			/// of an implementation unit.
			std::size_t importIndex = kNoIndex;
			std::size_t target      = kNoIndex;
		};

		// Providers of module names, looked up while the edges of every unit are built.
		class Resolver
		{
		public:
			using Units = std::map<std::string, ModuleUnitInfo, std::less<>>;

			explicit Resolver(const Units& units)
			{
				for (const auto& [file, unit] : units)
				{
					mFiles.push_back(file);
					mUnits.push_back(&unit);
					mIndexByFile.emplace(file, mFiles.size() - 1);
					switch (unit.kind)
					{
					case ModuleUnitKind::Interface:
						mInterfaces[unit.name].push_back(mFiles.size() - 1);
						break;
					case ModuleUnitKind::PartitionInterface:
					{
						// An interface partition is what `import :p;` means; an
						// implementation partition only answers when there is none.
						auto& providers = mPartitions[Key(unit.name, unit.partition)];
						providers.insert(providers.begin(), mFiles.size() - 1);
						break;
					}
					case ModuleUnitKind::PartitionImplementation:
						mPartitions[Key(unit.name, unit.partition)].push_back(mFiles.size() - 1);
						break;
					default:
						break;
					}
				}
			}

			[[nodiscard]] std::size_t Count() const noexcept
			{
				return mFiles.size();
			}

			[[nodiscard]] const std::string& File(std::size_t index) const
			{
				return mFiles[index];
			}

			[[nodiscard]] const ModuleUnitInfo& Unit(std::size_t index) const
			{
				return *mUnits[index];
			}

			[[nodiscard]] std::size_t IndexOf(std::string_view file) const
			{
				const auto found = mIndexByFile.find(std::string(file));
				return found == mIndexByFile.end() ? kNoIndex : found->second;
			}

			[[nodiscard]] const std::vector<std::size_t>* Interfaces(
				const std::string& module) const
			{
				const auto found = mInterfaces.find(module);
				return found == mInterfaces.end() ? nullptr : &found->second;
			}

			[[nodiscard]] std::size_t Interface(const std::string& module) const
			{
				const auto* all = Interfaces(module);
				return all == nullptr ? kNoIndex : all->front();
			}

			[[nodiscard]] std::size_t Partition(
				const std::string& module,
				const std::string& partition) const
			{
				const auto found = mPartitions.find(Key(module, partition));
				return found == mPartitions.end() ? kNoIndex : found->second.front();
			}

			// The unit that provides what `import` of `from` names, or `kNoIndex`.
			[[nodiscard]] std::size_t Provider(
				const ModuleUnitInfo& from,
				const ModuleImport& import) const
			{
				if (import.IsHeaderUnit())
				{
					return kNoIndex;
				}

				if (!import.module.empty())
				{
					return Interface(import.module);
				}

				return from.name.empty() ? kNoIndex : Partition(from.name, import.partition);
			}

			[[nodiscard]] std::vector<Edge> EdgesOf(std::size_t index) const
			{
				const auto& unit = Unit(index);
				std::vector<Edge> edges;
				if (unit.kind == ModuleUnitKind::Implementation)
				{
					if (const auto primary = Interface(unit.name); primary != kNoIndex)
					{
						edges.push_back({kNoIndex, primary});
					}
				}

				for (std::size_t i = 0; i < unit.imports.size(); ++i)
				{
					const auto target = Provider(unit, unit.imports[i]);
					if (target != kNoIndex && target != index)
					{
						edges.push_back({i, target});
					}
				}

				return edges;
			}

		private:
			static std::string Key(const std::string& module, const std::string& partition)
			{
				return module + ":" + partition;
			}

			std::vector<std::string> mFiles;
			std::vector<const ModuleUnitInfo*> mUnits;
			std::map<std::string, std::size_t> mIndexByFile;
			std::map<std::string, std::vector<std::size_t>> mInterfaces;
			std::map<std::string, std::vector<std::size_t>> mPartitions;
		};

		// Strongly connected components (Tarjan), iterative so that a long chain of imports
		// cannot exhaust the stack. Components come out dependencies first.
		std::vector<std::vector<std::size_t>> Components(
			const std::vector<std::vector<Edge>>& edges)
		{
			const auto count = edges.size();
			std::vector<std::size_t> index(count, kNoIndex);
			std::vector<std::size_t> low(count, 0);
			std::vector<bool> onStack(count, false);
			std::vector<std::size_t> stack;
			std::vector<std::vector<std::size_t>> components;
			std::size_t counter = 0;

			struct Frame
			{
				std::size_t node;
				std::size_t next;
			};

			for (std::size_t root = 0; root < count; ++root)
			{
				if (index[root] != kNoIndex)
				{
					continue;
				}

				std::vector<Frame> frames {
					{
						root, 0
				}};
				index[root] = low[root] = counter++;
				stack.push_back(root);
				onStack[root] = true;
				while (!frames.empty())
				{
					auto& frame = frames.back();
					if (frame.next < edges[frame.node].size())
					{
						const auto target = edges[frame.node][frame.next++].target;
						if (index[target] == kNoIndex)
						{
							index[target] = low[target] = counter++;
							stack.push_back(target);
							onStack[target] = true;
							frames.push_back({target, 0});
						}
						else if (onStack[target])
						{
							low[frame.node] = std::min(low[frame.node], index[target]);
						}

						continue;
					}

					const auto node = frame.node;
					frames.pop_back();
					if (!frames.empty())
					{
						low[frames.back().node] = std::min(low[frames.back().node], low[node]);
					}

					if (low[node] != index[node])
					{
						continue;
					}

					std::vector<std::size_t> component;
					std::size_t member = kNoIndex;
					do
					{
						member = stack.back();
						stack.pop_back();
						onStack[member] = false;
						component.push_back(member);
					} while (member != node);

					components.push_back(std::move(component));
				}
			}

			return components;
		}

		bool Contains(const std::vector<std::string>& values, std::string_view value)
		{
			return std::ranges::find(values, value) != values.end();
		}

	} // namespace

	void ModuleGraph::Set(std::string file, ModuleUnitInfo unit)
	{
		mUnits.insert_or_assign(std::move(file), std::move(unit));
	}

	void ModuleGraph::Remove(std::string_view file)
	{
		if (const auto found = mUnits.find(file); found != mUnits.end())
		{
			mUnits.erase(found);
		}
	}

	std::string ModuleGraph::InterfaceOf(std::string_view module) const
	{
		const Resolver resolver(mUnits);
		const auto found = resolver.Interface(std::string(module));
		return found == kNoIndex ? std::string() : resolver.File(found);
	}

	std::string ModuleGraph::PartitionOf(
		std::string_view module,
		std::string_view partition) const
	{
		const Resolver resolver(mUnits);
		const auto found = resolver.Partition(std::string(module), std::string(partition));
		return found == kNoIndex ? std::string() : resolver.File(found);
	}

	std::vector<std::string> ModuleGraph::ImplementationsOf(std::string_view module) const
	{
		std::vector<std::string> files;
		for (const auto& [file, unit] : mUnits)
		{
			if (unit.kind == ModuleUnitKind::Implementation && unit.name == module)
			{
				files.push_back(file);
			}
		}

		return files;
	}

	std::vector<std::string> ModuleGraph::Dependencies(std::string_view file) const
	{
		const Resolver resolver(mUnits);
		const auto index = resolver.IndexOf(file);
		std::vector<std::string> files;
		if (index == kNoIndex)
		{
			return files;
		}

		for (const auto& edge : resolver.EdgesOf(index))
		{
			files.push_back(resolver.File(edge.target));
		}

		std::ranges::sort(files);
		files.erase(std::ranges::unique(files).begin(), files.end());
		return files;
	}

	ModuleBuildOrder ModuleGraph::BuildOrder() const
	{
		const Resolver resolver(mUnits);
		const auto count = resolver.Count();
		std::vector<std::vector<std::size_t>> dependents(count);
		std::vector<std::size_t> waiting(count, 0);
		for (std::size_t node = 0; node < count; ++node)
		{
			std::vector<std::size_t> targets;
			for (const auto& edge : resolver.EdgesOf(node))
			{
				targets.push_back(edge.target);
			}

			std::ranges::sort(targets);
			targets.erase(std::ranges::unique(targets).begin(), targets.end());
			waiting[node] = targets.size();
			for (const auto target : targets)
			{
				dependents[target].push_back(node);
			}
		}

		// Files are indexed in path order, so a min-heap on the index keeps ties stable.
		std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
		for (std::size_t node = 0; node < count; ++node)
		{
			if (waiting[node] == 0)
			{
				ready.push(node);
			}
		}

		ModuleBuildOrder result;
		std::vector<bool> placed(count, false);
		while (!ready.empty())
		{
			const auto node = ready.top();
			ready.pop();
			placed[node] = true;
			result.order.push_back(resolver.File(node));
			for (const auto dependent : dependents[node])
			{
				if (--waiting[dependent] == 0)
				{
					ready.push(dependent);
				}
			}
		}

		for (std::size_t node = 0; node < count; ++node)
		{
			if (!placed[node])
			{
				result.cyclic.push_back(resolver.File(node));
			}
		}

		return result;
	}

	std::vector<ModuleIssue> ModuleGraph::Check(const ModuleCheckOptions& options) const
	{
		const Resolver resolver(mUnits);
		const auto count = resolver.Count();
		std::vector<ModuleIssue> issues;

		for (std::size_t node = 0; node < count; ++node)
		{
			const auto& unit = resolver.Unit(node);
			if (unit.kind != ModuleUnitKind::Interface)
			{
				continue;
			}

			const auto first = resolver.Interface(unit.name);
			if (first != node)
			{
				issues.push_back({ModuleIssueKind::DuplicateInterface, resolver.File(node),
						unit.nameOffset, unit.nameLength,
						"module '" + unit.name + "' already has a primary interface unit in " +
						resolver.File(first)});
			}
		}

		for (std::size_t node = 0; node < count; ++node)
		{
			const auto& unit = resolver.Unit(node);
			if (unit.kind != ModuleUnitKind::PartitionInterface)
			{
				continue;
			}

			const auto primary = resolver.Interface(unit.name);
			if (primary == kNoIndex)
			{
				continue;
			}

			const auto& exports = resolver.Unit(primary).imports;
			const bool exported = std::ranges::any_of(exports,
				[&unit](const ModuleImport& import)
				{
					return import.exported && import.module.empty() && !import.IsHeaderUnit() &&
					import.partition == unit.partition;
				});
			if (!exported)
			{
				issues.push_back({ModuleIssueKind::PartitionNotExported, resolver.File(node),
						unit.nameOffset, unit.nameLength,
						"interface partition '" + unit.QualifiedName() +
						"' is not exported by the primary interface of '" + unit.name + "'"});
			}
		}

		std::vector<std::vector<Edge>> edges(count);
		for (std::size_t node = 0; node < count; ++node)
		{
			edges[node] = resolver.EdgesOf(node);
		}

		for (const auto& component : Components(edges))
		{
			if (component.size() < 2)
			{
				continue;
			}

			const auto inside = [&component](std::size_t node)
			{
				return std::ranges::find(component, node) != component.end();
			};
			for (const auto node : component)
			{
				const auto& unit = resolver.Unit(node);
				for (const auto& edge : edges[node])
				{
					if (edge.importIndex == kNoIndex ||!inside(edge.target))
					{
						continue;
					}

					const auto& import = unit.imports[edge.importIndex];
					issues.push_back({ModuleIssueKind::ImportCycle, resolver.File(node),
							import.offset, import.length,
							"import of '" + unit.ImportTarget(import) +
							"' creates a cycle through " + resolver.File(edge.target)});
				}
			}
		}

		if (options.reportUnresolved)
		{
			for (std::size_t node = 0; node < count; ++node)
			{
				const auto& unit = resolver.Unit(node);
				for (const auto& import : unit.imports)
				{
					if (import.IsHeaderUnit() || resolver.Provider(unit, import) != kNoIndex)
					{
						continue;
					}

					const auto target = unit.ImportTarget(import);
					if (target.empty() || Contains(options.externalModules, target))
					{
						continue;
					}

					issues.push_back({ModuleIssueKind::UnresolvedImport, resolver.File(node),
							import.offset, import.length,
							"module '" + target + "' is not provided by any known file"});
				}
			}
		}

		std::ranges::stable_sort(issues,
			[](const ModuleIssue& a, const ModuleIssue& b)
			{
				return a.file != b.file ? a.file < b.file : a.offset < b.offset;
			});
		return issues;
	}

} // namespace heimdall
