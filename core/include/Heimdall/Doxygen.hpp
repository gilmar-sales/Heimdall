#pragma once

#include <cstddef>
#include <Heimdall/ParseTree.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{
	/** Explicit symbol reference in a Doxygen comment. */
	struct DoxygenReference
	{
		std::size_t offset     = 0;
		std::size_t length     = 0;
		std::size_t nameOffset = 0;
		std::size_t nameLength = 0;
		std::size_t commentEnd = 0;
		std::string target;
		bool parameter = false;
	};

	/** Extracts symbol-bearing commands, excluding labels and code/verbatim blocks. */
	[[nodiscard]] std::vector<DoxygenReference> DoxygenReferences(const ParseTree& tree);

	/** Renders cleaned documentation text as Markdown without raw Doxygen section tags. */
	[[nodiscard]] std::string RenderDoxygen(std::string_view text);
}
