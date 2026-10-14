#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <Heimdall/IndexSnapshot.hpp>
#include <Heimdall/Refactoring.hpp>

namespace heimdall
{
	/** One file as the frontend currently holds it: open buffer text or the file on disk. */
	struct IndexedDocument
	{
		DocumentId document  = InvalidDocument;
		std::int64_t version = 0;
		std::shared_ptr<const std::string> text;
		std::optional<ParserOptions> options;
		/** Current configuration fingerprint, when supplied by the frontend. */
		std::optional<std::uint64_t> configHash;
	};

	/** What the frontend knows that the index cannot. */
	struct IndexRenameContext
	{
		/** True only when every project file has been indexed and none was skipped. */
		bool complete = false;
		/** Require compiler-search-resolved includes and all their dependencies in the snapshot. */
		bool requireExactIncludes = false;
		/** Frontend discovery/indexing evidence explaining incomplete coverage. */
		std::string coverageIssue;
		/** Current text and parser options of an indexed file, keyed like the shard path. */
		std::function<std::optional<IndexedDocument>(const std::string& path) > document;
		/** False for files the rename must never touch (outside the workspace). */
		std::function<bool(const std::string& path) > editable;
	};

	/** Exact selected spelling and canonical entity used by prepareRename and rename. */
	struct PreparedIndexRename
	{
		index::SymbolRef symbol;
		std::size_t offset = 0;
		std::size_t length = 0;
		std::string name;
		/** Entity hashes renamed together, including implicit virtual overrides. */
		std::vector<index::SymbolHash> family;
	};

	/**
	 * Rename of an entity declared or used in several files. It edits an occurrence only
	 * when the index resolved it to that exact entity, and it refuses (with the reason) when
	 * any occurrence of the same spelling anywhere in the project is unresolved, ambiguous,
	 * inside a preprocessor directive or unclassified, when the entity is an operator,
	 * or when re-indexing the edited files changes what any name refers to. Constructors
	 * select their class; supported virtual overrides are renamed as one complete family.
	 */
	class IndexRename
	{
	public:
		/** Resolve and validate the selected entity without choosing a replacement spelling. */
		[[nodiscard]] static std::expected<PreparedIndexRename, RefactoringError> Prepare(
			const index::IndexSnapshot& snapshot,
			std::string_view path,
			std::size_t offset,
			const IndexRenameContext& context,
			std::stop_token stop = {});

		/** True when the symbol at `offset` is declared or used in more than one file. */
		[[nodiscard]] static bool SpansFiles(
			const index::IndexSnapshot& snapshot,
			std::string_view path,
			std::size_t offset);

		[[nodiscard]] static std::expected<RefactoringPlan, RefactoringError> Rename(
			const index::IndexSnapshot& snapshot,
			std::string_view path,
			std::size_t offset,
			std::string_view newName,
			const IndexRenameContext& context,
			std::stop_token stop = {});

		/** Validate an indexed plan against current buffers, files and index generation, without writes. */
		[[nodiscard]] static std::expected<std::vector<RefactoredSource>, RefactoringError> Preview(
			const index::IndexSnapshot& current,
			const RefactoringPlan& plan,
			const IndexRenameContext& context,
			std::stop_token stop = {});
	};
} // namespace heimdall
