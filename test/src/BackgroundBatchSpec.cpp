#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BackgroundBatch.hpp"

using heimdall::lsp::RunBackgroundBatch;
using heimdall::lsp::ThreadPool;

TEST(BackgroundBatchSpec, SlowFileDoesNotPreventOtherSlotsFromAdvancing)
{
	constexpr std::size_t ITEM_COUNT = 32;
	ThreadPool pool(3);
	std::promise<void> release;
	const auto gate = release.get_future().share();
	std::promise<void> lastProcessed;
	auto last = lastProcessed.get_future();
	std::vector<std::atomic<int>> visits(ITEM_COUNT);
	std::vector<std::atomic<int>> activeSlots(2);
	std::atomic<bool> validSlot {true};

	auto batch = std::async(std::launch::async,[&]
		{
			RunBackgroundBatch(pool, ITEM_COUNT, 2, {},[&](std::size_t index, std::size_t slot)
			{
				if (slot >= 2)
				{
					validSlot.store(false);
				}
				else if (activeSlots[slot].fetch_add(1) != 0)
				{
					validSlot.store(false);
				}

				if (index == 0)
				{
					gate.wait();
				}

				++visits[index];
				if (slot < 2)
				{
					--activeSlots[slot];
				}

				if (index == ITEM_COUNT - 1)
				{
					lastProcessed.set_value();
				}
			});
		});
	const auto status = last.wait_for(std::chrono::seconds(2));
	release.set_value();
	batch.get();

	EXPECT_EQ(status, std::future_status::ready);
	EXPECT_TRUE(validSlot.load());
	for (const auto& visit : visits)
	{
		EXPECT_EQ(visit.load(), 1);
	}
}

TEST(BackgroundBatchSpec, InteractiveRequestRunsBetweenFilesOnOneWorker)
{
	ThreadPool pool(1);
	std::vector<int> order;

	RunBackgroundBatch(pool, 2, 1, {},[&](std::size_t index, std::size_t)
		{
			order.push_back(static_cast<int>(index));
			if (index == 0)
			{
				pool.Submit([&]
				{
					order.push_back(42);
				});
			}
		});
	pool.WaitIdle();

	EXPECT_EQ(order, (std::vector<int> {0, 42, 1}));
	EXPECT_EQ(pool.Statistics().submitted, 3u);
}

TEST(BackgroundBatchSpec, CancellationStopsSchedulingAndJoinsPendingTasks)
{
	ThreadPool pool(2);
	std::stop_source stop;
	std::atomic<int> processed {0};

	RunBackgroundBatch(pool, 100, 1, stop.get_token(),[&](std::size_t, std::size_t)
		{
			++processed;
			stop.request_stop();
		});

	EXPECT_EQ(processed.load(), 1);
}

TEST(BackgroundBatchSpec, TaskFailurePropagatesWithoutStrandingTheDispatcher)
{
	ThreadPool pool(2);
	std::atomic<int> later {0};

	EXPECT_THROW(RunBackgroundBatch(pool, 4, 1, {},[](std::size_t, std::size_t)
		{
			throw std::runtime_error("file processing failed");
			}), std::runtime_error);
	pool.Submit([&]
		{
			++later;
		});
	pool.WaitIdle();

	EXPECT_EQ(later.load(), 1);
}

TEST(BackgroundBatchSpec, PoolShutdownReleasesQueuedBatchTasks)
{
	ThreadPool pool(1);
	std::promise<void> entered;
	std::promise<void> release;
	const auto gate = release.get_future().share();
	pool.Submit([&]
		{
			entered.set_value();
			gate.wait();
		});
	entered.get_future().wait();
	auto batch = std::async(std::launch::async,[&]
		{
			RunBackgroundBatch(pool, 8, 2, {},[](std::size_t, std::size_t) {});
		});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (pool.Statistics().queued == 0 && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}

	const bool queued = pool.Statistics().queued != 0;
	auto shutdown = std::async(std::launch::async,[&]
		{
			pool.Shutdown();
		});
	const auto status = batch.wait_for(std::chrono::seconds(2));
	release.set_value();
	shutdown.get();

	EXPECT_TRUE(queued);
	EXPECT_LE(pool.Statistics().peakQueued, 2u);
	EXPECT_EQ(status, std::future_status::ready);
	EXPECT_THROW(batch.get(), std::runtime_error);
}

TEST(BackgroundBatchSpec, EmptyAndAlreadyCancelledBatchesDoNotSubmitWork)
{
	ThreadPool pool(1);
	std::stop_source stop;
	stop.request_stop();
	std::atomic<int> processed {0};
	const auto process = [&](std::size_t, std::size_t)
	{
		++processed;
	};

	RunBackgroundBatch(pool, 0, 2, {}, process);
	RunBackgroundBatch(pool, 4, 2, stop.get_token(), process);

	EXPECT_EQ(processed.load(), 0);
	EXPECT_EQ(pool.Statistics().submitted, 0u);
}
