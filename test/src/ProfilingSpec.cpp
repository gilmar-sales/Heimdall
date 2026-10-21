#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <Heimdall/Profiling.hpp>
#include <Heimdall/Trace.hpp>

namespace
{
	std::filesystem::path ScratchDirectory(const char* name)
	{
		const auto directory = std::filesystem::temp_directory_path() / name;
		std::error_code error;
		std::filesystem::remove_all(directory, error);
		std::filesystem::create_directories(directory, error);
		return directory;
	}
}

TEST(ProfilingSpec, DefaultCategoriesExcludeHighVolumeDetailsAndLocks)
{
	const auto categories = heimdall::profiling::ParseCategories({});

	ASSERT_TRUE(categories.has_value());
	EXPECT_TRUE(heimdall::profiling::CategoryEnabled(*categories, heimdall::trace::Category::Index));
	EXPECT_FALSE(heimdall::profiling::CategoryEnabled(*categories,
		heimdall::trace::Category::MacroDetail));
	EXPECT_FALSE(heimdall::profiling::CategoryEnabled(*categories, heimdall::trace::Category::Header));
	EXPECT_FALSE(heimdall::profiling::CategoryEnabled(*categories,
		heimdall::trace::Category::LockWait));
}

TEST(ProfilingSpec, CategorySelectionSupportsPresetsAndOrderedExclusions)
{
	using heimdall::profiling::CategoryEnabled;
	using heimdall::trace::Category;
	const auto categories = heimdall::profiling::ParseCategories(" contention, header, -io, -lock.hold ");

	ASSERT_TRUE(categories.has_value());
	EXPECT_TRUE(CategoryEnabled(*categories, Category::LockWait));
	EXPECT_TRUE(CategoryEnabled(*categories, Category::Cache));
	EXPECT_TRUE(CategoryEnabled(*categories, Category::Header));
	EXPECT_FALSE(CategoryEnabled(*categories, Category::Io));
	EXPECT_FALSE(CategoryEnabled(*categories, Category::LockHold));
	EXPECT_FALSE(CategoryEnabled(*categories, Category::Count));
}

TEST(ProfilingSpec, CategorySelectionRejectsUnknownAndEmptyTokens)
{
	EXPECT_FALSE(heimdall::profiling::ParseCategories("overveiw").has_value());
	EXPECT_FALSE(heimdall::profiling::ParseCategories("index,,parse").has_value());
	EXPECT_FALSE(heimdall::profiling::ParseCategories("index,").has_value());
}

TEST(ProfilingSpec, CategorySelectionCanDisableAndReenableIndividualCategories)
{
	using heimdall::profiling::CategoryEnabled;
	using heimdall::trace::Category;
	const auto categories = heimdall::profiling::ParseCategories("all,-header,-macro.detail,header");
	const auto none       = heimdall::profiling::ParseCategories("none");
	const auto reset      = heimdall::profiling::ParseCategories("index,none,header");

	ASSERT_TRUE(categories.has_value());
	ASSERT_TRUE(none.has_value());
	ASSERT_TRUE(reset.has_value());
	EXPECT_TRUE(CategoryEnabled(*categories, Category::Header));
	EXPECT_FALSE(CategoryEnabled(*categories, Category::MacroDetail));
	EXPECT_EQ(*none, 0);
	EXPECT_FALSE(CategoryEnabled(*reset, Category::Index));
	EXPECT_TRUE(CategoryEnabled(*reset, Category::Header));
}

TEST(ProfilingSpec, SessionFiltersCategoriesBeforeEvaluatingDetails)
{
	if (!heimdall::profiling::Available())
	{
		GTEST_SKIP() << "built without HEIMDALL_PROFILING";
	}

	const auto directory = ScratchDirectory("heimdall-profiling-filter");
	auto session         = heimdall::profiling::Session::Start(directory / "filtered.pftrace", "index");
	ASSERT_NE(session, nullptr);
	int evaluations = 0;

	{
		const heimdall::trace::Scope outer(heimdall::trace::Category::Index, "included");
		const heimdall::trace::Scope disabled(heimdall::trace::Category::Header, "excluded",[&]
			{
				++evaluations;
				return std::string_view("unused");
			});
	}

	EXPECT_EQ(evaluations, 0);
	EXPECT_TRUE(session->Stop());
}

TEST(ProfilingSpec, ExplicitWorkspaceRootDeterminesTraceDestination)
{
	const auto projectRoot = ScratchDirectory("heimdall-profiling-workspace");

	const auto resolved = heimdall::profiling::ResolveOutputPath(projectRoot);

	EXPECT_EQ(resolved, projectRoot / "heimdall.pftrace");
}

TEST(ProfilingSpec, DefaultDestinationUsesFixedProjectFile)
{
	const auto fromEmpty = heimdall::profiling::ResolveOutputPath({});

	EXPECT_TRUE(fromEmpty.is_absolute());
	EXPECT_EQ(fromEmpty.filename(), "heimdall.pftrace");
}

TEST(ProfilingSpec, WorkspaceRootDoesNotNeedProjectMarkers)
{
	const auto directory = ScratchDirectory("heimdall-profiling-dir");

	const auto resolved = heimdall::profiling::ResolveOutputPath(directory);

	EXPECT_EQ(resolved.parent_path(), directory);
	EXPECT_EQ(resolved.filename(), "heimdall.pftrace");
}

TEST(ProfilingSpec, ProfilingBuildAutomaticallySelectsProjectTraceDestination)
{
	if (!heimdall::profiling::Available())
	{
		GTEST_SKIP() << "built without HEIMDALL_PROFILING";
	}

	const auto output = heimdall::profiling::OutputPathFromEnvironment();

	ASSERT_TRUE(output.has_value());
	EXPECT_EQ(*output, heimdall::profiling::ResolveOutputPath({}));
}

TEST(ProfilingSpec, SessionIsUnavailableWithoutThePerfettoBackend)
{
	if (heimdall::profiling::Available())
	{
		GTEST_SKIP() << "built with HEIMDALL_PROFILING";
	}

	const auto session = heimdall::profiling::Session::Start("unused.pftrace");

	EXPECT_EQ(session, nullptr);
	EXPECT_EQ(heimdall::trace::ActiveSink(), nullptr);
}

TEST(ProfilingSpec, SessionWritesNonEmptyTraceFileAndRemovesTheSink)
{
	if (!heimdall::profiling::Available())
	{
		GTEST_SKIP() << "built without HEIMDALL_PROFILING";
	}

	const auto output = ScratchDirectory("heimdall-profiling-session") / "trace.pftrace";
	auto session      = heimdall::profiling::Session::Start(output);
	ASSERT_NE(session, nullptr);

	{
		const heimdall::trace::Scope scope(heimdall::trace::Category::Index, "traced",[]
			{
				return std::string_view("detail");
			});
		heimdall::trace::Counter(heimdall::trace::Category::Index, "files", 3);
	}

	const bool written = session->Stop();

	EXPECT_TRUE(written);
	EXPECT_EQ(heimdall::trace::ActiveSink(), nullptr);
	EXPECT_GT(std::filesystem::file_size(output), 0u);
}

TEST(ProfilingSpec, OnlyOneSessionRecordsAtATime)
{
	if (!heimdall::profiling::Available())
	{
		GTEST_SKIP() << "built without HEIMDALL_PROFILING";
	}

	const auto directory = ScratchDirectory("heimdall-profiling-exclusive");
	auto first           = heimdall::profiling::Session::Start(directory / "first.pftrace");
	ASSERT_NE(first, nullptr);

	const auto second = heimdall::profiling::Session::Start(directory / "second.pftrace");

	EXPECT_EQ(second, nullptr);
	EXPECT_TRUE(first->Stop());
}

TEST(ProfilingSpec, SessionWritesToWorkspaceProvidedAfterStartup)
{
	if (!heimdall::profiling::Available())
	{
		GTEST_SKIP() << "built without HEIMDALL_PROFILING";
	}

	const auto directory = ScratchDirectory("heimdall-profiling-open-project");
	const auto initial   = directory / "initial.pftrace";
	auto session         = heimdall::profiling::Session::Start(initial);
	ASSERT_NE(session, nullptr);

	session->SetProjectRoot(directory);
	heimdall::trace::Counter(heimdall::trace::Category::Index, "files", 1);
	const bool written = session->Stop();

	EXPECT_TRUE(written);
	EXPECT_EQ(session->Output(), directory / "heimdall.pftrace");
	EXPECT_TRUE(std::filesystem::exists(session->Output()));
	EXPECT_FALSE(std::filesystem::exists(initial));
}

TEST(ProfilingSpec, StopClosesActiveScopesWithoutLeakingEndsIntoTheNextSession)
{
	if (!heimdall::profiling::Available())
	{
		GTEST_SKIP() << "built without HEIMDALL_PROFILING";
	}

	const auto directory = ScratchDirectory("heimdall-profiling-cutoff");
	auto session         = heimdall::profiling::Session::Start(directory / "cutoff.pftrace");
	ASSERT_NE(session, nullptr);
	std::latch started(1);
	std::latch resume(1);
	std::jthread worker(
		[&]
		{
			const heimdall::trace::Scope outer(heimdall::trace::Category::Index, "active-outer");
			const heimdall::trace::Scope inner(heimdall::trace::Category::Parse, "active-inner");
			started.count_down();
			resume.wait();
			const heimdall::trace::Scope next(heimdall::trace::Category::Index, "next-session");
		});
	started.wait();

	const bool firstWritten = session->Stop();
	auto nextSession        = heimdall::profiling::Session::Start(directory / "next.pftrace");
	resume.count_down();
	worker.join();

	EXPECT_TRUE(firstWritten);
	ASSERT_NE(nextSession, nullptr);
	EXPECT_TRUE(nextSession->Stop());
	EXPECT_GT(std::filesystem::file_size(directory / "cutoff.pftrace"), 0u);
	EXPECT_GT(std::filesystem::file_size(directory / "next.pftrace"), 0u);
	std::ifstream cutoffTrace(directory / "cutoff.pftrace", std::ios::binary);
	const std::string cutoffData(
		(std::istreambuf_iterator<char>(cutoffTrace)),
		std::istreambuf_iterator<char>());
	EXPECT_NE(cutoffData.find("capture_cutoff"), std::string::npos);
	std::ifstream nextTrace(directory / "next.pftrace", std::ios::binary);
	const std::string nextData(
		(std::istreambuf_iterator<char>(nextTrace)),
		std::istreambuf_iterator<char>());
	EXPECT_EQ(nextData.find("active-outer"), std::string::npos);
	EXPECT_EQ(nextData.find("active-inner"), std::string::npos);
}

TEST(ProfilingSpec, ConcurrentBurstWritersProduceACompleteTrace)
{
	if (!heimdall::profiling::Available())
	{
		GTEST_SKIP() << "built without HEIMDALL_PROFILING";
	}

	constexpr std::size_t WRITER_COUNT      = 8;
	constexpr std::size_t SCOPES_PER_WRITER = 10000;
	const auto directory                    = ScratchDirectory("heimdall-profiling-burst");
	auto session                            = heimdall::profiling::Session::Start(directory / "burst.pftrace");
	ASSERT_NE(session, nullptr);
	std::latch ready(WRITER_COUNT);
	std::vector<std::jthread> workers;
	workers.reserve(WRITER_COUNT);

	for (std::size_t writer = 0; writer < WRITER_COUNT; ++writer)
	{
		workers.emplace_back([&]
			{
				ready.arrive_and_wait();
				for (std::size_t index = 0; index < SCOPES_PER_WRITER; ++index)
				{
					const heimdall::trace::Scope outer(heimdall::trace::Category::Index, "burst-outer");
					const heimdall::trace::Scope inner(heimdall::trace::Category::Parse, "burst-inner");
				}
			});
	}

	workers.clear();
	const bool written = session->Stop();

	EXPECT_TRUE(written);
	EXPECT_GT(std::filesystem::file_size(directory / "burst.pftrace"), 0u);
}

TEST(ProfilingSpec, SustainedRecordingStreamsToDiskBeforeStop)
{
	if (!heimdall::profiling::Available())
	{
		GTEST_SKIP() << "built without HEIMDALL_PROFILING";
	}

	constexpr std::size_t BATCH_COUNT      = 120;
	constexpr std::size_t EVENTS_PER_BATCH = 1000;
	constexpr std::size_t DETAIL_LENGTH    = 256;
	constexpr auto BATCH_INTERVAL          = std::chrono::milliseconds(10);
	const auto directory                   = ScratchDirectory("heimdall-profiling-streaming");
	const std::string detail(DETAIL_LENGTH, 'x');
	auto session = heimdall::profiling::Session::Start(directory / "streaming.pftrace");
	ASSERT_NE(session, nullptr);

	for (std::size_t batch = 0; batch < BATCH_COUNT; ++batch)
	{
		for (std::size_t index = 0; index < EVENTS_PER_BATCH; ++index)
		{
			heimdall::trace::Instant(heimdall::trace::Category::Index, "streamed", detail);
		}

		std::this_thread::sleep_for(BATCH_INTERVAL);
	}

	const bool written = session->Stop();

	EXPECT_TRUE(written);
	EXPECT_GT(std::filesystem::file_size(directory / "streaming.pftrace"),
		BATCH_COUNT * EVENTS_PER_BATCH * DETAIL_LENGTH);
}
