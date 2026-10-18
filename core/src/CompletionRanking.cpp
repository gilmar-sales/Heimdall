#include <Heimdall/CompletionRanking.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace heimdall
{
	namespace
	{
		constexpr std::size_t MAX_FUZZY_NAME   = 256;
		constexpr float EXACT_NAME_SCORE       = 1.2f;
		constexpr float FOLDED_PREFIX_SCORE    = 0.9f;
		constexpr float FOLDED_EXACT_SCORE     = 1.05f;
		constexpr float FUZZY_BASE_SCORE       = 0.4f;
		constexpr float FUZZY_LENGTH_WEIGHT    = 0.2f;
		constexpr float UNAVAILABLE_PENALTY    = 0.1f;
		constexpr float RESERVED_PENALTY       = 0.1f;
		constexpr float FUNCTION_BOOST         = 4;
		constexpr float CLASS_BOOST            = 2;
		constexpr float FILE_BOOST             = 1.5f;
		constexpr float MACRO_PENALTY          = 0.5f;
		constexpr float INCLUDE_PENALTY        = 0.7f;
		constexpr float INHERITED_PENALTY      = 0.5f;
		constexpr float IMPLEMENTATION_PENALTY = 0.2f;
		constexpr float QUALIFICATION_PENALTY  = 0.8f;
		constexpr float PREFERRED_TYPE_BOOST   = 5;
		constexpr float NON_INSTANCE_PENALTY   = 0.2f;
		constexpr float ENTITY_BOOST           = 1.1f;
		constexpr float NAMESPACE_PENALTY      = 0.8f;
		constexpr float OPERATOR_PENALTY       = 0.5f;
		constexpr float TYPE_ROLE_BOOST        = 4;
		constexpr float OTHER_ROLE_PENALTY     = 0.5f;
		constexpr float MAX_REFERENCE_BOOST    = 3;
		constexpr float REFERENCE_WEIGHT       = 0.35f;
		constexpr float FUZZY_PATH_WEIGHT      = 0.2f;
		constexpr float CONTIGUOUS_REWARD      = 0.4f;
		constexpr float SEGMENT_REWARD         = 0.2f;
		constexpr float CASE_REWARD            = 0.05f;
		constexpr float GAP_PENALTY            = 0.01f;
		constexpr float OVERRIDE_BOOST         = 2;

		float ReferenceBoost(std::uint32_t references) noexcept
		{
			return references < 2 ? 1 :
			1 + std::min(MAX_REFERENCE_BOOST, REFERENCE_WEIGHT * std::log2(static_cast<float>(references)));
		}

		float RoleBoost(const CompletionItem& item) noexcept
		{
			switch (item.completionRole)
			{
			case CompletionRole::Type:
				return item.kind == CompletionKind::Type ? TYPE_ROLE_BOOST :
				item.kind == CompletionKind::Namespace ? FILE_BOOST : OTHER_ROLE_PENALTY;
			case CompletionRole::Namespace:
				return item.kind == CompletionKind::Namespace ? TYPE_ROLE_BOOST : OTHER_ROLE_PENALTY;
			case CompletionRole::Preprocessor:
				return item.kind == CompletionKind::Directive ? TYPE_ROLE_BOOST :
				item.kind == CompletionKind::Macro ? CLASS_BOOST : OTHER_ROLE_PENALTY;
			case CompletionRole::Expression:
				return item.kind == CompletionKind::Type || item.kind == CompletionKind::Namespace ?
					NAMESPACE_PENALTY : 1;
			case CompletionRole::Member:
			case CompletionRole::Any:
				return 1;
			}

			return 1;
		}

		constexpr char FoldCase(char value) noexcept
		{
			return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
		}

		bool IsSegmentStart(std::string_view name, std::size_t at) noexcept
		{
			if (at == 0)
			{
				return true;
			}

			const char previous = name[at - 1];
			const char current  = name[at];
			const bool acronymBoundary = current >= 'A' && current <= 'Z' &&
				previous >= 'A' && previous <= 'Z' && at + 1 < name.size() &&
				name[at + 1] >= 'a' && name[at + 1] <= 'z';
			return (previous == '_' && current != '_') ||
				(current >= 'A' && current <= 'Z' && previous >= 'a' && previous <= 'z') ||
				acronymBoundary ||
				(!(current >= '0' && current <= '9') && previous >= '0' && previous <= '9');
		}
	}

	float CompletionRanking::MatchScore(
		std::string_view name,
		std::string_view prefix,
		bool macro) noexcept
	{
		if (name.empty())
		{
			return 0;
		}

		if (prefix.empty())
		{
			return 1;
		}

		if (name.starts_with(prefix))
		{
			return name == prefix ? EXACT_NAME_SCORE : 1.0f;
		}

		if (macro || name.size() < prefix.size())
		{
			return 0;
		}

		bool folded = true;
		for (std::size_t index = 0; index < prefix.size(); ++index)
		{
			folded = folded && FoldCase(name[index]) == FoldCase(prefix[index]);
		}

		if (folded)
		{
			return name.size() == prefix.size() ? FOLDED_EXACT_SCORE : FOLDED_PREFIX_SCORE;
		}

		if (name.size() > MAX_FUZZY_NAME)
		{
			return 0;
		}

		constexpr float NO_MATCH = -std::numeric_limits<float>::infinity();
		std::array<float, MAX_FUZZY_NAME> matched;
		std::array<float, MAX_FUZZY_NAME> next;
		for (std::size_t index = 0; index < name.size(); ++index)
		{
			matched[index] = FoldCase(name[index]) == FoldCase(prefix.front()) &&
				IsSegmentStart(name, index) ? (index == 0 ? SEGMENT_REWARD : 0) +
				(name[index] == prefix.front() ? CASE_REWARD : 0) - static_cast<float>(index) * GAP_PENALTY : NO_MATCH;
		}

		for (std::size_t query = 1; query < prefix.size(); ++query)
		{
			float bestBefore = NO_MATCH;
			float best       = NO_MATCH;
			for (std::size_t index = 0; index < name.size(); ++index)
			{
				next[index] = NO_MATCH;
				if (FoldCase(name[index]) == FoldCase(prefix[query]) && index > 0)
				{
					float path = matched[index - 1] + CONTIGUOUS_REWARD;
					if (IsSegmentStart(name, index))
					{
						path = std::max(path, bestBefore - static_cast<float>(index) * GAP_PENALTY + SEGMENT_REWARD);
					}

					next[index] = path +(name[index] == prefix[query] ? CASE_REWARD : 0);
					best        = std::max(best, next[index]);
				}

				bestBefore = std::max(bestBefore, matched[index] + static_cast<float>(index) * GAP_PENALTY);
			}

			if (best == NO_MATCH)
			{
				return 0;
			}

			std::copy_n(next.begin(), name.size(), matched.begin());
		}

		float best = NO_MATCH;
		for (std::size_t index = 0; index < name.size(); ++index)
		{
			best = std::max(best, matched[index]);
		}

		if (best == NO_MATCH)
		{
			return 0;
		}

		const float ideal = SEGMENT_REWARD + static_cast<float>(prefix.size()) * (CONTIGUOUS_REWARD + CASE_REWARD);
		return FUZZY_BASE_SCORE + FUZZY_LENGTH_WEIGHT * static_cast<float>(prefix.size()) /
			static_cast<float>(name.size()) + FUZZY_PATH_WEIGHT * std::clamp(best / ideal, 0.0f, 1.0f);
	}

	float CompletionRanking::Evaluate(
		const CompletionItem& item,
		std::string_view prefix,
		bool needsInclude) noexcept
	{
		return Explain(item, prefix, needsInclude).total;
	}

	CompletionScore CompletionRanking::Explain(
		const CompletionItem& item,
		std::string_view prefix,
		bool needsInclude) noexcept
	{
		const std::string_view name = item.filter_text.empty() ? std::string_view(item.label) :
		std::string_view(item.filter_text);
		CompletionScore result;
		result.name = MatchScore(name, prefix, item.kind == CompletionKind::Macro);
		if (item.deleted)
		{
			result.relevance = 0;
			return result;
		}

		if (result.name == 0)
		{
			return result;
		}

		float& quality   = result.quality;
		float& relevance = result.relevance;
		if (item.deprecated)
		{
			quality *= UNAVAILABLE_PENALTY;
		}

		if (!prefix.starts_with('_') && (name.starts_with("__") ||
			(name.size() > 1 && name.front() == '_' && name[1] >= 'A' && name[1] <= 'Z')))
		{
			quality *= RESERVED_PENALTY;
		}

		if (item.implementationDetail)
		{
			quality *= IMPLEMENTATION_PENALTY;
		}

		switch (item.completionScope)
		{
		case CompletionScope::Function:
			relevance *= FUNCTION_BOOST;
			break;
		case CompletionScope::Class:
			relevance *= CLASS_BOOST;
			break;
		case CompletionScope::File:
			relevance *= FILE_BOOST;
			break;
		case CompletionScope::Unknown:
			break;
		}

		if (item.kind == CompletionKind::Macro)
		{
			quality *= MACRO_PENALTY;
		}
		else if (item.kind == CompletionKind::Namespace)
		{
			quality *= NAMESPACE_PENALTY;
		}
		else if (item.kind == CompletionKind::Type || item.kind == CompletionKind::Variable ||
			item.kind == CompletionKind::Function)
		{
			quality *= ENTITY_BOOST;
		}

		if (name.starts_with('~') ||(name.starts_with("operator") && name.size() > 8 &&
			!(name[8] == '_' ||(name[8] >= 'a' && name[8] <= 'z') ||
			(name[8] >= 'A' && name[8] <= 'Z'))))
		{
			quality *= OPERATOR_PENALTY;
		}

		if (needsInclude)
		{
			relevance *= INCLUDE_PENALTY;
		}

		if (item.inheritedCompletion)
		{
			relevance *= INHERITED_PENALTY;
		}

		if (item.nonInstanceCompletion)
		{
			relevance *= NON_INSTANCE_PENALTY;
		}

		if (item.preferredTypeMatch)
		{
			relevance *= PREFERRED_TYPE_BOOST;
		}

		if (item.needsQualification)
		{
			relevance *= QUALIFICATION_PENALTY;
		}

		if (item.contextWordMatch)
		{
			relevance *= FILE_BOOST;
		}

		if (item.kind == CompletionKind::Function && !item.filter_text.empty() && item.label.ends_with(" override"))
		{
			relevance *= OVERRIDE_BOOST;
		}

		if (item.scopeDistance >= 0)
		{
			relevance *= 1 + 1 /(1 + static_cast<float>(item.scopeDistance));
		}

		if (item.fileDistance >= 0)
		{
			relevance *= 1 + 1 /(1 + static_cast<float>(item.fileDistance));
		}

		relevance *= RoleBoost(item) * ReferenceBoost(item.mainFileReferences) * ReferenceBoost(item.scopeReferences);
		result.total = result.name * quality * relevance;
		return result;
	}

	void CompletionRanking::Rank(std::vector<CompletionItem>& items, std::string_view prefix)
	{
		struct RankedItem
		{
			float score;
			std::size_t index;
		};

		std::vector<RankedItem> order;
		order.reserve(items.size());
		for (std::size_t index = 0; index < items.size(); ++index)
		{
			auto& item = items[index];
			if (item.kind == CompletionKind::Namespace && !item.detail.starts_with("namespace"))
			{
				item.kind = CompletionKind::Type;
			}

			const float score = Evaluate(item, prefix);
			if (score > 0)
			{
				order.push_back({score, index});
			}
		}

		std::sort(order.begin(), order.end(),[&](const auto& left, const auto& right)
			{
				if (left.score != right.score)
				{
					return left.score > right.score;
				}

				const auto& a = items[left.index];
				const auto& b = items[right.index];
				if (a.label != b.label)
				{
					return a.label < b.label;
				}

				if (a.detail != b.detail)
				{
					return a.detail < b.detail;
				}

				if (a.kind != b.kind)
				{
					return a.kind < b.kind;
				}

				if (a.file != b.file)
				{
					return a.file < b.file;
				}

				if (a.offset != b.offset)
				{
					return a.offset < b.offset;
				}

				return left.index < right.index;
			});
		std::vector<CompletionItem> ranked;
		ranked.reserve(order.size());
		for (const auto& entry : order)
		{
			ranked.push_back(std::move(items[entry.index]));
		}

		items = std::move(ranked);
	}
}
