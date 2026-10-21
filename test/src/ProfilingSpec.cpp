#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

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
    auto session = heimdall::profiling::Session::Start(output);
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
    auto first = heimdall::profiling::Session::Start(directory / "first.pftrace");
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
    const auto initial = directory / "initial.pftrace";
    auto session = heimdall::profiling::Session::Start(initial);
    ASSERT_NE(session, nullptr);

    session->SetProjectRoot(directory);
    heimdall::trace::Counter(heimdall::trace::Category::Index, "files", 1);
    const bool written = session->Stop();

    EXPECT_TRUE(written);
    EXPECT_EQ(session->Output(), directory / "heimdall.pftrace");
    EXPECT_TRUE(std::filesystem::exists(session->Output()));
    EXPECT_FALSE(std::filesystem::exists(initial));
}
