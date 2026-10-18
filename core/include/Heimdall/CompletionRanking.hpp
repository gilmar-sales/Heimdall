#pragma once

#include <string_view>
#include <vector>

#include <Heimdall/Completion.hpp>

namespace heimdall
{
	/** @brief Independent factors for explaining the heuristic score without allocation. */
	struct CompletionScore
	{
		float name      = 0;
		float quality   = 1;
		float relevance = 1;
		float total     = 0;
	};

	/** @brief Shared, deterministic ranking for local, macro and workspace completions. */
	class CompletionRanking
	{
	public:
		/// Returns zero for a non-match; macros require a case-sensitive prefix.
		[[nodiscard]] static float MatchScore(
			std::string_view name,
			std::string_view prefix,
			bool macro = false) noexcept;

		/// Combines name matching with known symbol quality and visibility signals.
		[[nodiscard]] static float Evaluate(
			const CompletionItem& item,
			std::string_view prefix,
			bool needsInclude = false) noexcept;

		/// Explains name, category/availability quality and contextual relevance separately.
		[[nodiscard]] static CompletionScore Explain(
			const CompletionItem& item,
			std::string_view prefix,
			bool needsInclude = false) noexcept;

		/// Normalizes, filters and ranks items with deterministic name and detail tie breaks.
		static void Rank(std::vector<CompletionItem>& items, std::string_view prefix);
	};
}
