#include <Heimdall/Profiling.hpp>

#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

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
	perfetto::Category("cli").SetDescription("Command line pipeline"));

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
		X(Cli, "cli")

#endif

namespace heimdall::profiling
{
	namespace
	{
		constexpr std::size_t kDefaultBufferMegabytes = 256;
		constexpr std::size_t kKilobytesPerMegabyte   = 1024;

		std::atomic<bool> gRunning {false};

#ifdef HEIMDALL_PROFILING

		void Begin(trace::Category category, const char* name, std::string_view detail) noexcept
		{
			switch (category)
			{
	#define HEIMDALL_CASE(id, text)                                                                \
		case trace::Category::id:                                                                  \
			if (detail.empty())                                                                    \
			{                                                                                      \
				TRACE_EVENT_BEGIN(text, perfetto::StaticString {name});                            \
			}                                                                                      \
			else                                                                                   \
			{                                                                                      \
				TRACE_EVENT_BEGIN(text, perfetto::StaticString {name}, "detail",                   \
					perfetto::DynamicString {detail.data(), detail.size()});                       \
			}                                                                                      \
			break;
				HEIMDALL_TRACE_CATEGORIES(HEIMDALL_CASE)
	#undef HEIMDALL_CASE
			}
		}

		void End(trace::Category category) noexcept
		{
			switch (category)
			{
	#define HEIMDALL_CASE(id, text)                                                                \
		case trace::Category::id:                                                                  \
			TRACE_EVENT_END(text);                                                                 \
			break;
				HEIMDALL_TRACE_CATEGORIES(HEIMDALL_CASE)
	#undef HEIMDALL_CASE
			}
		}

		void Instant(trace::Category category, const char* name, std::string_view detail) noexcept
		{
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
			}
		}

		void Counter(trace::Category category, const char* name, std::int64_t value) noexcept
		{
			switch (category)
			{
	#define HEIMDALL_CASE(id, text)                                                                \
		case trace::Category::id:                                                                  \
			TRACE_COUNTER(text, perfetto::CounterTrack(perfetto::StaticString {name}), value);                              \
			break;
				HEIMDALL_TRACE_CATEGORIES(HEIMDALL_CASE)
	#undef HEIMDALL_CASE
			}
		}

		void NameThread(const char* name) noexcept
		{
			const auto track = perfetto::ThreadTrack::Current();
			auto descriptor  = track.Serialize();
			descriptor.mutable_thread()->set_thread_name(name);
			perfetto::TrackEvent::SetTrackDescriptor(track, descriptor);
		}

		constexpr trace::Sink kPerfettoSink {&Begin, &End, &Instant, &Counter, &NameThread};

		std::size_t BufferKilobytes()
		{
			std::size_t megabytes = kDefaultBufferMegabytes;
			if (const char * value = std::getenv("HEIMDALL_TRACE_BUFFER_MB"))
			{
				const std::string_view text(value);
				std::size_t requested = 0;
				const auto parsed     = std::from_chars(text.data(), text.data() + text.size(), requested);
				if (parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size() &&
					requested > 0)
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
#endif
	};

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
#ifdef HEIMDALL_PROFILING
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
			perfetto::Tracing::Initialize(args);
			perfetto::TrackEvent::Register();
		}

		perfetto::TraceConfig config;
		config.add_buffers()->set_size_kb(static_cast<std::uint32_t>(BufferKilobytes()));
		auto* source = config.add_data_sources()->mutable_config();
		source->set_name("track_event");

		auto impl     = std::make_unique<Impl>();
		impl->session = perfetto::Tracing::NewTrace();
		impl->session->Setup(config);
		impl->session->StartBlocking();
		trace::InstallSink(&kPerfettoSink);
		return std::unique_ptr<Session>(new Session(std::move(output), std::move(impl)));
#else
		static_cast<void>(output);
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
		perfetto::TrackEvent::Flush();
		mImpl->session->StopBlocking();
		const auto data = mImpl->session->ReadTraceBlocking();
		mImpl->session.reset();
		gRunning.store(false);

		std::error_code error;
		if (mOutput.has_parent_path())
		{
			std::filesystem::create_directories(mOutput.parent_path(), error);
		}

		std::ofstream file(mOutput, std::ios::out | std::ios::binary | std::ios::trunc);
		file.write(data.data(), static_cast<std::streamsize>(data.size()));
		return file.good() && !data.empty();
#else
		return false;
#endif
	}

	Session::~Session()
	{
		Stop();
	}
}
