#include <gtest/gtest.h>

#include "ThreadPool.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <vector>

using heimdall::lsp::ThreadPool;

TEST(ThreadPoolSpec, SurvivesThrowingTasksAndKeepsRunningLaterOnes)
{
    ThreadPool pool(2);
    std::atomic<int> done = 0;
    // More throwing tasks than workers: if an exception killed a worker, the
    // pool would run out of threads and the later tasks would never run.
    for (int i = 0; i < 20; ++i)
    {
        pool.Submit([]
            {
                throw std::runtime_error("boom");
        });
    }

    for (int i = 0; i < 20; ++i)
    {
        pool.Submit([&]
            {
                ++done;
        });
    }

    pool.WaitIdle();
    EXPECT_EQ(done.load(), 20);
}

TEST(ThreadPoolSpec, WaitIdleReturnsAfterManyRounds)
{
    ThreadPool pool(3);
    std::atomic<int> done = 0;
    for (int round = 0; round < 200; ++round)
    {
        for (int i = 0; i < 8; ++i)
        {
            pool.Submit([&]
                {
                    ++done;
            });
        }

        pool.WaitIdle();
    }

    EXPECT_EQ(done.load(), 1600);
}

TEST(ThreadPoolSpec, InteractiveRunsBeforeQueuedBackground)
{
    ThreadPool pool(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    pool.Submit([gate]
        {
            gate.wait();
    });

    std::mutex mu;
    std::vector<int> order;
    pool.Submit([&]
        {
            const std::lock_guard<std::mutex> lock(mu);
            order.push_back(1);
        }, ThreadPool::Priority::Background);
    pool.Submit([&]
        {
            const std::lock_guard<std::mutex> lock(mu);
            order.push_back(2);
        }, ThreadPool::Priority::Interactive);
    release.set_value();
    pool.WaitIdle();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 2);
    EXPECT_EQ(order[1], 1);
}

TEST(ThreadPoolSpec, ShutdownIsIdempotentAndIgnoresLaterSubmits)
{
    ThreadPool pool(2);
    pool.Shutdown();
    pool.Shutdown();
    std::atomic<int> done = 0;
    pool.Submit([&]
        {
            ++done;
    });
    EXPECT_EQ(done.load(), 0);
}
