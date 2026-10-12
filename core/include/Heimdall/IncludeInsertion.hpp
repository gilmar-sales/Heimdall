#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace heimdall
{

	/// Text to insert at `offset` to add one `#include` line.
	struct IncludeEdit
	{
		std::size_t offset = 0;
		std::string text;
	};

	/// Where `#include <spelled>` belongs in `source`, or nothing when the file already
	/// includes it. `spelled` keeps its delimiters (`"widget.hpp"`, `<vector>`). The line
	/// joins the top-level include block: angled includes before quoted ones, each group
	/// sorted case-insensitively. Without includes it goes after `#pragma once` or the
	/// include guard, else after the leading file comment.
	[[nodiscard]] std::optional<IncludeEdit> PlanIncludeInsertion(std::string_view source,
		std::string_view spelled);

} // namespace heimdall
