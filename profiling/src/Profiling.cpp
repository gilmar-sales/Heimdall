#include <Heimdall/Profiling.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <Heimdall/Trace.hpp>

#ifdef HEIMDALL_PROFILING

	#include <perfetto.h>

PERFETTO_DEFINE_CATEGORIES(
	perfetto::Category("startup").SetDescription("Process start, configuration and compile database"),
	perfetto::Category("config").SetDescription("Rule and project configuration"),
	perfetto::Category("compiler").SetDescription("Compiler probes for builtin macros and system includes"),
	perfetto::Category("macro").SetDescription("Macro environment preparation"),
	perfetto::Category("include").SetDescription("Include resolution and header effects"),
	perfetto::Category("parse").SetDescription("Lexing and parsing"),
	perfetto::Category("index").SetDescription("Workspace symbol index and shards"),
	perfetto::Category("semantic").SetDescription("Semantic analysis"),
	perfetto::Category("lsp").SetDescription("Language server requests and lifecycle"),
	perfetto::Category("cli").SetDescription("Command line pipeline"),
	perfetto::Category("header").SetDescription("Recursive header scans"),
	perfetto::Category("macro.detail").SetDescription("Per-condition macro processing"),
	perfetto::Category("include.detail").SetDescription("Per-include resolution"),
	perfetto::Category("cache").SetDescription("Cache lookups, updates and outcomes"),
	perfetto::Category("cache.detail").SetDescription("Cached effect validation and replay"),
	perfetto::Category("io").SetDescription("Header file and metadata access"),
	perfetto::Category("lock.wait").SetDescription("Time acquiring instrumented mutexes"),
	perfetto::Category("lock.hold").SetDescription("Time owning instrumented mutexes"),
	perfetto::Category("scheduler").SetDescription("Shared worker pool queue and execution"));

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

// Perfetto needs the category as a literal at each call site, so every operation expands
// this list into a switch over trace::Category.
	#define HEIMDALL_TRACE_CATEGORIES(X)                                                           \
		X(Startup, "startup")                                                                      \
		X(Config, "config")                                                                        \
		X(Compiler, "compiler")                                                                    \
		X(Macro, "macro")                                                                          \
		X(Include, "include")                                                                      \
		X(Parse, "parse")                                                                          \
		X(Index, "index")                                                                          \
		X(Semantic, "semantic")                                                                    \
		X(Lsp, "lsp")                                                                              \
		X(Cli, "cli") \
		X(Header, "header") \
		X(MacroDetail, "macro.detail") \
		X(IncludeDetail, "include.detail") \
		X(Cache, "cache") \
		X(CacheDetail, "cache.detail") \
		X(Io, "io") \
		X(LockWait, "lock.wait") \
		X(LockHold, "lock.hold") \
		X(Scheduler, "scheduler")

#endif

namespace heimdall::profiling
{
	namespace
	{
		constexpr std::size_t kDefaultBufferMegabytes = 256;
		constexpr std::size_t kKilobytesPerMegabyte   = 1024;

		std::atomic<bool> gRunning {false};

		constexpr std::array CATEGORY_NAMES {
			"startup", "config", "compiler", "macro", "include", "parse", "index", "semantic",
			"lsp", "cli", "header", "macro.detail", "include.detail", "cache", "cache.detail",
			"io", "lock.wait", "lock.hold", "scheduler"
		};
		static_assert(CATEGORY_NAMES.size() == static_cast<std::size_t>(trace::Category::Count));
		constexpr std::uint64_t ALL_CATEGORIES = (std::uint64_t {1} << CATEGORY_NAMES.size()) - 1;
		constexpr std::uint64_t OVERVIEW_CATEGORIES =
			((std::uint64_t {1} << static_cast<unsigned>(trace::Category::Header)) - 1) |
			(std::uint64_t {1} << static_cast<unsigned>(trace::Category::Scheduler));

		constexpr std::uint64_t Bit(trace::Category category)
		{
			return std::uint64_t {1} << static_cast<unsigned>(category);
		}

		std::string_view Trim(std::string_view value)
		{
			constexpr std::string_view WHITESPACE = " \t\r\n";
			const auto first                      = value.find_first_not_of(WHITESPACE);
			if (first == std::string_view::npos)
			{
				return {};
			}

			return value.substr(first, value.find_last_not_of(WHITESPACE) - first + 1);
		}

#ifdef HEIMDALL_PROFILING

		constexpr std::uint32_t SHARED_MEMORY_KILOBYTES        = 32 * 1024;
		constexpr std::uint32_t FILE_WRITE_PERIOD_MILLISECONDS = 100;
		constexpr std::uint32_t FLUSH_TIMEOUT_MILLISECONDS     = 10000;

		struct ScopeRecord
		{
			trace::Category category;
			bool recorded;
		};

		struct ThreadState
		{
			std::mutex mutex;
			perfetto::ThreadTrack track = perfetto::ThreadTrack::Current();
			std::vector<ScopeRecord> scopes;
		};

		std::mutex gThreadsMutex;
		std::vector<std::weak_ptr<ThreadState>> gThreads;
		std::atomic<bool> gRecording {false};
		std::atomic<std::uint64_t> gEnabledCategories {OVERVIEW_CATEGORIES};

		bool Enabled(trace::Category category) noexcept
		{
			return gRecording.load(std::memory_order_acquire) &&
				CategoryEnabled(gEnabledCategories.load(std::memory_order_relaxed), category);
		}

		ThreadState& CurrentThread()
		{
			thread_local auto state = []
			{
				auto result = std::make_shared<ThreadState>();
				const std::lock_guard lock(gThreadsMutex);
				gThreads.emplace_back(result);
				return result;
			}
			();
			return *state;
		}

		void EmitBegin(trace::Category category, const char* name, std::string_view detail,
			const perfetto::ThreadTrack& track) noexcept
		{
			switch (category)
			{
	#define HEIMDALL_CASE(id, text)                                                                \
		case trace::Category::id:                                                                  \
			if (detail.empty())                                                                    \
			{                                                                                      \
				TRACE_EVENT_BEGIN(text, perfetto::StaticString {name}, track);                     \
			}                                                                                      \
			else                                                                                   \
			{                                                                                      \
				TRACE_EVENT_BEGIN(text, perfetto::StaticString {name}, track, "detail",            \
					perfetto::DynamicString {detail.data(), detail.size()});                       \
			}                                                                                      \
			break;
				HEIMDALL_TRACE_CATEGORIES(HEIMDALL_CASE)
	#undef HEIMDALL_CASE
			case trace::Category::Count:
					break;
			}
		}

		void EmitEnd(trace::Category category, const perfetto::ThreadTrack& track,
			bool cutoff) noexcept
		{
			switch (category)
			{
	#define HEIMDALL_CASE(id, text)                                                                \
		case trace::Category::id:                                                                  \
			TRACE_EVENT_END(text, track, "capture_cutoff", cutoff);                                \
			break;
				HEIMDALL_TRACE_CATEGORIES(HEIMDALL_CASE)
	#undef HEIMDALL_CASE
			case trace::Category::Count:
					break;
			}
		}

		void Begin(trace::Category category, const char* name, std::string_view detail) noexcept
		{
			auto& state = CurrentThread();
			const std::lock_guard lock(state.mutex);
			const bool recorded = gRecording.load(std::memory_order_acquire);
			state.scopes.push_back({category, recorded});
			if (recorded)
			{
				EmitBegin(category, name, detail, state.track);
			}
		}

		void End(trace::Category) noexcept
		{
			auto& state = CurrentThread();
			const std::lock_guard lock(state.mutex);
			if (state.scopes.empty())
			{
				return;
			}

			const auto scope = state.scopes.back();
			state.scopes.pop_back();
			if (scope.recorded)
			{
				EmitEnd(scope.category, state.track, false);
			}
		}

		void CloseActiveScopes()
		{
			gRecording.store(false, std::memory_order_release);
			const std::lock_guard threadsLock(gThreadsMutex);
			std::erase_if(gThreads,[](const auto& entry)
				{
					return entry.expired();
				});
			for (const auto& entry : gThreads)
			{
				if (const auto state = entry.lock())
				{
					const std::lock_guard lock(state->mutex);
					for (auto scope = state->scopes.rbegin(); scope != state->scopes.rend(); ++scope)
					{
						if (scope->recorded)
						{
							EmitEnd(scope->category, state->track, true);
							scope->recorded = false;
						}
					}
				}
			}
		}

		void Instant(trace::Category category, const char* name, std::string_view detail) noexcept
		{
			auto& state = CurrentThread();
			const std::lock_guard lock(state.mutex);
			if (!gRecording.load(std::memory_order_acquire))
			{
				return;
			}

			switch (category)
			{
	#define HEIMDALL_CASE(id, text)                                                                \
		case trace::Category::id:                                                                  \
			if (detail.empty())                                                                    \
			{                                                                                      \
				TRACE_EVENT_INSTANT(text, perfetto::StaticString {name});                          \
			}                                                                                      \
			else                                                                                   \
			{                                                                                      \
				TRACE_EVENT_INSTANT(text, perfetto::StaticString {name}, "detail",                 \
					perfetto::DynamicString {detail.data(), detail.size()});                       \
			}                                                                                      \
			break;
				HEIMDALL_TRACE_CATEGORIES(HEIMDALL_CASE)
	#undef HEIMDALL_CASE
			case trace::Category::Count:
					break;
			}
		}

		void Counter(trace::Category category, const char* name, std::int64_t value) noexcept
		{
			auto& state = CurrentThread();
			const std::lock_guard lock(state.mutex);
			if (!gRecording.load(std::memory_order_acquire))
			{
				return;
			}

			switch (category)
			{
	#define HEIMDALL_CASE(id, text)                                                                \
		case trace::Category::id:                                                                  \
			TRACE_COUNTER(text, perfetto::CounterTrack(perfetto::StaticString {name}), value);                              \
			break;
				HEIMDALL_TRACE_CATEGORIES(HEIMDALL_CASE)
	#undef HEIMDALL_CASE
			case trace::Category::Count:
					break;
			}
		}

		void NameThread(const char* name) noexcept
		{
			auto& state = CurrentThread();
			const std::lock_guard lock(state.mutex);
			if (!gRecording.load(std::memory_order_acquire))
			{
				return;
			}

			const auto& track = state.track;
			auto descriptor   = track.Serialize();
			descriptor.mutable_thread()->set_thread_name(name);
			perfetto::TrackEvent::SetTrackDescriptor(track, descriptor);
		}

		constexpr trace::Sink kPerfettoSink {&Begin, &End, &Instant, &Counter, &NameThread, &Enabled};

		std::size_t BufferKilobytes()
		{
			std::size_t megabytes = kDefaultBufferMegabytes;
			if (const char * value = std::getenv("HEIMDALL_TRACE_BUFFER_MB"))
			{
				const std::string_view text(value);
				std::size_t requested = 0;
				const auto parsed     = std::from_chars(text.data(), text.data() + text.size(), requested);
				if (parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size() &&
					requested > 0 && requested <=
					std::numeric_limits<std::uint32_t>::max() / kKilobytesPerMegabyte)
				{
					megabytes = requested;
				}
			}

			return megabytes * kKilobytesPerMegabyte;
		}

#endif
	}

	struct Session::Impl
	{
#ifdef HEIMDALL_PROFILING
		std::unique_ptr<perfetto::TracingSession> session;
		std::filesystem::path spoolDirectory;
		std::filesystem::path spoolFile;
		std::atomic<bool> failed {false};
		bool preserveSpool = false;

		~Impl()
		{
			session.reset();
			if (preserveSpool)
			{
				return;
			}

			std::error_code error;
			if (!spoolFile.empty())
			{
				std::filesystem::remove(spoolFile, error);
			}

			if (!spoolDirectory.empty())
			{
				std::filesystem::remove(spoolDirectory, error);
			}
		}
#endif
	};

	bool CategoryEnabled(std::uint64_t mask, trace::Category category) noexcept
	{
		return category < trace::Category::Count && (mask & Bit(category)) != 0;
	}

	std::expected<std::uint64_t, std::string> ParseCategories(std::string_view selection)
	{
		selection = Trim(selection);
		if (selection.empty())
		{
			return OVERVIEW_CATEGORIES;
		}

		std::uint64_t mask = selection.starts_with('-') ? OVERVIEW_CATEGORIES : 0;
		while (!selection.empty())
		{
			const auto comma    = selection.find(',');
			auto token          = Trim(selection.substr(0, comma));
			const bool excluded = token.starts_with('-');
			if (excluded)
			{
				token.remove_prefix(1);
				token = Trim(token);
			}

			std::uint64_t bits = 0;
			if (token == "overview")
			{
				bits = OVERVIEW_CATEGORIES;
			}
			else if (token == "all" || token == "*")
			{
				bits = ALL_CATEGORIES;
			}
			else if (token == "macros")
			{
				bits = OVERVIEW_CATEGORIES | Bit(trace::Category::Header) | Bit(trace::Category::MacroDetail);
			}
			else if (token == "includes")
			{
				bits = OVERVIEW_CATEGORIES | Bit(trace::Category::Header) | Bit(trace::Category::IncludeDetail) |
					Bit(trace::Category::Cache) | Bit(trace::Category::CacheDetail) | Bit(trace::Category::Io);
			}
			else if (token == "contention")
			{
				bits = OVERVIEW_CATEGORIES | Bit(trace::Category::Cache) | Bit(trace::Category::Io) |
					Bit(trace::Category::LockWait) | Bit(trace::Category::LockHold);
			}
			else if (token != "none")
			{
				bool found = false;
				for (std::size_t index = 0; index < CATEGORY_NAMES.size(); ++index)
				{
					if (token == CATEGORY_NAMES[index])
					{
						bits  = std::uint64_t {1} << index;
						found = true;
						break;
					}
				}

				if (!found)
				{
					return std::unexpected("Unknown profiling category or preset: " + std::string(token));
				}
			}

			if (token == "none" && !excluded)
			{
				mask = 0;
			}
			else
			{
				mask = excluded ? mask & ~bits : mask | bits;
			}

			if (comma == std::string_view::npos)
			{
				break;
			}

			selection.remove_prefix(comma + 1);
			if (Trim(selection).empty())
			{
				return std::unexpected("Empty profiling category after comma");
			}
		}

		return mask;
	}

	bool Available() noexcept
	{
#ifdef HEIMDALL_PROFILING
		return true;
#else
		return false;
#endif
	}

	std::filesystem::path ResolveOutputPath(const std::filesystem::path& projectRoot)
	{
		std::error_code error;
		if (!projectRoot.empty())
		{
			return std::filesystem::absolute(projectRoot, error) / "heimdall.pftrace";
		}

		const auto current = std::filesystem::current_path(error);
		for (auto directory = current; !directory.empty(); directory = directory.parent_path())
		{
			if (std::filesystem::exists(directory / ".git", error) ||
				std::filesystem::exists(directory / ".heimdall.json", error) ||
				std::filesystem::exists(directory / "CMakeLists.txt", error))
			{
				return directory / "heimdall.pftrace";
			}

			if (directory == directory.parent_path())
			{
				break;
			}
		}

		return current / "heimdall.pftrace";
	}

	std::optional<std::filesystem::path> OutputPathFromEnvironment()
	{
		if (Available())
		{
			return ResolveOutputPath({});
		}

		const char* value = std::getenv(kTraceEnvironmentVariable);
		if (value == nullptr || *value == '\0')
		{
			return std::nullopt;
		}

		return ResolveOutputPath({});
	}

	void Session::SetProjectRoot(const std::filesystem::path& projectRoot)
	{
		mOutput = ResolveOutputPath(projectRoot);
	}

	Session::Session(std::filesystem::path output, std::unique_ptr<Impl> impl)
	: mOutput(std::move(output)), mImpl(std::move(impl)) {}

	std::unique_ptr<Session> Session::Start(std::filesystem::path output)
	{
		const auto* selection = std::getenv("HEIMDALL_TRACE_CATEGORIES");
		return Start(std::move(output), selection == nullptr ? std::string_view {}: selection);
	}

	std::unique_ptr<Session> Session::Start(
		std::filesystem::path output,
		std::string_view categorySelection)
	{
#ifdef HEIMDALL_PROFILING
		const auto categories = ParseCategories(categorySelection);
		if (!categories)
		{
			std::fprintf(stderr, "heimdall: %s\n", categories.error().c_str());
			return nullptr;
		}

		bool expected = false;
		if (!gRunning.compare_exchange_strong(expected, true))
		{
			return nullptr;
		}

		static std::atomic<bool> initialized {false};
		if (!initialized.exchange(true))
		{
			perfetto::TracingInitArgs args;
			args.backends |= perfetto::kInProcessBackend;
			args.shmem_size_hint_kb = SHARED_MEMORY_KILOBYTES;
			perfetto::Tracing::Initialize(args);
			perfetto::TrackEvent::Register();
		}

		auto impl = std::make_unique<Impl>();
		std::error_code error;
		const auto temporary = std::filesystem::temp_directory_path(error);
		if (error)
		{
			gRunning.store(false);
			return nullptr;
		}

		const auto stamp          = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto spoolDirectory = temporary /("heimdall-trace-" + std::to_string(stamp));
		if (!std::filesystem::create_directory(spoolDirectory, error))
		{
			gRunning.store(false);
			return nullptr;
		}

		impl->spoolDirectory = spoolDirectory;
		impl->spoolFile      = spoolDirectory / "capture.pftrace";

		perfetto::TraceConfig config;
		config.add_buffers()->set_size_kb(static_cast<std::uint32_t>(BufferKilobytes()));
		config.set_write_into_file(true);
		config.set_output_path(impl->spoolFile.string());
		config.set_file_write_period_ms(FILE_WRITE_PERIOD_MILLISECONDS);
		auto* source = config.add_data_sources()->mutable_config();
		source->set_name("track_event");
		perfetto::protos::gen::TrackEventConfig eventConfig;
		eventConfig.add_disabled_categories("*");
		for (std::size_t index = 0; index < CATEGORY_NAMES.size(); ++index)
		{
			if (CategoryEnabled(*categories, static_cast<trace::Category>(index)))
			{
				eventConfig.add_enabled_categories(CATEGORY_NAMES[index]);
			}
		}

		source->set_track_event_config_raw(eventConfig.SerializeAsString());

		impl->session = perfetto::Tracing::NewTrace();
		impl->session->SetOnErrorCallback([state = impl.get()](perfetto::TracingError)
			{
				state->failed.store(true, std::memory_order_release);
			});
		impl->session->Setup(config);
		impl->session->StartBlocking();
		if (impl->failed.load(std::memory_order_acquire))
		{
			impl.reset();
			gRunning.store(false);
			return nullptr;
		}

		gEnabledCategories.store(*categories, std::memory_order_relaxed);
		gRecording.store(true, std::memory_order_release);
		trace::InstallSink(&kPerfettoSink);
		return std::unique_ptr<Session>(new Session(std::move(output), std::move(impl)));
#else
		static_cast<void>(output);
		static_cast<void>(categorySelection);
		return nullptr;
#endif
	}

	bool Session::Stop()
	{
#ifdef HEIMDALL_PROFILING
		if (!mImpl ||!mImpl->session)
		{
			return false;
		}

		trace::InstallSink(nullptr);
		CloseActiveScopes();
		perfetto::TrackEvent::Flush();
		const bool flushed = mImpl->session->FlushBlocking(FLUSH_TIMEOUT_MILLISECONDS);
		mImpl->session->StopBlocking();
		const bool healthy = flushed && !mImpl->failed.load(std::memory_order_acquire);
		mImpl->session.reset();

		std::error_code error;
		if (mOutput.has_parent_path())
		{
			std::filesystem::create_directories(mOutput.parent_path(), error);
		}

		const bool copied = std::filesystem::copy_file(mImpl->spoolFile, mOutput,
			std::filesystem::copy_options::overwrite_existing, error);
		const bool written = copied && !error && std::filesystem::file_size(mOutput, error) > 0 && !error;
		if (!written ||!healthy)
		{
			mImpl->preserveSpool = true;
			std::fprintf(stderr, "heimdall: trace export incomplete; recording retained at %s\n",
				mImpl->spoolFile.string().c_str());
		}

		mImpl.reset();
		gRunning.store(false);
		return written && healthy;
#else
		return false;
#endif
	}

	Session::~Session()
	{
		Stop();
	}
}
