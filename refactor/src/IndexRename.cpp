#include <Heimdall/IndexRename.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

#include <Heimdall/ParseTree.hpp>
#include <Heimdall/ShardBuilder.hpp>

#include "Support.hpp"

namespace heimdall
{
	namespace
	{
		namespace idx = heimdall::index;

		std::unexpected<RefactoringError> Fail(RefactoringErrorCode code, std::string message)
		{
			return std::unexpected(RefactoringError {code, std::move(message)});
		}

		std::size_t LineOf(std::string_view text, std::size_t offset)
		{
			return 1 + static_cast<std::size_t>(std::count(
				text.begin(), text.begin() + std::min(offset, text.size()), '\n'));
		}

		struct Edit
		{
			std::uint32_t offset;
			std::uint32_t length;
		};

		std::uint32_t MapOffset(
			const std::vector<Edit>& edits,
			std::int64_t delta,
			std::uint32_t offset)
		{
			std::int64_t shift = 0;
			for (const auto& edit : edits)
			{
				if (edit.offset < offset)
				{
					shift += delta;
				}
			}

			return static_cast<std::uint32_t>(static_cast<std::int64_t>(offset) + shift);
		}

		bool IsIdentifierChar(char c)
		{
			return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
		}

		// After a class-name use at `end`: `::` [~] name, where name repeats the class name.
		std::optional<std::uint32_t> ConstructorNameAfter(
			std::string_view text,
			std::size_t end,
			std::string_view name)
		{
			auto at = end;
			const auto skip = [&]
			{
				while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0)
				{
					++at;
				}
			};
			skip();
			if (text.substr(at, 2) != "::")
			{
				return std::nullopt;
			}

			at += 2;
			skip();
			if (at < text.size() && text[at] == '~')
			{
				++at;
				skip();
			}

			if (text.substr(at, name.size()) != name ||
				(at + name.size() < text.size() && IsIdentifierChar(text[at + name.size()])))
			{
				return std::nullopt;
			}

			return static_cast<std::uint32_t>(at);
		}

		struct Run
		{
			const idx::IndexSnapshot& snapshot;
			const IndexRenameContext& context;
			std::stop_token stop;
			std::map<std::string, IndexedDocument> documents;

			const IndexedDocument* Document(const std::string& path)
			{
				if (const auto found = documents.find(path); found != documents.end())
				{
					return &found->second;
				}

				if (!context.document)
				{
					return nullptr;
				}

				auto loaded = context.document(path);
				if (!loaded ||!loaded->text)
				{
					return nullptr;
				}

				return &documents.emplace(path, std::move(*loaded)).first->second;
			}

			std::string Where(std::uint32_t shard, std::uint32_t offset)
			{
				const auto& path     = snapshot.Shards()[shard]->Path();
				const auto* document = Document(path);
				return path + ":" +
					(document ? std::to_string(LineOf(*document->text, offset)) : "?");
			}
		};

		RefactoringError Cancelled();

		struct IdentifierToken
		{
			std::uint32_t offset;
			std::uint32_t length;
		};

		bool IsTriviaToken(TokenKind kind)
		{
			return kind == TokenKind::Whitespace || kind == TokenKind::LineComment ||
				kind == TokenKind::BlockComment;
		}

		// Byte ranges of `#include` / `#import` lines: header names are not identifiers.
		std::vector<std::pair<std::size_t, std::size_t>> IncludeLines(std::string_view text)
		{
			std::vector<std::pair<std::size_t, std::size_t>> lines;
			std::size_t start = 0;
			while (start < text.size())
			{
				std::size_t end = text.find('\n', start);
				end             = end == std::string_view::npos ? text.size() : end;
				std::size_t at  = start;
				while (at < end && (text[at] == ' ' || text[at] == '\t'))
				{
					++at;
				}

				if (at < end && text[at] == '#')
				{
					++at;
					while (at < end && (text[at] == ' ' || text[at] == '\t'))
					{
						++at;
					}

					if (text.substr(at, 7) == "include" || text.substr(at, 6) == "import")
					{
						lines.emplace_back(start, end);
					}
				}

				start = end + 1;
			}

			return lines;
		}

		// Renames a macro: its definitions, every use the index resolved to it (code,
		// conditions, other macros' bodies). It refuses whenever the old spelling is reachable
		// in a way the index does not see, or the new one could change what other code means.
		std::expected<RefactoringPlan, RefactoringError> RenameMacro(
			const idx::IndexSnapshot& snapshot,
			const PreparedIndexRename& prepared,
			std::string_view newName,
			const IndexRenameContext& context,
			Run& run,
			std::stop_token stop)
		{
			const std::string& oldName = prepared.name;
			const auto hash            = snapshot.HashOf(prepared.symbol);
			std::map<std::uint32_t, std::set<std::uint32_t>> uses;
			for (std::uint32_t shardIndex = 0; shardIndex < snapshot.Shards().size(); ++shardIndex)
			{
				const auto& file     = *snapshot.Shards()[shardIndex];
				const auto& occ      = file.Occurrences();
				const auto& resolved = snapshot.Resolved(shardIndex);
				for (std::uint32_t i = 0; i < occ.Size(); ++i)
				{
					if ((occ.flags[i] & idx::kOccurrenceMacro) == 0 ||
						file.Strings().Text(occ.name[i]) != oldName)
					{
						continue;
					}

					const auto targets = resolved.Targets(i);
					if (resolved.state[i] != idx::Resolution::Resolved || targets.size() != 1 ||
						snapshot.HashOf(targets.front()) != hash)
					{
						return Fail(RefactoringErrorCode::IncompleteAnalysis,
							"A use of this macro is not resolved to its definition: " +
							run.Where(shardIndex, occ.offset[i]));
					}

					uses[shardIndex].insert(occ.offset[i]);
				}
			}

			for (std::uint32_t shardIndex = 0; shardIndex < snapshot.Shards().size(); ++shardIndex)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(Cancelled());
				}

				const auto& file     = *snapshot.Shards()[shardIndex];
				const auto* document = run.Document(file.Path());
				if (document == nullptr)
				{
					return Fail(RefactoringErrorCode::IncompleteAnalysis,
						"Document is not available: " + file.Path());
				}

				if (idx::HashBytes(*document->text) != file.ContentHash() ||
					(document->configHash && *document->configHash != file.ConfigHash()))
				{
					return Fail(RefactoringErrorCode::StaleSnapshot,
						"The index of " + file.Path() + " is outdated; request the rename again");
				}

				if (document->options && document->options->Macros().contains(std::string(newName)))
				{
					return Fail(RefactoringErrorCode::MacroContext,
						"The new name is a predefined macro: " + file.Path());
				}

				const std::string_view text = *document->text;
				const bool hasOld           = text.find(oldName) != std::string_view::npos;
				if (!hasOld && text.find(newName) == std::string_view::npos)
				{
					continue;
				}

				if (!file.SyntaxComplete() && hasOld)
				{
					return Fail(RefactoringErrorCode::IncompleteAnalysis,
						"Syntax errors prevent proving the rename for an affected name: " +
						file.Path());
				}

				const auto includeLines = IncludeLines(text);
				const auto inInclude = [&](std::size_t offset)
				{
					return std::ranges::any_of(includeLines,[&](const auto& line)
						{
							return offset >= line.first && offset < line.second;
						});
				};
				const auto tokens = Lexer(text).Lex();
				std::vector<std::size_t> significant;
				for (std::size_t t = 0; t < tokens.size(); ++t)
				{
					if (!IsTriviaToken(tokens[t].kind))
					{
						significant.push_back(t);
					}
				}

				const auto spelling = [&](std::size_t sig)
				{
					return text.substr(tokens[significant[sig]].offset, tokens[significant[sig]].length);
				};
				// Macros used in this file that paste or stringify their arguments.
				std::set<std::uint32_t> pastingCalls;
				{
					const auto& occ      = file.Occurrences();
					const auto& resolved = snapshot.Resolved(shardIndex);
					for (std::uint32_t i = 0; i < occ.Size(); ++i)
					{
						const auto targets = resolved.Targets(i);
						if ((occ.flags[i] & idx::kOccurrenceMacro) != 0 && targets.size() == 1 &&
							(snapshot.Shards()[targets.front().shard]->Symbols().flags[targets.front().symbol] &
							idx::kMacroTokenPasting) != 0)
						{
							pastingCalls.insert(occ.offset[i]);
						}
					}
				}

				const auto found = uses.find(shardIndex);
				for (std::size_t sig = 0; sig < significant.size(); ++sig)
				{
					const Token& token = tokens[significant[sig]];
					if (token.kind != TokenKind::Identifier || inInclude(token.offset))
					{
						continue;
					}

					const auto word = spelling(sig);
					if (word == newName)
					{
						return Fail(RefactoringErrorCode::NameCollision,
							"The new name is already used: " +
							run.Where(shardIndex, static_cast<std::uint32_t>(token.offset)));
					}

					if (word != oldName)
					{
						continue;
					}

					if (found == uses.end() ||!found->second.contains(token.offset))
					{
						return Fail(RefactoringErrorCode::IncompleteAnalysis,
							"An occurrence of the macro's spelling is not a recorded use "
							"(inactive code?): " +
							run.Where(shardIndex, static_cast<std::uint32_t>(token.offset)));
					}

					const bool pasted = (sig > 0 && (spelling(sig - 1) == "##" || spelling(sig - 1) == "#")) ||
						(sig + 1 < significant.size() && spelling(sig + 1) == "##");
					if (pasted)
					{
						return Fail(RefactoringErrorCode::MacroContext,
							"The macro name takes part in token pasting or stringification: " +
							run.Where(shardIndex, static_cast<std::uint32_t>(token.offset)));
					}

					// An argument of a macro that pastes or stringifies it: its spelling is the result.
					constexpr std::size_t kMaxLookback = 4096;
					std::size_t depth                  = 0;
					for (std::size_t back = sig, steps = 0; back > 0 && steps < kMaxLookback; --back, ++steps)
					{
						const auto previous = spelling(back - 1);
						if (previous == ")")
						{
							++depth;
						}
						else if (previous == "(" && depth > 0)
						{
							--depth;
						}
						else if (previous == "(")
						{
							if (back >= 2 && pastingCalls.contains(static_cast<std::uint32_t>(
								tokens[significant[back - 2]].offset)))
							{
								return Fail(RefactoringErrorCode::MacroContext,
									"The macro name is an argument of a macro that pastes or stringifies "
									"it: " + run.Where(shardIndex, static_cast<std::uint32_t>(token.offset)));
							}
						}
						else if (depth == 0 && (previous == ";" || previous == "{" || previous == "}"))
						{
							break;
						}
					}
				}
			}

			std::set<std::string> seen;
			RefactoringPlan plan {"Rename " + oldName + " to " + std::string(newName),
				snapshot.Revision(), {}};
			for (const auto& [shardIndex, offsets] : uses)
			{
				const auto& file     = *snapshot.Shards()[shardIndex];
				const auto* document = run.Document(file.Path());
				if (context.editable && !context.editable(file.Path()))
				{
					return Fail(RefactoringErrorCode::IncompleteAnalysis,
						"Document cannot be edited: " + file.Path());
				}

				DocumentEdits documentEdits {document->document, file.Path(), document->version,
					document->text, {}, document->options};
				std::string edited;
				std::size_t cursor = 0;
				for (const auto use : offsets)
				{
					if (use + oldName.size() > document->text->size() ||
						document->text->compare(use, oldName.size(), oldName) != 0)
					{
						return Fail(RefactoringErrorCode::StaleSnapshot,
							"Workspace changed; request the rename again");
					}

					documentEdits.edits.push_back({use, oldName.size(), oldName, std::string(newName)});
					edited.append(*document->text, cursor, use - cursor);
					edited += newName;
					cursor = use + oldName.size();
				}

				edited.append(*document->text, cursor, std::string::npos);
				const auto options = document->options.value_or(ParserOptions {});
				const auto before  = ParseTree::Parse(*document->text, options, stop);
				const auto after   = ParseTree::Parse(edited, options, stop);
				if (before.Cancelled() || after.Cancelled() || stop.stop_requested())
				{
					return std::unexpected(Cancelled());
				}

				if (after.Diagnostics().size() > before.Diagnostics().size())
				{
					return Fail(RefactoringErrorCode::SemanticChange,
						"The rename would introduce parse errors in " + file.Path());
				}

				plan.documents.push_back(std::move(documentEdits));
			}

			return plan;
		}

		RefactoringError Cancelled()
		{
			return {RefactoringErrorCode::Cancelled, "Request cancelled"};
		}

		idx::SymbolRef ClassOwner(const idx::IndexSnapshot& snapshot, idx::SymbolRef symbol)
		{
			const auto& file = *snapshot.Shards()[symbol.shard];
			const auto owner = file.Symbols().container[symbol.symbol];
			if (owner != idx::kInvalid && file.Symbols().kind[owner] == SymbolKind::Class)
			{
				return {symbol.shard, owner};
			}

			const auto qualified = file.Qualified(symbol.symbol);
			const auto separator = qualified.rfind("::");
			idx::SymbolRef result;
			if (separator != std::string_view::npos)
			{
				for (const auto candidate : snapshot.Named(qualified.substr(0, separator)))
				{
					if (snapshot.Shards()[candidate.shard]->Symbols().kind[candidate.symbol] != SymbolKind::Class)
					{
						continue;
					}

					if (result.Valid() && snapshot.HashOf(result) != snapshot.HashOf(candidate))
					{
						return {};
					}

					result = candidate;
				}
			}

			return result;
		}

		std::expected<std::vector<idx::SymbolHash>, RefactoringError> RenameFamily(
			const idx::IndexSnapshot& snapshot,
			idx::SymbolRef anchor,
			std::stop_token stop)
		{
			const auto hash        = snapshot.HashOf(anchor);
			const auto owner       = ClassOwner(snapshot, anchor);
			const auto& targetFile = *snapshot.Shards()[anchor.shard];
			if (!owner.Valid() || targetFile.Symbols().kind[anchor.symbol] != SymbolKind::Function)
			{
				return std::vector<idx::SymbolHash> {hash};
			}

			struct Method
			{
				idx::SymbolRef symbol;
				idx::SymbolHash hash;
				idx::SymbolHash owner;
				std::string_view signature;
				std::uint32_t flags;
				std::set<idx::SymbolHash> ancestors;
			};

			std::vector<Method> methods;
			std::map<idx::SymbolHash, std::size_t> byHash;
			for (std::uint32_t shard = 0; shard < snapshot.Shards().size(); ++shard)
			{
				const auto& file = *snapshot.Shards()[shard];
				for (std::uint32_t symbol = 0; symbol < file.Symbols().Size(); ++symbol)
				{
					if (stop.stop_requested())
					{
						return std::unexpected(Cancelled());
					}

					if (file.Symbols().kind[symbol] != SymbolKind::Function || file.Name(symbol) != targetFile.Name(anchor.symbol) ||
						(file.Symbols().flags[symbol] & (SymbolFlag::Static | SymbolFlag::Constructor | SymbolFlag::Destructor | SymbolFlag::Operator)) != 0)
					{
						continue;
					}

					const idx::SymbolRef ref
					{
						shard, symbol
					};
					const auto container = ClassOwner(snapshot, ref);
					if (!container.Valid())
					{
						continue;
					}

					const auto methodHash    = snapshot.HashOf(ref);
					const auto[at, inserted] = byHash.emplace(methodHash, methods.size());
					if (!inserted)
					{
						methods[at->second].flags |= file.Symbols().flags[symbol];
						continue;
					}

					methods.emplace_back(ref, methodHash, snapshot.HashOf(container),
						file.Strings().Text(file.Symbols().signature[symbol]), file.Symbols().flags[symbol],
						std::set<idx::SymbolHash> {});
				}
			}

			for (auto& method : methods)
			{
				std::vector<idx::SymbolHash> pending {method.owner};
				std::set<idx::SymbolHash> visited;
				while (!pending.empty())
				{
					if (stop.stop_requested())
					{
						return std::unexpected(Cancelled());
					}

					const auto current = pending.back();
					pending.pop_back();
					if (!visited.insert(current).second)
					{
						continue;
					}

					if (snapshot.Declarations(current).empty())
					{
						return Fail(RefactoringErrorCode::IncompleteAnalysis,
							"A method hierarchy contains an unindexed or conflicting class");
					}

					for (const auto declaration : snapshot.Declarations(current))
					{
						const auto& file = *snapshot.Shards()[declaration.shard];
						if ((file.Symbols().flags[declaration.symbol] & SymbolFlag::Template) != 0)
						{
							return Fail(RefactoringErrorCode::IncompleteAnalysis,
								"A template hierarchy requires instantiation coverage: " + file.Path());
						}

						for (const auto relation : file.RelationsTo(current))
						{
							const auto& relations = file.Relations();
							if (relations.kind[relation] == idx::RelationKind::BaseOf)
							{
								pending.push_back(relations.subject[relation]);
								method.ancestors.insert(relations.subject[relation]);
							}
							else
							{
								const auto occurrence = file.OccurrenceAt(relations.offset[relation]);
								if (occurrence == idx::kInvalid || snapshot.Resolved(declaration.shard).state[occurrence] != idx::Resolution::Resolved ||
									snapshot.Resolved(declaration.shard).Targets(occurrence).size() != 1)
								{
									return Fail(RefactoringErrorCode::IncompleteAnalysis,
										"A method hierarchy has an unresolved base: " + file.Path());
								}

								const auto base = snapshot.HashOf(snapshot.Resolved(declaration.shard).Targets(occurrence).front());
								pending.push_back(base);
								method.ancestors.insert(base);
							}
						}
					}
				}
			}

			std::vector<bool> isVirtual(methods.size());
			for (std::size_t i = 0; i < methods.size(); ++i)
			{
				isVirtual[i] = (methods[i].flags & (SymbolFlag::Virtual | SymbolFlag::Pure)) != 0;
			}

			bool changed = true;
			while (changed)
			{
				changed = false;
				for (std::size_t derived = 0; derived < methods.size(); ++derived)
				{
					for (std::size_t base = 0; base < methods.size(); ++base)
					{
						if (isVirtual[base] && !isVirtual[derived] && methods[derived].ancestors.contains(methods[base].owner) &&
							methods[derived].signature == methods[base].signature)
						{
							isVirtual[derived] = true;
							changed            = true;
						}
					}
				}
			}

			const auto selected = byHash.find(hash);
			if (selected != byHash.end())
			{
				for (std::size_t base = 0; base < methods.size(); ++base)
				{
					if (isVirtual[base] && methods[selected->second].ancestors.contains(methods[base].owner) &&
						methods[selected->second].signature != methods[base].signature)
					{
						return Fail(RefactoringErrorCode::IncompleteAnalysis,
							"Same-named base overloads require canonical type and override resolution");
					}
				}
			}

			if (selected == byHash.end() ||!isVirtual[selected->second])
			{
				return std::vector<idx::SymbolHash> {hash};
			}

			std::set<std::size_t> family {selected->second};
			changed = true;
			while (changed)
			{
				changed = false;
				for (std::size_t i = 0; i < methods.size(); ++i)
				{
					if (!isVirtual[i] || family.contains(i))
					{
						continue;
					}

					for (const auto member : family)
					{
						if (methods[i].signature == methods[member].signature &&
							(methods[i].ancestors.contains(methods[member].owner) || methods[member].ancestors.contains(methods[i].owner)))
						{
							family.insert(i);
							changed = true;
							break;
						}
					}
				}
			}

			std::vector<idx::SymbolHash> result;
			for (const auto member : family)
			{
				result.push_back(methods[member].hash);
			}

			return result;
		}
	} // namespace

	bool IndexRename::SpansFiles(
		const idx::IndexSnapshot& snapshot,
		std::string_view path,
		std::size_t offset)
	{
		const auto shard = snapshot.ShardFor(path);
		if (shard == idx::kInvalid)
		{
			return false;
		}

		auto anchor = snapshot.SymbolAt(shard, static_cast<std::uint32_t>(offset));
		if (!anchor.Valid())
		{
			const auto candidates = snapshot.Candidates(shard, static_cast<std::uint32_t>(offset));
			if (candidates.size() != 1)
			{
				return false;
			}

			anchor = candidates.front();
		}

		const auto hash = snapshot.HashOf(anchor);
		std::set<std::uint32_t> shards;
		for (const auto& declaration : snapshot.Declarations(hash))
		{
			shards.insert(declaration.shard);
		}

		for (const auto& use : snapshot.References(hash))
		{
			shards.insert(use.shard);
		}

		return shards.size() > 1;
	}

	std::expected<PreparedIndexRename, RefactoringError> IndexRename::Prepare(
		const idx::IndexSnapshot& snapshot,
		std::string_view path,
		std::size_t offset,
		const IndexRenameContext& context,
		std::stop_token stop)
	{
		Run run {snapshot, context, stop, {}};
		if (stop.stop_requested())
		{
			return std::unexpected(Cancelled());
		}

		if (!context.complete)
		{
			return Fail(RefactoringErrorCode::IncompleteAnalysis,
				context.coverageIssue.empty() ? "The project index is still being built or skipped some files; try again once indexing has finished" :
				context.coverageIssue);
		}

		if (context.requireExactIncludes)
		{
			for (const auto& file : snapshot.Shards())
			{
				if (context.editable && !context.editable(file->Path()))
				{
					continue;
				}

				if (!file->ExactIncludes())
				{
					return Fail(RefactoringErrorCode::IncompleteAnalysis,
						"Include resolution is incomplete: " + file->Path());
				}

				for (const auto target : file->IncludeTargets())
				{
					if (target == idx::kInvalid)
					{
						return Fail(RefactoringErrorCode::IncompleteAnalysis,
							"An include in " + file->Path() + " could not be resolved");
					}

					const std::string targetPath(file->Strings().Text(target));
					if (context.editable && !context.editable(targetPath))
					{
						continue;
					}

					if (snapshot.ShardFor(targetPath) == idx::kInvalid)
					{
						return Fail(RefactoringErrorCode::IncompleteAnalysis,
							"An included dependency is not indexed: " + targetPath +
							" (included from " + file->Path() + ")");
					}
				}
			}
		}

		const auto origin   = snapshot.ShardFor(path);
		const auto* current = origin == idx::kInvalid ? nullptr : run.Document(std::string(path));
		if (current == nullptr)
		{
			return Fail(RefactoringErrorCode::UnsupportedSymbol, "This file is not indexed");
		}

		if (!snapshot.Shards()[origin]->SyntaxComplete())
		{
			return Fail(RefactoringErrorCode::IncompleteAnalysis,
				"The selected document has syntax errors: " + std::string(path));
		}

		if (offset >= current->text->size() || offset > std::numeric_limits<std::uint32_t>::max())
		{
			return Fail(RefactoringErrorCode::InvalidSelection, "Select an identifier");
		}

		if (idx::HashBytes(*current->text) != snapshot.Shards()[origin]->ContentHash() ||
			(current->configHash && *current->configHash != snapshot.Shards()[origin]->ConfigHash()))
		{
			return Fail(RefactoringErrorCode::StaleSnapshot,
				"The index of this file is outdated; request the rename again");
		}

		const auto occurrence = snapshot.Shards()[origin]->OccurrenceAt(static_cast<std::uint32_t>(offset));
		if (occurrence == idx::kInvalid)
		{
			return Fail(RefactoringErrorCode::InvalidSelection, "Select an indexed identifier");
		}

		const auto& resolved = snapshot.Resolved(origin);
		if (resolved.state[occurrence] != idx::Resolution::Resolved || resolved.Targets(occurrence).size() != 1)
		{
			return Fail(RefactoringErrorCode::IncompleteAnalysis,
				"The selected occurrence does not resolve to exactly one entity");
		}

		auto anchor             = resolved.Targets(occurrence).front();
		const auto& initial     = *snapshot.Shards()[anchor.shard];
		const auto impliedOwner = ClassOwner(snapshot, anchor);
		const bool qualifiedConstructor = initial.Symbols().kind[anchor.symbol] == SymbolKind::Function &&
			(initial.Symbols().flags[anchor.symbol] & SymbolFlag::Qualified) != 0 && impliedOwner.Valid() &&
			initial.Name(anchor.symbol) == snapshot.Shards()[impliedOwner.shard]->Name(impliedOwner.symbol);
		if (qualifiedConstructor ||(initial.Symbols().flags[anchor.symbol] & (SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0)
		{
			const auto owner = initial.Symbols().container[anchor.symbol];
			if (owner != idx::kInvalid && initial.Symbols().kind[owner] == SymbolKind::Class)
			{
				anchor.symbol = owner;
			}
			else
			{
				const auto qualified = initial.Qualified(anchor.symbol);
				const auto separator = qualified.rfind("::");
				std::vector<idx::SymbolRef> owners;
				for (const auto candidate : separator == std::string_view::npos ? std::vector<idx::SymbolRef> {}:
					snapshot.Named(qualified.substr(0, separator)))
				{
					const auto& file = *snapshot.Shards()[candidate.shard];
					if (separator != std::string_view::npos && file.Qualified(candidate.symbol) == qualified.substr(0,
						separator) &&
						file.Symbols().kind[candidate.symbol] == SymbolKind::Class)
					{
						owners.push_back(candidate);
					}
				}

				if (owners.empty())
				{
					return Fail(RefactoringErrorCode::IncompleteAnalysis, "The constructor's class is not indexed");
				}

				anchor = owners.front();
				if (std::ranges::any_of(owners,[&](const idx::SymbolRef candidate)
					{
						return snapshot.HashOf(candidate) != snapshot.HashOf(anchor);
					}))
				{
					return Fail(RefactoringErrorCode::IncompleteAnalysis, "The constructor's class is ambiguous");
				}
			}
		}

		const auto hash         = snapshot.HashOf(anchor);
		const auto declarations = snapshot.Declarations(hash);
		if (declarations.empty())
		{
			return Fail(RefactoringErrorCode::UnsupportedSymbol, "The symbol is not declared in the project");
		}

		const auto& declaringShard = *snapshot.Shards()[anchor.shard];
		const std::string oldName(declaringShard.Name(anchor.symbol));
		const auto flags = declaringShard.Symbols().flags[anchor.symbol];

		for (const auto& declaration : declarations)
		{
			const auto& declarationPath = snapshot.Shards()[declaration.shard]->Path();
			if (context.editable && !context.editable(declarationPath))
			{
				return Fail(RefactoringErrorCode::UnsupportedSymbol,
					"The symbol is declared outside the workspace: " + declarationPath);
			}
		}

		if ((flags & (SymbolFlag::Operator | SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0)
		{
			return Fail(RefactoringErrorCode::UnsupportedSymbol,
				"Operators, constructors and destructors cannot be renamed on their own");
		}

		const auto family = RenameFamily(snapshot, anchor, stop);
		if (!family)
		{
			return std::unexpected(family.error());
		}

		for (const auto member : *family)
		{
			for (const auto declaration : snapshot.Declarations(member))
			{
				if (context.editable && !context.editable(snapshot.Shards()[declaration.shard]->Path()))
				{
					return Fail(RefactoringErrorCode::UnsupportedSymbol,
						"A virtual family declaration is outside the editable workspace");
				}
			}
		}

		const auto& occurrences = snapshot.Shards()[origin]->Occurrences();
		return PreparedIndexRename {anchor, occurrences.offset[occurrence], occurrences.length[occurrence],
			oldName, *family};
	}

	std::expected<RefactoringPlan, RefactoringError> IndexRename::Rename(
		const idx::IndexSnapshot& snapshot,
		std::string_view path,
		std::size_t offset,
		std::string_view newName,
		const IndexRenameContext& context,
		std::stop_token stop)
	{
		if (stop.stop_requested())
		{
			return std::unexpected(Cancelled());
		}

		if (!refactor_detail::ValidName(newName))
		{
			return Fail(RefactoringErrorCode::InvalidName, "Use a non-reserved ASCII C++ identifier");
		}

		const auto prepared = Prepare(snapshot, path, offset, context, stop);
		if (!prepared)
		{
			return std::unexpected(prepared.error());
		}

		Run run {snapshot, context, stop, {}};
		const auto anchor          = prepared->symbol;
		const auto hash            = snapshot.HashOf(anchor);
		const auto declarations    = snapshot.Declarations(hash);
		const auto& declaringShard = *snapshot.Shards()[anchor.shard];
		const auto& oldName        = prepared->name;
		const auto kind            = declaringShard.Symbols().kind[anchor.symbol];
		if (oldName == newName)
		{
			return RefactoringPlan {"Rename " + oldName, snapshot.Revision(), {}};
		}

		if (kind == SymbolKind::Macro)
		{
			return RenameMacro(snapshot, *prepared, newName, context, run, stop);
		}

		if (declaringShard.Symbols().container[anchor.symbol] != idx::kInvalid)
		{
			std::vector<idx::SymbolRef> pending;
			std::set<idx::SymbolHash> seen;
			const auto& container = declaringShard.Symbols().hash[declaringShard.Symbols().container[anchor.symbol]];
			pending.push_back({anchor.shard, declaringShard.Symbols().container[anchor.symbol]});
			seen.insert(container);
			while (!pending.empty())
			{
				const auto current = pending.back();
				pending.pop_back();
				for (const auto& derived : snapshot.Subtypes(snapshot.HashOf(current)))
				{
					const auto derivedHash = snapshot.HashOf(derived.symbol);
					if (!seen.insert(derivedHash).second)
					{
						continue;
					}

					const auto& file = *snapshot.Shards()[derived.symbol.shard];
					for (std::uint32_t s = 0; s < file.Symbols().Size(); ++s)
					{
						if (file.Symbols().container[s] == derived.symbol.symbol &&
							file.Name(s) == oldName && std::ranges::find(prepared->family,
							file.Symbols().hash[s]) == prepared->family.end())
						{
							return Fail(RefactoringErrorCode::NameCollision,
								"A derived class declares a member with the same name: " +
								std::string(file.Path()));
						}
					}

					pending.push_back(derived.symbol);
				}
			}
		}

		// Occurrences of the entity, all of which must be exact.
		std::map<std::uint32_t, std::set<std::uint32_t>> offsets; // shard -> offsets
		for (const auto member : prepared->family)
		{
			for (const auto& use : snapshot.References(member))
			{
				if (use.resolution != idx::Resolution::Resolved)
				{
					return Fail(RefactoringErrorCode::IncompleteAnalysis,
						"A use of this symbol is not resolved to a single declaration: " +
						run.Where(use.shard, use.offset));
				}

				offsets[use.shard].insert(use.offset);
			}
		}

		if (kind == SymbolKind::Class)
		{
			for (const auto& [shardIndex, uses] : std::map(offsets))
			{
				const auto& file     = *snapshot.Shards()[shardIndex];
				const auto* document = run.Document(file.Path());
				if (document == nullptr)
				{
					return Fail(RefactoringErrorCode::IncompleteAnalysis,
						"Document is not available: " + file.Path());
				}

				for (const auto use : uses)
				{
					if (const auto name = ConstructorNameAfter(*document->text, use + oldName.size(), oldName))
					{
						offsets[shardIndex].insert(*name);
					}
				}

				for (std::uint32_t s = 0; s < file.Symbols().Size(); ++s)
				{
					const auto symbolFlags = file.Symbols().flags[s];
					if ((symbolFlags & (SymbolFlag::Constructor | SymbolFlag::Destructor)) != 0 &&
						file.Name(s) == oldName &&
						file.Symbols().container[s] != idx::kInvalid &&
						file.Symbols().hash[file.Symbols().container[s]] == hash)
					{
						offsets[shardIndex].insert(file.Symbols().nameOffset[s]);
					}
				}
			}
		}

		if (stop.stop_requested())
		{
			return std::unexpected(Cancelled());
		}

		// Every spelling of the old name anywhere in the project must be accounted for.
		std::set<idx::SymbolHash> sameNamedEntities;
		for (const auto& file : snapshot.Shards())
		{
			for (std::uint32_t symbol = 0; symbol < file->Symbols().Size(); ++symbol)
			{
				if (file->Name(symbol) == oldName &&
					(file->Symbols().flags[symbol] & (SymbolFlag::Constructor | SymbolFlag::Destructor)) == 0)
				{
					sameNamedEntities.insert(file->Symbols().hash[symbol]);
				}
			}
		}

		const bool uniqueWorkspaceName = sameNamedEntities.size() == 1;
		for (std::uint32_t shardIndex = 0; shardIndex < snapshot.Shards().size(); ++shardIndex)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(Cancelled());
			}

			const auto& file = *snapshot.Shards()[shardIndex];
			const auto& occ  = file.Occurrences();
			for (std::uint32_t i = 0; i < occ.Size(); ++i)
			{
				if (file.Strings().Text(occ.name[i]) != oldName)
				{
					continue;
				}

				if (snapshot.Resolved(shardIndex).state[i] != idx::Resolution::Resolved)
				{
					return Fail(RefactoringErrorCode::IncompleteAnalysis,
						"An occurrence of the same spelling is not resolved, so it may refer to "
						"this symbol: " + run.Where(shardIndex, occ.offset[i]));
				}
			}

			const auto* document = run.Document(file.Path());
			if (document == nullptr)
			{
				return Fail(RefactoringErrorCode::IncompleteAnalysis,
					"Document is not available: " + file.Path());
			}

			if (!file.SyntaxComplete())
			{
				for (const auto& token : Lexer(*document->text).Lex())
				{
					if (token.kind == TokenKind::Identifier)
					{
						const auto spelling = std::string_view(*document->text).substr(token.offset, token.length);
						if (spelling == oldName || spelling == newName)
						{
							return Fail(RefactoringErrorCode::IncompleteAnalysis,
								"Syntax errors prevent proving the rename for an affected name: " + file.Path());
						}
					}
				}
			}

			if (idx::HashBytes(*document->text) != file.ContentHash() ||
				(document->configHash && *document->configHash != file.ConfigHash()))
			{
				return Fail(RefactoringErrorCode::StaleSnapshot,
					"The index of " + file.Path() + " is outdated; request the rename again");
			}

			if (document->options && document->options->Macros().contains(std::string(newName)))
			{
				return Fail(RefactoringErrorCode::MacroContext,
					"The new name is a predefined macro: " + file.Path());
			}

			const auto tree = ParseTree::Parse(*document->text,
				document->options.value_or(ParserOptions {}), stop);
			if (tree.Cancelled())
			{
				return std::unexpected(Cancelled());
			}

			for (const auto& directive : tree.Directives())
			{
				if (directive.kind == DirectiveKind::Include)
				{
					continue;
				}

				for (const auto& token : Lexer(std::string_view(*document->text).substr(
					directive.offset, directive.length)).Lex())
				{
					if (token.kind == TokenKind::Identifier &&
						std::string_view(*document->text).substr(directive.offset + token.offset,
						token.length) == newName)
					{
						return Fail(RefactoringErrorCode::MacroContext,
							"The new name appears in a preprocessor directive: " + file.Path());
					}
				}
			}

			if (document->text->find(oldName) == std::string::npos)
			{
				continue;
			}

			const auto model = Binder::Bind(tree);
			std::vector<std::size_t> declarationOffsets;
			for (const auto member : prepared->family)
			{
				for (const auto declaration : snapshot.Declarations(member))
				{
					if (declaration.shard == shardIndex)
					{
						declarationOffsets.push_back(file.Symbols().nameOffset[declaration.symbol]);
					}
				}
			}

			DocumentEdits documentation;
			refactor_detail::CollectDocumentationEdits(tree, documentation, declarationOffsets,
				oldName, newName, kind == SymbolKind::Class || kind == SymbolKind::Function,
				uniqueWorkspaceName, declaringShard.Qualified(anchor.symbol));
			for (const auto& edit : documentation.edits)
			{
				offsets[shardIndex].insert(static_cast<std::uint32_t>(edit.offset));
			}

			std::set<std::uint32_t> declared;
			for (SymbolId s = 0; s < model.Symbols().Size(); ++s)
			{
				declared.insert(model.Symbols().decl_token[s]);
			}

			const auto& tokens = tree.Tokens();
			for (std::uint32_t t = 0; t < tokens.size(); ++t)
			{
				if (tokens[t].kind != TokenKind::Identifier || tree.Text(tokens[t]) != oldName)
				{
					continue;
				}

				for (const auto& directive : tree.Directives())
				{
					if (directive.kind != DirectiveKind::Include && tokens[t].offset >= directive.offset &&
						tokens[t].offset < directive.offset + directive.length)
					{
						return Fail(RefactoringErrorCode::MacroContext,
							"The name appears in a preprocessor directive: " +
							run.Where(shardIndex, static_cast<std::uint32_t>(tokens[t].offset)));
					}
				}

				const auto at       = file.OccurrenceAt(static_cast<std::uint32_t>(tokens[t].offset));
				const bool recorded = at != idx::kInvalid && occ.offset[at] == tokens[t].offset;
				if (!recorded && !declared.contains(t) && model.ResolveToken(t) == kNone)
				{
					return Fail(RefactoringErrorCode::IncompleteAnalysis,
						"An occurrence of the same spelling could not be classified: " +
						run.Where(shardIndex, static_cast<std::uint32_t>(tokens[t].offset)));
				}
			}
		}

		if (stop.stop_requested())
		{
			return std::unexpected(Cancelled());
		}

		// Edits, then re-index the edited files and compare what every name refers to.
		const std::int64_t delta = static_cast<std::int64_t>(newName.size()) -
			static_cast<std::int64_t>(oldName.size());
		std::map<std::uint32_t, std::vector<Edit>> edits;
		std::map<std::uint32_t, std::string> edited;
		for (const auto& [shardIndex, uses] : offsets)
		{
			const auto& file     = *snapshot.Shards()[shardIndex];
			const auto* document = run.Document(file.Path());
			if (document == nullptr || context.editable && !context.editable(file.Path()))
			{
				return Fail(RefactoringErrorCode::IncompleteAnalysis,
					"Document cannot be edited: " + file.Path());
			}

			if (idx::HashBytes(*document->text) != file.ContentHash())
			{
				return Fail(RefactoringErrorCode::StaleSnapshot,
					"The index of " + file.Path() + " is outdated; request the rename again");
			}

			std::string text;
			std::size_t cursor = 0;
			for (const auto use : uses)
			{
				if (use < cursor || use + oldName.size() > document->text->size() ||
					document->text->compare(use, oldName.size(), oldName) != 0)
				{
					return Fail(RefactoringErrorCode::StaleSnapshot,
						"Workspace changed; request the rename again");
				}

				text.append(*document->text, cursor, use - cursor);
				text += newName;
				cursor = use + oldName.size();
				edits[shardIndex].push_back({use, static_cast<std::uint32_t>(oldName.size())});
			}

			text.append(*document->text, cursor, std::string::npos);
			edited[shardIndex] = std::move(text);
		}

		std::vector<std::shared_ptr<const idx::FileShard>> shards(
			snapshot.Shards().begin(),
			snapshot.Shards().end());
		for (const auto& [shardIndex, text] : edited)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(Cancelled());
			}

			const auto& file     = *snapshot.Shards()[shardIndex];
			const auto* document = run.Document(file.Path());
			const auto options   = document->options.value_or(ParserOptions {});
			const auto before    = ParseTree::Parse(*document->text, options, stop);
			const auto after     = ParseTree::Parse(text, options, stop);
			if (before.Cancelled() || after.Cancelled() || stop.stop_requested())
			{
				return std::unexpected(Cancelled());
			}

			if (!before.Diagnostics().empty())
			{
				return Fail(RefactoringErrorCode::IncompleteAnalysis,
					"An edited document has parse errors: " + file.Path());
			}

			if (!after.Diagnostics().empty())
			{
				return Fail(RefactoringErrorCode::SemanticChange,
					"The rename would introduce parse errors in " + file.Path());
			}

			const auto model = Binder::Bind(after);
			shards[shardIndex] = idx::ShardBuilder::Build(model, file.Path(), file.ConfigHash(),
				[&file, &edits, shardIndex, delta](std::string_view, bool,
				std::uint32_t offset)->std::optional<std::string>
				{
					for (std::size_t include = 0; include < file.IncludeOffsets().size(); ++include)
					{
						if (MapOffset(edits.at(shardIndex), delta, file.IncludeOffsets()[include]) == offset &&
						file.IncludeTargets()[include] != idx::kInvalid)
						{
							return std::string(file.Strings().Text(file.IncludeTargets()[include]));
					}
				}

					return std::nullopt;
			});
		}

		const auto changed = idx::IndexSnapshot::Build(shards);
		std::unordered_map<idx::SymbolHash, idx::SymbolHash> renamed;
		std::set<idx::SymbolHash> targets;
		for (const auto& [shardIndex, shardEdits] : edits)
		{
			const auto& before = *snapshot.Shards()[shardIndex];
			for (std::uint32_t s = 0; s < before.Symbols().Size(); ++s)
			{
				const auto mapped      = MapOffset(shardEdits, delta, before.Symbols().nameOffset[s]);
				const auto afterSymbol = changed->SymbolAt(shardIndex, mapped);
				if (!afterSymbol.Valid())
				{
					return Fail(RefactoringErrorCode::SemanticChange,
						"A declaration would no longer be recognised after the rename: " +
						before.Path());
				}

				const auto oldHash = before.Symbols().hash[s];
				const auto newHash = changed->HashOf(afterSymbol);
				if (const auto[found, inserted] = renamed.emplace(oldHash, newHash);
					!inserted && found->second != newHash)
				{
					return Fail(RefactoringErrorCode::SemanticChange,
						"Declarations of one entity would diverge: " + before.Path());
				}
			}
		}

		for (const auto& [oldHash, newHash] : renamed)
		{
			if (oldHash != newHash && !targets.insert(newHash).second)
			{
				return Fail(RefactoringErrorCode::NameCollision,
					"The new name is already declared and would merge with another entity");
			}
		}

		const auto hashOf = [&](idx::SymbolHash old)
		{
			const auto found = renamed.find(old);
			return found == renamed.end() ? old : found->second;
		};
		for (const auto member : prepared->family)
		{
			const auto newHash = hashOf(member);
			if (changed->Declarations(newHash).size() != snapshot.Declarations(member).size())
			{
				return Fail(RefactoringErrorCode::NameCollision,
					"The new name is already declared in the project");
			}
		}

		for (std::uint32_t shardIndex = 0; shardIndex < snapshot.Shards().size(); ++shardIndex)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(Cancelled());
			}

			const auto& before = *snapshot.Shards()[shardIndex];
			const auto& after  = *changed->Shards()[shardIndex];
			const auto found   = edits.find(shardIndex);
			const std::vector<Edit> none;
			const auto& shardEdits     = found == edits.end() ? none : found->second;
			const auto& resolvedBefore = snapshot.Resolved(shardIndex);
			const auto& resolvedAfter  = changed->Resolved(shardIndex);
			const auto& occ            = before.Occurrences();
			for (std::uint32_t i = 0; i < occ.Size(); ++i)
			{
				const auto mapped = MapOffset(shardEdits, delta, occ.offset[i]);
				const auto j      = after.OccurrenceAt(mapped);
				bool same = j != idx::kInvalid && after.Occurrences().offset[j] == mapped &&
					resolvedBefore.state[i] == resolvedAfter.state[j];
				if (same)
				{
					std::vector<idx::SymbolHash> expected;
					std::vector<idx::SymbolHash> actual;
					for (const auto& target : resolvedBefore.Targets(i))
					{
						expected.push_back(hashOf(snapshot.HashOf(target)));
					}

					for (const auto& target : resolvedAfter.Targets(j))
					{
						actual.push_back(changed->HashOf(target));
					}

					std::ranges::sort(expected);
					std::ranges::sort(actual);
					same = expected == actual;
				}

				if (!same)
				{
					return Fail(RefactoringErrorCode::SemanticChange,
						"The rename would change what '" +
						std::string(before.Strings().Text(occ.name[i])) + "' refers to: " +
						run.Where(shardIndex, occ.offset[i]));
				}
			}
		}

		RefactoringPlan plan {"Rename " + oldName + " to " + std::string(newName), snapshot.Revision(), {}};
		for (const auto& [shardIndex, shardEdits] : edits)
		{
			const auto& file     = *snapshot.Shards()[shardIndex];
			const auto* document = run.Document(file.Path());
			DocumentEdits documentEdits {document->document, file.Path(), document->version,
				document->text, {}, document->options};
			for (const auto& edit : shardEdits)
			{
				documentEdits.edits.push_back(
					{edit.offset, edit.length, oldName, std::string(newName)});
			}

			plan.documents.push_back(std::move(documentEdits));
		}

		return plan;
	}

	std::expected<std::vector<RefactoredSource>, RefactoringError> IndexRename::Preview(
		const idx::IndexSnapshot& current,
		const RefactoringPlan& plan,
		const IndexRenameContext& context,
		std::stop_token stop)
	{
		if (stop.stop_requested())
		{
			return std::unexpected(Cancelled());
		}

		if (!context.complete || current.Revision() != plan.revision)
		{
			return Fail(RefactoringErrorCode::StaleSnapshot,
				"The project index changed; request the rename again");
		}

		Run run {current, context, stop, {}};
		for (const auto& shard : current.Shards())
		{
			if (stop.stop_requested())
			{
				return std::unexpected(Cancelled());
			}

			const auto* document = run.Document(shard->Path());
			if (document == nullptr || idx::HashBytes(*document->text) != shard->ContentHash() ||
				(document->configHash && *document->configHash != shard->ConfigHash()))
			{
				return Fail(RefactoringErrorCode::StaleSnapshot,
					"Document content or configuration changed: " + shard->Path());
			}
		}

		std::set<std::string> seen;
		std::vector<RefactoredSource> result;
		result.reserve(plan.documents.size());
		for (const auto& planned : plan.documents)
		{
			if (stop.stop_requested())
			{
				return std::unexpected(Cancelled());
			}

			const auto path      = planned.path.generic_string();
			const auto* document = run.Document(path);
			if (current.ShardFor(path) == idx::kInvalid || document == nullptr ||!planned.original ||
				document->document != planned.document || document->version != planned.version ||
				*document->text != *planned.original ||(context.editable && !context.editable(path)))
			{
				return Fail(RefactoringErrorCode::StaleSnapshot,
					"Document content, version or editability changed: " + path);
			}

			if (!seen.insert(path).second)
			{
				return Fail(RefactoringErrorCode::ConflictingEdits, "Duplicate document in edit plan");
			}

			if (planned.options.has_value() != document->options.has_value())
			{
				return Fail(RefactoringErrorCode::StaleSnapshot, "Parser configuration changed: " + path);
			}

			if (planned.options)
			{
				const auto& expected = *planned.options;
				const auto& actual   = *document->options;
				if (expected.standard != actual.standard || expected.Macros() != actual.Macros() ||
					(expected.type_names ? expected.type_names->Fingerprint() : 0) !=
					(actual.type_names ? actual.type_names->Fingerprint() : 0))
				{
					return Fail(RefactoringErrorCode::StaleSnapshot, "Parser configuration changed: " + path);
				}
			}

			std::vector<const SourceEdit*> edits;
			for (const auto& edit : planned.edits)
			{
				if (edit.offset > document->text->size() || edit.length > document->text->size() - edit.offset ||
					edit.expected.size() != edit.length ||
					document->text->compare(edit.offset, edit.length, edit.expected) != 0)
				{
					return Fail(RefactoringErrorCode::InvalidEdit, "Edit range or expected content is invalid");
				}

				edits.push_back(&edit);
			}

			std::ranges::sort(edits, {},[](const SourceEdit* edit)
				{
					return edit->offset;
				});
			std::string source;
			std::size_t cursor = 0;
			for (std::size_t i = 0; i < edits.size(); ++i)
			{
				if (stop.stop_requested())
				{
					return std::unexpected(Cancelled());
				}

				const auto& edit = *edits[i];
				if (edit.offset < cursor ||(i != 0 && edit.offset == edits[i - 1]->offset))
				{
					return Fail(RefactoringErrorCode::ConflictingEdits, "Overlapping edits in edit plan");
				}

				source.append(*document->text, cursor, edit.offset - cursor);
				source += edit.replacement;
				cursor = edit.offset + edit.length;
			}

			source.append(*document->text, cursor, std::string::npos);
			result.emplace_back(planned.document, planned.path, std::move(source));
		}

		return result;
	}
} // namespace heimdall
