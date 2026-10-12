#pragma once

#include <Heimdall/CompileDatabase.hpp>
#include <Heimdall/MacroResolver.hpp>
#include <Heimdall/MacroScanner.hpp>
#include <Heimdall/MacroTable.hpp>
#include <Heimdall/SourceOverlay.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{

	inline constexpr std::size_t kBytesPerMebibyte = std::size_t {1} << 20;
	/// Include nesting GCC and Clang accept before giving up.
	inline constexpr std::size_t kDefaultMaxIncludeDepth = 200;
	/// Macros defined over the whole translation unit by default (the standard's minimum).
	inline constexpr std::size_t kDefaultMaxMacros = 65536;
	/// Largest header read by default: 16 MiB.
	inline constexpr std::size_t kDefaultMaxFileBytes = std::size_t { 16 } * kBytesPerMebibyte;

	/// Bounds and sources of a `MacroEnvironment` build.
	struct MacroEnvironmentOptions
	{
		/// Deepest `#include` nesting (GCC and Clang stop at 200).
		std::size_t maxIncludeDepth = kDefaultMaxIncludeDepth;
		/// Largest header read, in bytes; bigger ones count as unresolved.
		/// Distinct macro names defined across the translation unit; past it no more headers
		/// are entered and the environment is incomplete.
		std::size_t maxMacros = kDefaultMaxMacros;
		std::size_t maxFileBytes = kDefaultMaxFileBytes;
		/// Run the compiler (`-dM -E`) for builtin macros; otherwise use the built-in table.
		bool probeCompiler = true;
		/// Open buffers that win over disk contents; may be null.
		const SourceOverlay* overlay = nullptr;
	};

	/// The macros of one translation unit, as the compiler's preprocessor would see them:
	/// builtins, then `-D`/`-U`, then `-include` files, then the file with every `#include`
	/// entered in order. `At(offset)` answers "which macros are live here?" for the main file.
	class MacroEnvironment
	{
	public:
		/// Builds the environment of `file` (whose current `text` is given) under `command`.
		[[nodiscard]] static MacroEnvironment Build(
			const std::filesystem::path& file,
			std::string_view text,
			const CompileCommand* command,
			const MacroEnvironmentOptions& options = {},
			std::stop_token stop = {});

		/// Predefined macros of the compiler named by `command`. The compiler runs once per
		/// distinct driver and flag set; the result is cached for the process.
		[[nodiscard]] static MacroTable Builtins(const CompileCommand* command, bool probeCompiler);

		/// Macros live at `offset` of the main file (after the directive that ends before it).
		[[nodiscard]] MacroTable At(std::size_t offset) const;

		/// The definition of `name` live at `offset` of the main file, or null. Valid while this
		/// environment lives.
		[[nodiscard]] const MacroDef* Find(std::string_view name, std::size_t offset) const;

		/// False when no macro of this name is ever defined in the translation unit: a cheap
		/// test that spares most identifiers the position-dependent lookup.
		[[nodiscard]] bool MayDefine(std::string_view name) const noexcept
		{
			return mEverDefined.contains(name);
		}

		/// Macros at the end of the main file.
		[[nodiscard]] const MacroTable& Final() const noexcept
		{
			return mFinal;
		}

		/// What the main file's own scan found: active ranges and diagnostics.
		[[nodiscard]] const MacroScanResult& Main() const noexcept
		{
			return mMain;
		}

		/// Headers entered, in order of first entry; includes `-include` files.
		[[nodiscard]] const std::vector<std::filesystem::path>& Files() const noexcept
		{
			return mFiles;
		}

		/// True when `file` lies under a compiler system include directory.
		[[nodiscard]] bool IsSystemFile(const std::filesystem::path& file) const;

		/// True when every `#include` outside system headers was found and no limit was hit.
		[[nodiscard]] bool Complete() const noexcept
		{
			return mComplete;
		}

	private:
		[[nodiscard]] const MacroTable& Snapshot(std::size_t offset) const noexcept;

		struct Checkpoint
		{
			std::size_t offset = 0;
			MacroTable table;
		};

		class Builder;

		MacroTable mInitial;
		MacroTable mFinal;
		MacroScanResult mMain;
		std::vector<Checkpoint> mCheckpoints;
		std::vector<std::filesystem::path> mFiles;
		std::vector<std::filesystem::path> mSystemDirectories;
		Preprocessor::ErasedSet mEverDefined;
		bool mComplete = true;
	};

	/// Lets the parser ask a `MacroEnvironment` which macro a name is at a position.
	class MacroEnvironmentResolver final : public MacroResolver
	{
	public:
		explicit MacroEnvironmentResolver(std::shared_ptr<const MacroEnvironment> environment);

		[[nodiscard]] const MacroDef* Find(std::string_view name,
			std::size_t offset) const noexcept override;

		[[nodiscard]] std::uint64_t Fingerprint() const noexcept override
		{
			return mFingerprint;
		}

		[[nodiscard]] const PreprocessorResult* Preprocessing() const noexcept override
		{
			return &mPreprocessing;
		}

		[[nodiscard]] std::optional<MacroTable> TableAt(std::size_t offset) const override
		{
			return mEnvironment->At(offset);
		}

	private:
		std::shared_ptr<const MacroEnvironment> mEnvironment;
		std::uint64_t mFingerprint = 0;
		PreprocessorResult mPreprocessing;
	};

} // namespace heimdall
