#pragma once

#include <concepts>
#include <cstdint>
#include <string_view>
#include <utility>

namespace heimdall::trace
{
	/** Coarse load phases; a profiler maps each one to its own filterable category. */
	enum class Category : std::uint8_t
	{
		Startup,
		Config,
		Compiler,
		Macro,
		Include,
		Parse,
		Index,
		Semantic,
		Lsp,
		Cli
	};

	/**
	 * Receiver of trace events, installed by a profiler library. Every callback must be
	 * non-null, thread-safe and must not fail; `name` always has static storage duration.
	 */
	struct Sink
	{
		void(*begin)(Category category, const char* name, std::string_view detail) noexcept;
		void(*end)(Category category) noexcept;
		void(*instant)(Category category, const char* name, std::string_view detail) noexcept;
		void(*counter)(Category category, const char* name, std::int64_t value) noexcept;
		void(*nameThread)(const char* name) noexcept;
	};

	/** Installs `sink` (or removes it with nullptr); the sink must outlive its installation. */
	void InstallSink(const Sink* sink) noexcept;

	/** Returns the installed sink, or nullptr when nothing is collecting events. */
	[[nodiscard]] const Sink* ActiveSink() noexcept;

	/**
	 * Emits a begin event on construction and the matching end event on destruction. Without
	 * an installed sink it costs one atomic load, so call sites need no conditional builds.
	 */
	class Scope
	{
	public:
		Scope(Category category, const char* name) noexcept
		: mSink(ActiveSink()), mCategory(category)
		{
			if (mSink != nullptr)
			{
				mSink->begin(category, name, {});
			}
		}

		/** `detail` is only invoked while a sink is installed; it may return a string or view. */
		template <typename Detail>
		requires std::invocable<Detail>
		Scope(Category category, const char* name, Detail&& detail)
		: mSink(ActiveSink()), mCategory(category)
		{
			if (mSink != nullptr)
			{
				const auto&& text = std::forward<Detail>(detail)();
				mSink->begin(category, name, std::string_view(text));
			}
		}

		~Scope()
		{
			if (mSink != nullptr)
			{
				mSink->end(mCategory);
			}
		}

		Scope(const Scope&) = delete;

		Scope& operator= (const Scope&) = delete;

	private:
		const Sink* mSink;
		Category mCategory;
	};

	inline void Instant(
		Category category,
		const char* name,
		std::string_view detail = {}) noexcept
	{
		if (const Sink * sink = ActiveSink())
		{
			sink->instant(category, name, detail);
		}
	}

	inline void Counter(Category category, const char* name, std::int64_t value) noexcept
	{
		if (const Sink * sink = ActiveSink())
		{
			sink->counter(category, name, value);
		}
	}

	inline void NameThread(const char* name) noexcept
	{
		if (const Sink * sink = ActiveSink())
		{
			sink->nameThread(name);
		}
	}
}

#define HEIMDALL_TRACE_CONCAT_INNER(a, b) a##b
#define HEIMDALL_TRACE_CONCAT(a, b)       HEIMDALL_TRACE_CONCAT_INNER(a, b)

#ifdef HEIMDALL_PROFILING

/** Traces the enclosing scope; `name` must be a string literal. */
	#define HEIMDALL_TRACE_SCOPE(category, name)                                                   		const ::heimdall::trace::Scope HEIMDALL_TRACE_CONCAT(heimdallTrace, __LINE__)(             			::heimdall::trace::Category::category, name)

/** Like HEIMDALL_TRACE_SCOPE; `detail` is only evaluated while a sink is installed. */
	#define HEIMDALL_TRACE_SCOPE_DETAIL(category, name, detail)                                    		const ::heimdall::trace::Scope HEIMDALL_TRACE_CONCAT(heimdallTrace, __LINE__)(             			::heimdall::trace::Category::category, name,                                           			[&]                                                                                    			{                                                                                      				return detail;                                                                     			})

	#define HEIMDALL_TRACE_INSTANT(category, name)                                                 		::heimdall::trace::Instant(::heimdall::trace::Category::category, name)

	#define HEIMDALL_TRACE_COUNTER(category, name, value)                                          		::heimdall::trace::Counter(::heimdall::trace::Category::category, name, (value))

	#define HEIMDALL_TRACE_THREAD(name) ::heimdall::trace::NameThread(name)

#else

	#define HEIMDALL_TRACE_SCOPE(category, name)
	#define HEIMDALL_TRACE_SCOPE_DETAIL(category, name, detail)
	#define HEIMDALL_TRACE_INSTANT(category, name)
	#define HEIMDALL_TRACE_COUNTER(category, name, value)
	#define HEIMDALL_TRACE_THREAD(name)

#endif
