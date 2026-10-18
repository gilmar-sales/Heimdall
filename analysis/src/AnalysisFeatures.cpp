#include <Heimdall/AnalysisFeatures.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <utility>

#include <Heimdall/CompletionRanking.hpp>
#include <Heimdall/SemanticRuleDispatch.hpp>

namespace heimdall
{
	namespace
	{
		void ApplyCompletionUses(
			const AnalysisContext& context,
			std::size_t offset,
			std::vector<CompletionItem>& items)
		{
			const auto model = context.Snapshot().TrySemantic(context.Document());
			if (!model || items.empty())
			{
				return;
			}

			const auto& symbols = model->Symbols();
			const auto& scopes  = model->Scopes();
			const auto& refs    = model->Refs();
			const auto& tokens  = context.Syntax().Tokens();
			std::vector<SymbolId> groups(symbols.Size());
			std::iota(groups.begin(), groups.end(), SymbolId {});
			const auto root = [&](SymbolId symbol)
			{
				while (groups[symbol] != symbol)
				{
					groups[symbol] = groups[groups[symbol]];
					symbol         = groups[symbol];
				}

				return symbol;
			};
			for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				const auto next = symbols.next_same_name[symbol];
				if (next < symbols.Size() && symbols.scope[symbol] == symbols.scope[next] &&
					symbols.name[symbol] == symbols.name[next])
				{
					groups[root(next)] = root(symbol);
				}
			}

			for (auto& group : groups)
			{
				group = root(group);
			}

			std::vector<std::uint32_t> counts(symbols.Size());
			for (std::size_t ref = 0; ref < refs.target.size(); ++ref)
			{
				const auto token = refs.token[ref];
				if (refs.state[ref] == RefState::Resolved && refs.target[ref] < counts.size() &&
					token < tokens.size() && static_cast<std::size_t>(tokens[token].offset) + tokens[token].length < offset)
				{
					++counts[groups[refs.target[ref]]];
				}
			}

			std::vector<ScopeId> namespaces(symbols.Size());
			std::vector<std::uint32_t> namespaceUses(scopes.Size());
			std::vector<std::pair<std::uint32_t, SymbolId>> byOffset;
			byOffset.reserve(symbols.Size());
			for (SymbolId symbol = 0; symbol < symbols.Size(); ++symbol)
			{
				auto scope = symbols.scope[symbol];
				for (std::size_t depth = 0; scope < scopes.Size() && depth < scopes.Size(); ++depth)
				{
					if (scopes.kind[scope] == ScopeKind::Namespace || scope == SemanticModel::TranslationUnitScope)
					{
						namespaces[symbol] = scope;
						break;
					}

					scope = scopes.parent[scope];
				}

				if (groups[symbol] == symbol && counts[symbol] > 0 && namespaces[symbol] < namespaceUses.size())
				{
					++namespaceUses[namespaces[symbol]];
				}

				if (symbols.decl_token[symbol] < tokens.size())
				{
					byOffset.emplace_back(tokens[symbols.decl_token[symbol]].offset, symbol);
				}
			}

			std::sort(byOffset.begin(), byOffset.end());
			for (auto& item : items)
			{
				if (!item.has_location || item.file != -1)
				{
					continue;
				}

				const auto found = std::lower_bound(byOffset.begin(), byOffset.end(), item.offset,
					[](const auto& entry, std::uint32_t position)
					{
						return entry.first < position;
					});
				if (found != byOffset.end() && found->first == item.offset)
				{
					const auto symbol       = found->second;
					item.mainFileReferences = counts[groups[symbol]];
					item.scopeReferences    = namespaceUses[namespaces[symbol]];
				}
			}

			CompletionRanking::Rank(items, CompletionEngine::PrefixAt(context.Syntax().Source(), offset));
		}
	}

	std::vector<CompletionItem> AnalysisFeatures::Complete(
		const AnalysisContext& context,
		std::size_t offset,
		const ScopeIndex* external)
	{
		if (!context.Valid())
			return {};
		offset = std::min(offset, context.Syntax().Source().size());
		auto items = CompletionEngine::Complete(
			context.Syntax(), context.Snapshot().Options(context.Document()), offset, external);
		ApplyCompletionUses(context, offset, items);
		return items;
	}

	std::optional<CompletionItem> AnalysisFeatures::Hover(
		const AnalysisContext& context,
		std::size_t offset,
		const ScopeIndex* external)
	{
		if (!context.Valid())
			return {};
		return CompletionEngine::Hover(
			context.Syntax(), context.Snapshot().Options(context.Document()), offset, external);
	}

	SignatureHelp AnalysisFeatures::Signatures(
		const AnalysisContext& context,
		std::size_t offset,
		const ScopeIndex* external)
	{
		if (!context.Valid())
			return {};
		return CompletionEngine::Signatures(
			context.Syntax(), context.Snapshot().Options(context.Document()), offset, external);
	}

	std::vector<NavTarget> AnalysisFeatures::Definition(
		const AnalysisContext& context,
		std::size_t offset,
		const ScopeIndex* external)
	{
		return context.Valid() ? Navigation::Definition(context.Syntax(), offset, external)
		: std::vector<NavTarget> {};
	}

	std::vector<NavTarget> AnalysisFeatures::Implementation(
		const AnalysisContext& context,
		std::size_t offset,
		const ScopeIndex* external)
	{
		return context.Valid() ? Navigation::Implementation(context.Syntax(), offset, external)
		: std::vector<NavTarget> {};
	}

	std::vector<Diagnostic> AnalysisFeatures::Diagnostics(
		const AnalysisContext& context,
		const RuleEngine& engine,
		bool semantic,
		const ProjectContext& project)
	{
		if (!context.Valid())
			return {};
		auto diagnostics = engine.Analyze(context.Syntax());
		if (semantic)
		{
			auto analyzed = detail::AnalyzeSelectedSemantic(context, engine, project);
			analyzed      = engine.ApplyPolicy(std::move(analyzed), context.Syntax());
			diagnostics.insert(diagnostics.end(), std::make_move_iterator(analyzed.begin()),
				std::make_move_iterator(analyzed.end()));
		}

		std::stable_sort(
			diagnostics.begin(), diagnostics.end(),
			[](const Diagnostic& a, const Diagnostic& b)
			{
				return a.offset < b.offset;
			});
		return diagnostics;
	}
} // namespace heimdall
