#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/FileShard.hpp>

namespace heimdall::index
{
	/** Turns one bound translation unit into an immutable FileShard. */
	class ShardBuilder
	{
	public:
		/** Resolve a literal include with the file's real search configuration. */
		using IncludeResolver = std::function<std::optional<std::string>(std::string_view spelling,
			bool quoted, std::uint32_t offset) >;

		/** Stable fingerprint of parser options and compilation/search configuration. */
		[[nodiscard]] static std::uint64_t ConfigurationHash(
			const ParserOptions& options,
			const CompileCommand* command = nullptr);

/**
		 * Indexes declarations at translation-unit, namespace and class scope plus every
		 * identifier use the binder recorded. Locals and parameters are not indexed.
		 */
		[[nodiscard]] static std::shared_ptr<const FileShard> Build(
			const SemanticModel& model,
			std::string path,
			std::uint64_t configHash = 0,
			IncludeResolver resolveInclude = {});
	};
} // namespace heimdall::index
