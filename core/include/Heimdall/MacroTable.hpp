#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Heimdall/Lexer.hpp>
#include <Heimdall/Preprocessor.hpp>

namespace heimdall
{

	/// Where a macro definition came from.
	enum class MacroOrigin : std::uint8_t
	{
		/// Compiler predefined (`__cplusplus`, `__GNUC__`, ...).
		Builtin,
		/// `-D` / `-include` on the compile command.
		CommandLine,
		/// A `#define` in a source or header file.
		File
	};

	/// One significant token of a replacement list; `token` offsets index `MacroDef::body`.
	struct MacroBodyToken
	{
		Token token;
		bool spaceBefore = false;
	};

	/// A macro definition. Immutable once shared through `MacroPtr`.
	struct MacroDef
	{
		std::string name;
		/// Parameter names; a variadic macro ends with `__VA_ARGS__` (or the GNU named form).
		std::vector<std::string> params;
		/// Replacement list text without comments or line continuations.
		std::string body;
		std::vector<MacroBodyToken> tokens;
		bool functionLike  = false;
		bool variadic      = false;
		MacroOrigin origin = MacroOrigin::File;
		/// File and byte offset of the `#define`; empty/zero for builtins and `-D`.
		std::string file;
		std::uint32_t offset = 0;
		/// Hash of name, parameters and replacement list; equal definitions hash equally.
		std::uint64_t hash = 0;
	};

	using MacroPtr = std::shared_ptr<const MacroDef>;

	/// Watches the reads and writes made through one `MacroTable` object (snapshots made by
	/// `Freeze()` are not watched). Used to learn what a header reads from, and does to, the
	/// macro state, so its effect can be replayed without scanning it again.
	class MacroObserver
	{
	public:
		MacroObserver() = default;

		MacroObserver(const MacroObserver&) = delete;

		MacroObserver& operator= (const MacroObserver&) = delete;

		virtual~MacroObserver() = default;

		/// `name` was looked up; `found` is its live definition or null.
		virtual void Read(std::string_view name, const MacroDef* found) = 0;

		/// `macro` became the definition of its name.
		virtual void Defined(const MacroPtr& macro) = 0;

		/// `name` was removed.
		virtual void Undefined(std::string_view name) = 0;
	};

	/// Persistent macro scope. `Freeze()` is O(1) and the returned table shares all earlier
	/// definitions, so one snapshot per `#include`/offset costs only the delta.
	class MacroTable
	{
	public:
		/// Parses the text after `#define` (`NAME`, `NAME body`, `NAME(a, b) body`).
		[[nodiscard]] static std::expected<MacroDef, std::string>
		ParseDefinition(
			std::string_view directiveText);

		/// Parses a command line `-D` argument (`NAME`, `NAME=value`); `NAME` means `1`.
		[[nodiscard]] static std::expected<MacroDef, std::string>
		ParseCommandLine(
			std::string_view argument);

		/// Adapts the legacy object-like `name -> replacement text` map as builtin macros.
		[[nodiscard]] static MacroTable FromMap(const Preprocessor::MacroMap& map);

		/// Defines or redefines a macro.
		void Define(MacroDef definition);

		/// Defines a macro that is already shared, without copying it.
		void Define(MacroPtr definition);

		/// Reports every lookup and change of this table object to `observer` (may be null).
		void SetObserver(MacroObserver* observer) noexcept
		{
			mObserver = observer;
		}

		/// Like `Find`, but unobserved: for inspecting the table without recording a read.
		[[nodiscard]] const MacroDef* Peek(std::string_view name) const noexcept;

		/// Removes a macro; a no-op when it is not defined.
		void Undef(std::string_view name);

		/// The live definition of `name`, or null. Valid while this table or a copy lives.
		[[nodiscard]] const MacroDef* Find(std::string_view name) const noexcept;

		/// Shared pointer to the live definition of `name`, or null.
		[[nodiscard]] MacroPtr Lookup(std::string_view name) const noexcept;

		/// Freezes the current definitions into a shared layer and returns a snapshot of them.
		[[nodiscard]] MacroTable Freeze();

		/// Number of live macros.
		[[nodiscard]] std::size_t Size() const;

		/// Calls `visit` for every live macro, in unspecified order.
		void ForEach(const std::function<void(const MacroDef&) > & visit) const;

	private:
		using Entries = std::unordered_map<std::string, MacroPtr, TransparentStringHash,
			std::equal_to<>>;

		struct Layer
		{
			std::shared_ptr<const Layer> parent;
			Entries entries;
			std::size_t depth = 1;
		};

		static constexpr std::size_t kMaxLayerDepth = 32;

		void Flatten();

		// The stored pointer itself (no reference count traffic); null when undefined.
		[[nodiscard]] const MacroPtr* RawRef(std::string_view name) const noexcept;

		[[nodiscard]] MacroPtr Raw(std::string_view name) const noexcept;

		std::shared_ptr<const Layer> mBase;
		Entries mLocal;
		MacroObserver* mObserver = nullptr;
	};

} // namespace heimdall
