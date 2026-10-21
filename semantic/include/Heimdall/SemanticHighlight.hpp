#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Heimdall/SemanticModel.hpp>
#include <Heimdall/SyntaxHighlight.hpp>

namespace heimdall
{
	/// Semantic identity retained independently of a header's parse tree lifetime.
	struct HighlightSymbol
	{
		HighlightKind kind      = HighlightKind::Type;
		std::uint32_t modifiers = 0;
		std::string aliasTarget;
		bool operator==(const HighlightSymbol&) const = default;
	};

	/// Immutable after publication; qualified names from distinct headers remain distinct.
	class SemanticHighlightIndex
	{
	public:
		/// Collect declarations addressable through named namespace and record scopes.
		[[nodiscard]] static SemanticHighlightIndex FromModel(const SemanticModel& model);

		/// Merge header contributions; conflicting declarations become unavailable.
		void Merge(const SemanticHighlightIndex& other);

		/// Find an unambiguous qualified declaration, following alias qualifiers.
		[[nodiscard]] const HighlightSymbol* Find(std::string_view qualifiedName) const;

	private:
		std::unordered_map<std::string, std::optional<HighlightSymbol>> mSymbols;
	};

	/// Enrich lexical spans using local symbol identities and an optional header snapshot.
	class SemanticHighlight
	{
	public:
		/// The model owns no source: its tree and header snapshot must outlive this call.
		[[nodiscard]] static std::vector<HighlightSpan> Extract(
			const SemanticModel& model,
			std::span<const HighlightSpan> lexical,
			const SemanticHighlightIndex* headers = nullptr);
	};
}
