#pragma once

#include <Heimdall/FileShard.hpp>

#include <memory>
#include <string>

namespace heimdall::index
{
	/** Turns one bound translation unit into an immutable FileShard. */
	class ShardBuilder
	{
	public:
		/**
		 * Indexes declarations at translation-unit, namespace and class scope plus every
		 * identifier use the binder recorded. Locals and parameters are not indexed.
		 */
		[[nodiscard]] static std::shared_ptr<const FileShard> Build(
			const SemanticModel& model, std::string path, std::uint64_t configHash = 0);
	};
} // namespace heimdall::index
