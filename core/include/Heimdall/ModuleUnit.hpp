#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Heimdall/ParseTree.hpp>

namespace heimdall
{

	/// Role of a translation unit in the C++20 module system ([module.unit]).
	enum class ModuleUnitKind : std::uint8_t
	{
		/// Not a module unit (no module declaration).
		None,
		/// `export module a.b;`
		Interface,
		/// `module a.b;`
		Implementation,
		/// `export module a.b:part;`
		PartitionInterface,
		/// `module a.b:part;`
		PartitionImplementation
	};

	/// One `import` declaration, as written in a module unit.
	struct ModuleImport
	{
		/// Imported module name (`app.util`); empty for `import :part;` and header units.
		std::string module;
		/// Partition name without the colon (`detail` for `import :detail;`).
		std::string partition;
		/// Header unit with its delimiters (`<vector>` or `"local.h"`); empty for modules.
		std::string header;
		/// `export import ...;`, also inside an `export { ... }` block.
		bool exported = false;
		/// Span of the imported name or header name.
		std::uint32_t offset = 0;
		std::uint32_t length = 0;

		/// Whether this imports a header unit rather than a named module.
		[[nodiscard]] bool IsHeaderUnit() const noexcept
		{
			return !header.empty();
		}
	};

	/// A structural violation of the module-unit rules that needs no other file to detect.
	struct ModuleProblem
	{
		std::uint32_t offset = 0;
		std::uint32_t length = 0;
		std::string message;
	};

	/// Module-related facts of one translation unit, extracted from its parse tree.
	struct ModuleUnitInfo
	{
		ModuleUnitKind kind = ModuleUnitKind::None;
		/// Module name without the partition (`app.core`).
		std::string name;
		/// Partition name without the colon (`part`); empty when not a partition.
		std::string partition;
		/// `module;` introduces a global module fragment.
		bool hasGlobalFragment = false;
		/// `module :private;` introduces a private module fragment.
		bool hasPrivateFragment = false;
		/// Span of the declared module name including the partition.
		std::uint32_t nameOffset = 0;
		std::uint32_t nameLength = 0;
		std::vector<ModuleImport> imports;
		std::vector<ModuleProblem> problems;

		/// Whether the file contains a module declaration.
		[[nodiscard]] bool IsModuleUnit() const noexcept
		{
			return kind != ModuleUnitKind::None;
		}

		/// Whether the unit contributes exported declarations to the module interface.
		[[nodiscard]] bool IsInterface() const noexcept
		{
			return kind == ModuleUnitKind::Interface || kind == ModuleUnitKind::PartitionInterface;
		}

		/// `app.core:part` for a partition, `app.core` otherwise; empty for non-modules.
		[[nodiscard]] std::string QualifiedName() const
		{
			return partition.empty() ? name : name + ":" + partition;
		}

		/// The import whose name or header name covers `offset` (end included), if any.
		[[nodiscard]] const ModuleImport* ImportAt(std::size_t offset) const noexcept
		{
			for (const auto& import : imports)
			{
				if (offset >= import.offset && offset <= import.offset + import.length)
				{
					return &import;
				}
			}

			return nullptr;
		}

		/// Qualified name of the unit that provides `import`: `app.util` for `import
		/// app.util;`, `app.core:detail` for `import :detail;` inside module `app.core`.
		/// Empty for header units and for a partition import outside a module unit.
		[[nodiscard]] std::string ImportTarget(const ModuleImport& import) const
		{
			if (import.IsHeaderUnit())
			{
				return {};
			}

			if (!import.module.empty())
			{
				return import.module;
			}

			return name.empty() ? std::string() : name + ":" + import.partition;
		}
	};

	/// Whether a file extension (with its dot) is conventionally used for module interface
	/// units: `.cppm` (Clang), `.ixx` (MSVC), `.mpp`, `.ccm`, `.cxxm` and `.c++m`.
	[[nodiscard]] constexpr bool IsModuleUnitExtension(std::string_view extension) noexcept
	{
		return extension == ".cppm" || extension == ".ixx" || extension == ".mpp" ||
			extension == ".ccm" || extension == ".cxxm" || extension == ".c++m";
	}

	/// Extracts the module declaration, the imports and the structural problems of a
	/// parsed translation unit. A file without module syntax yields `ModuleUnitKind::None`.
	[[nodiscard]] ModuleUnitInfo ExtractModuleUnit(const ParseTree& tree);

} // namespace heimdall
