#include <Heimdall/CompletionRanking.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace heimdall
{
	namespace
	{
		constexpr std::size_t MAX_FUZZY_NAME = 256;
		constexpr float EXACT_NAME_SCORE     = 1.2f;
		constexpr float FOLDED_PREFIX_SCORE  = 0.9f;
		constexpr float FUZZY_BASE_SCORE     = 0.4f;
		constexpr float FUZZY_LENGTH_WEIGHT  = 0.2f;
		constexpr float UNAVAILABLE_PENALTY  = 0.1f;
		constexpr float RESERVED_PENALTY     = 0.1f;
		constexpr float FUNCTION_BOOST       = 4;
		constexpr float CLASS_BOOST          = 2;
		constexpr float FILE_BOOST           = 1.5f;
		constexpr float MACRO_PENALTY        = 0.5f;
		constexpr float INCLUDE_PENALTY      = 0.7f;
		constexpr float INHERITED_PENALTY    = 0.5f;

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
			return FOLDED_PREFIX_SCORE;
		}

		if (name.size() > MAX_FUZZY_NAME)
		{
			return 0;
		}

		std::array<bool, MAX_FUZZY_NAME> matched {};
		std::array<bool, MAX_FUZZY_NAME> next {};
		for (std::size_t index = 0; index < name.size(); ++index)
		{
			matched[index] = FoldCase(name[index]) == FoldCase(prefix.front()) &&
				IsSegmentStart(name, index);
		}

		for (std::size_t query = 1; query < prefix.size(); ++query)
		{
			bool anyBefore = false;
			for (std::size_t index = 0; index < name.size(); ++index)
			{
				next[index] = FoldCase(name[index]) == FoldCase(prefix[query]) && index > 0 &&
					(matched[index - 1] ||(anyBefore && IsSegmentStart(name, index)));
				anyBefore = anyBefore || matched[index];
			}

			matched = next;
		}

		for (std::size_t index = 0; index < name.size(); ++index)
		{
			if (matched[index])
			{
				return FUZZY_BASE_SCORE + FUZZY_LENGTH_WEIGHT * static_cast<float>(prefix.size()) /
					static_cast<float>(name.size());
			}
		}

		return 0;
	}

	float CompletionRanking::Evaluate(
		const CompletionItem& item,
		std::string_view prefix,
		bool needsInclude) noexcept
	{
		const std::string_view name = item.filter_text.empty() ? std::string_view(item.label) :
		std::string_view(item.filter_text);
		float score = MatchScore(name, prefix, item.kind == CompletionKind::Macro);
		if (item.deprecated || item.deleted)
		{
			score *= UNAVAILABLE_PENALTY;
		}

		if (!prefix.starts_with('_') && (name.starts_with("__") ||
			(name.size() > 1 && name.front() == '_' && name[1] >= 'A' && name[1] <= 'Z')))
		{
			score *= RESERVED_PENALTY;
		}

		switch (item.completionScope)
		{
		case CompletionScope::Function:
			score *= FUNCTION_BOOST;
			break;
		case CompletionScope::Class:
			score *= CLASS_BOOST;
			break;
		case CompletionScope::File:
			score *= FILE_BOOST;
			break;
		case CompletionScope::Unknown:
			break;
		}

		if (item.kind == CompletionKind::Macro)
		{
			score *= MACRO_PENALTY;
		}

		if (needsInclude)
		{
			score *= INCLUDE_PENALTY;
		}

		if (item.inheritedCompletion)
		{
			score *= INHERITED_PENALTY;
		}

		return score;
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
