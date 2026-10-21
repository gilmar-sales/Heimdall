#include "../../semantic/src/CompilerProbe.hpp"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

TEST(CompilerProbeSpec, ConcurrentQueriesExecuteOneProducer)
{
    static std::atomic<unsigned> counter {0};
    const auto path = std::filesystem::temp_directory_path() /
                      ("heimdall-probe-" + std::to_string(counter.fetch_add(1)) + ".txt");
    std::error_code error;
    std::filesystem::remove(path, error);
#if defined(_WIN32)
    const auto command = "(echo called)>>\"" + path.string() + "\" & echo result";
#else
    const auto command = "echo called >> '" + path.string() + "'; echo result";
#endif
    constexpr std::size_t kWorkers = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(kWorkers));
    std::array<heimdall::detail::CompilerProbeResult, kWorkers> results;
    std::vector<std::jthread> workers;

    for (std::size_t i = 0; i < kWorkers; ++i)
    {
        workers.emplace_back([&, i]
            {
                start.arrive_and_wait();
                results[i] = heimdall::detail::QueryCompiler("probe-fixture", command);
            });
    }

    workers.clear();
    std::ifstream input(path);
    const std::string calls(std::istreambuf_iterator<char>(input), {});

    for (const auto& result : results)
    {
        EXPECT_TRUE(result.completed);
        EXPECT_NE(result.output.find("result"), std::string::npos);
    }

    EXPECT_EQ(std::count(calls.begin(), calls.end(), '\n'), 1);
    input.close();
    std::filesystem::remove(path, error);
}

TEST(CompilerProbeSpec, FailedQueriesCanBeRetriedAfterTimeout)
{
    const std::string command = "echo timeout-retry-probe";

    const auto timedOut = heimdall::detail::QueryCompiler("probe-fixture", command, {},
        std::chrono::milliseconds(0));
    const auto retried = heimdall::detail::QueryCompiler("probe-fixture", command);

    EXPECT_FALSE(timedOut.completed);
    EXPECT_TRUE(timedOut.output.empty());
    EXPECT_TRUE(retried.completed);
    EXPECT_NE(retried.output.find("timeout-retry-probe"), std::string::npos);
}

TEST(CompilerProbeSpec, PreCancelledQueriesDoNotPopulateTheCache)
{
    std::stop_source stop;
    stop.request_stop();
    const std::string command = "echo cancelled-probe";

    const auto cancelled = heimdall::detail::QueryCompiler("probe-fixture", command, stop.get_token());
    const auto retried = heimdall::detail::QueryCompiler("probe-fixture", command);

    EXPECT_FALSE(cancelled.completed);
    EXPECT_TRUE(retried.completed);
}

TEST(CompilerProbeSpec, OutputWrittenJustBeforeExitIsNotLost)
{
    constexpr int kProbes = 50;
    int empty             = 0;

    for (int i = 0; i < kProbes; ++i)
    {
        const std::string marker  = "late-output-" + std::to_string(i);
        const auto result = heimdall::detail::QueryCompiler("probe-fixture", "echo " + marker);
        if (!result.completed || result.output.find(marker) == std::string::npos)
        {
            ++empty;
        }
    }

    EXPECT_EQ(empty, 0);
}
