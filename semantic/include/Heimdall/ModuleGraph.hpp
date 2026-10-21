#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <Heimdall/ModuleUnit.hpp>

namespace heimdall
{

	/// A cross-file problem of the module graph.
	enum class ModuleIssueKind : std::uint8_t
	{
		/// An import names a module no known file provides.
		UnresolvedImport,
		/// A chain of imports leads back to the importing unit.
		ImportCycle,
		/// Two files declare the primary interface of the same module.
		DuplicateInterface,
		/// An interface partition is not exported by the primary interface of its module.
		PartitionNotExported
	};

	/// One problem, anchored at a span of `file`.
	struct ModuleIssue
	{
		ModuleIssueKind kind = ModuleIssueKind::UnresolvedImport;
		std::string file;
		std::uint32_t offset = 0;
		std::uint32_t length = 0;
		std::string message;
	};

	struct ModuleCheckOptions
	{
		/// Report imports of modules that no known file provides. Off by default: it is only
		/// meaningful when every file of the project was given.
		bool reportUnresolved = false;
		/// Modules provided by the toolchain, never reported as unresolved.
		std::vector<std::string> externalModules = {"std", "std.compat"};
	};

	/// Units in the order a build must compile them; `cyclic` lists the units that could not
	/// be ordered because they take part in, or depend on, an import cycle.
	struct ModuleBuildOrder
	{
		std::vector<std::string> order;
		std::vector<std::string> cyclic;
	};

	/// Module units of a project and the imports between them (STL only).
	///
	/// A unit is identified by the path of its file. Lookups scan the units, so they cost
	/// O(files); the graph is meant for project-sized sets, not for per-keystroke queries.
	class ModuleGraph
	{
	public:
		/// Registers `file`, replacing what it declared before. Files that only import
		/// modules (plain translation units) are tracked too, so they appear in the order.
		void Set(std::string file, ModuleUnitInfo unit);

		void Remove(std::string_view file);

		[[nodiscard]] std::size_t Size() const noexcept
		{
			return mUnits.size();
		}

		/// File of the primary interface unit of `module`, or empty. With duplicates the
		/// first file in path order wins.
		[[nodiscard]] std::string InterfaceOf(std::string_view module) const;

		/// File of the interface partition `module:partition`, or empty.
		[[nodiscard]] std::string PartitionOf(
			std::string_view module,
			std::string_view partition) const;

		/// Files of the implementation units (`module m;`) of `module`, in path order.
		[[nodiscard]] std::vector<std::string> ImplementationsOf(std::string_view module) const;

		/// Files that must be compiled before `file`: the units providing its imports, and
		/// the primary interface for an implementation unit. Sorted and without repeats.
		[[nodiscard]] std::vector<std::string> Dependencies(std::string_view file) const;

		/// Topological order of every known file, dependencies first, ties in path order.
		[[nodiscard]] ModuleBuildOrder BuildOrder() const;

		/// Problems that span files. Problems inside one file are `ModuleUnitInfo::problems`.
		[[nodiscard]] std::vector<ModuleIssue> Check(const ModuleCheckOptions& options = {}) const;

	private:
		std::map<std::string, ModuleUnitInfo, std::less<>> mUnits;
	};

} // namespace heimdall
