#pragma once

#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <Heimdall/Workspace.hpp>

namespace heimdall
{
	enum class RefactoringErrorCode
	{
		InvalidSelection,
		InvalidName,
		UnsupportedSymbol,
		IncompleteAnalysis,
		MacroContext,
		NameCollision,
		StaleSnapshot,
		ConflictingEdits,
		InvalidEdit,
		SemanticChange,
		Cancelled
	};

	struct RefactoringError
	{
		RefactoringErrorCode code;
		std::string message;
	};

	struct SourceEdit
	{
		std::size_t offset = 0;
		std::size_t length = 0;
		std::string expected;
		std::string replacement;
	};

	// Pins the complete original content, not just an mtime or a hash. This
	// also makes preview generation independent of mutable frontend buffers.
	struct DocumentEdits
	{
		DocumentId document = InvalidDocument;
		std::filesystem::path path;
		std::int64_t version = 0;
		std::shared_ptr<const std::string> original;
		std::vector<SourceEdit> edits;
		std::optional<ParserOptions> options;
	};

	struct RefactoringPlan
	{
		std::string title;
		std::uint64_t revision = 0;
		std::vector<DocumentEdits> documents;
	};

	struct RefactoredSource
	{
		DocumentId document = InvalidDocument;
		std::filesystem::path path;
		std::string source;
	};

	struct SymbolOccurrences
	{
		SymbolId symbol    = kNone;
		std::size_t offset = 0;
		std::size_t length = 0;
		std::string name;
		std::vector<std::uint32_t> tokens;
	};

	struct DocumentOccurrences
	{
		DocumentId document = InvalidDocument;
		std::vector<std::uint32_t> tokens;
	};

	// No filesystem writes. All files are validated before any preview is
	// returned. Frontends are responsible for versioned/transactional apply.
	std::expected<std::vector<RefactoredSource>, RefactoringError> PreviewRefactoring(
		const AnalysisSnapshot& current,
		const RefactoringPlan& plan,
		std::stop_token stop = {});

	class RefactoringService
	{
	public:
		static std::expected<SymbolOccurrences, RefactoringError> LocalReferences(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			bool include_declaration = true,
			std::stop_token stop = {});

		// Cross-file references for free functions and globals via the
		// workspace symbol index. Locals and fields keep LocalReferences.
		static std::expected<std::vector<DocumentOccurrences>, RefactoringError> WorkspaceReferences(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			bool include_declaration = true,
			std::stop_token stop = {});

		static std::expected<RefactoringPlan, RefactoringError> RenameLocal(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			std::string_view new_name,
			std::stop_token stop = {});

		// First supported subset: a complete integral/bool constant expression in a
		// standalone return statement. Other selections return a reason.
		static std::expected<RefactoringPlan, RefactoringError> ExtractVariable(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			std::size_t length,
			std::string_view new_name,
			std::stop_token stop = {});

		// First supported subset: an arithmetic literal read in an ordinary
		// function. Every identical literal in the function is replaced;
		// literals nested in lambdas veto (capture hazard).
		static std::expected<RefactoringPlan, RefactoringError> ExtractConstant(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			std::size_t length,
			std::string_view new_name,
			std::stop_token stop = {});

		// A complete integral literal return expression in a free function.
		// No inputs/outputs, RAII objects or transfers of control are moved.
		static std::expected<RefactoringPlan, RefactoringError> ExtractFunction(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			std::size_t length,
			std::string_view new_name,
			std::stop_token stop = {});

		// First supported subset: an integral local initialized by a same-type
		// literal, whose reads are complete standalone return expressions.
		static std::expected<RefactoringPlan, RefactoringError> InlineVariable(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			std::stop_token stop = {});

		// First supported subset: a call to a same-document free function with
		// a single pure `return` body over its own parameters, with pure
		// arguments. Parameters are substituted parenthesized.
		static std::expected<RefactoringPlan, RefactoringError> InlineFunction(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			std::stop_token stop = {});

		// First supported subset: a complete pure scalar expression in a
		// standalone return of a member function. Reads of the enclosing
		// class stay direct (instance method) or become parameters.
		static std::expected<RefactoringPlan, RefactoringError> ExtractMethod(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			std::size_t length,
			std::string_view new_name,
			std::stop_token stop = {});

		// First supported subset: free functions across the workspace.
		// Adding appends a defaulted trailing parameter (calls keep working);
		// removing drops one unused parameter and rewrites every call.
		// Overloads, methods, address-takers and used removals veto.
		static std::expected<RefactoringPlan, RefactoringError> AddParameter(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			std::string_view type,
			std::string_view name,
			std::string_view default_value,
			std::stop_token stop = {});

		static std::expected<RefactoringPlan, RefactoringError> RemoveParameter(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			std::string_view name,
			std::stop_token stop = {});

		// First supported subset: a free function used only in its own
		// document moves into an existing namespace; every call is rewritten
		// fully qualified. Overloads, external uses and member functions veto.
		static std::expected<RefactoringPlan, RefactoringError> MoveToNamespace(
			const AnalysisSnapshot& snapshot,
			DocumentId document,
			std::size_t offset,
			std::string_view target_namespace,
			std::stop_token stop = {});
	};
} // namespace heimdall
