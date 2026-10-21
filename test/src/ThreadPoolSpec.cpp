#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <vector>

#include <gtest/gtest.h>

#include "ThreadPool.hpp"

using heimdall::lsp::ThreadPool;

TEST(ThreadPoolSpec, SerialLanePreservesOrderWithoutBlockingAnotherDocument)
{
	ThreadPool pool(2);
	std::promise<void> release;
	auto gate = release.get_future().share();
	std::promise<void> entered;
	std::promise<void> independent;
	std::vector<int> order;

	pool.SubmitSerial("a.cpp",
		[&]
		{
			entered.set_value();
			gate.wait();
			order.push_back(1);
		});
	entered.get_future().wait();
	pool.SubmitSerial("a.cpp",[&]
		{
			order.push_back(2);
		});
	pool.SubmitSerial("b.cpp",[&]
		{
			independent.set_value();
		});
	const auto status = independent.get_future().wait_for(std::chrono::seconds(2));
	release.set_value();
	pool.WaitIdle();

	EXPECT_EQ(status, std::future_status::ready);
	EXPECT_EQ(order, (std::vector<int> {1, 2}));
}

TEST(ThreadPoolSpec, ThrowingSerialTaskDoesNotStrandLaterTasks)
{
	ThreadPool pool(2);
	std::atomic<int> done = 0;

	pool.SubmitSerial("a.cpp",[]
		{
			throw std::runtime_error("boom");
		});
	pool.SubmitSerial("a.cpp",[&]
		{
			++done;
		});
	pool.WaitIdle();

	EXPECT_EQ(done.load(), 1);
}

TEST(ThreadPoolSpec, CancelledQueuedTaskReleasesItsSnapshotBeforeWorkersAreAvailable)
{
	ThreadPool pool(1);
	std::promise<void> release;
	auto gate = release.get_future().share();
	std::promise<void> entered;
	pool.Submit(
		[&]
		{
			entered.set_value();
			gate.wait();
		});
	entered.get_future().wait();
	auto snapshot           = std::make_shared<int>(42);
	std::weak_ptr<int> weak = snapshot;
	std::stop_source stop;
	std::atomic<int> ran       = 0;
	std::atomic<int> cancelled = 0;

	pool.SubmitCancellable([snapshot, &ran]
		{
			++ran;
			}, stop.get_token(),[&]
		{
			++cancelled;
		});
	snapshot.reset();
	stop.request_stop();
	const bool released = weak.expired();
	release.set_value();
	pool.WaitIdle();

	EXPECT_TRUE(released);
	EXPECT_EQ(ran.load(), 0);
	EXPECT_EQ(cancelled.load(), 1);
}

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
	pool.Submit(
		[&]
		{
			const std::lock_guard<std::mutex> lock(mu);
			order.push_back(1);
		},
		ThreadPool::Priority::Background);
	pool.Submit(
		[&]
		{
			const std::lock_guard<std::mutex> lock(mu);
			order.push_back(2);
		},
		ThreadPool::Priority::Interactive);
	release.set_value();
	pool.WaitIdle();
	ASSERT_EQ(order.size(), 2u);
	EXPECT_EQ(order[0], 2);
	EXPECT_EQ(order[1], 1);
}

TEST(ThreadPoolSpec, BackgroundWorkLeavesCapacityForInteractiveRequests)
{
	ThreadPool pool(2);
	std::promise<void> release;
	const auto gate = release.get_future().share();
	std::promise<void> firstStarted;
	std::promise<void> secondStarted;
	auto second = secondStarted.get_future();
	std::promise<void> interactiveDone;
	auto interactive = interactiveDone.get_future();

	pool.Submit([&]
		{
			firstStarted.set_value();
			gate.wait();
			}, ThreadPool::Priority::Background);
	firstStarted.get_future().wait();
	pool.Submit([&]
		{
			secondStarted.set_value();
			gate.wait();
			}, ThreadPool::Priority::Background);
	const auto secondStatus = second.wait_for(std::chrono::milliseconds(100));
	pool.Submit([&]
		{
			interactiveDone.set_value();
		});
	const auto interactiveStatus = interactive.wait_for(std::chrono::seconds(2));
	release.set_value();
	pool.WaitIdle();

	EXPECT_EQ(secondStatus, std::future_status::timeout);
	EXPECT_EQ(interactiveStatus, std::future_status::ready);
}

TEST(ThreadPoolSpec, SinglePoolDrainsDocumentParseAndRequestContinuations)
{
	ThreadPool pool(1);
	std::vector<int> phases;

	pool.SubmitSerial("document.cpp",[&]
		{
			phases.push_back(1);
			pool.Submit([&]
			{
				phases.push_back(2);
				pool.Submit([&]
				{
					phases.push_back(3);
				});
			});
		});
	pool.WaitIdle();

	EXPECT_EQ(phases, (std::vector<int> {1, 2, 3}));
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

TEST(ThreadPoolSpec, RecordsLaneQueueTimeAndFailuresSeparatelyFromExecution)
{
	ThreadPool pool(1);
	pool.SubmitSerial("file",[] {});
	pool.SubmitSerial("file",[]
		{
			throw std::runtime_error("failure");
		});
	pool.Submit([] {});

	pool.WaitIdle();
	const auto metrics = pool.Statistics();

	EXPECT_EQ(metrics.submitted, 3u);
	EXPECT_EQ(metrics.completed, 3u);
	EXPECT_EQ(metrics.failed, 1u);
	EXPECT_EQ(metrics.queued, 0u);
	EXPECT_EQ(metrics.active, 0u);
	EXPECT_GT(metrics.peakQueued, 0u);
	EXPECT_GT(metrics.queueNs, 0u);
	EXPECT_GT(metrics.executionNs, 0u);
}

TEST(ThreadPoolSpec, WorkspaceBarrierPinsAllEarlierDocumentsBeforeLaterChanges)
{
	ThreadPool pool(3);
	std::promise<void> release;
	const auto gate                    = release.get_future().share();
	std::atomic<int> completed         = 0;
	std::atomic<bool> pinned           = false;
	std::atomic<bool> laterObservedPin = false;
	pool.SubmitSerial("first",
		[&, gate]
		{
			gate.wait();
			++completed;
		});
	pool.SubmitSerial("second",[&]
		{
			++completed;
		});
	pool.SubmitBarrier(
		[&]
		{
			EXPECT_EQ(completed.load(), 2);
			pinned = true;
		});
	pool.SubmitSerial("third",[&]
		{
			laterObservedPin = pinned.load();
		});

	release.set_value();
	pool.WaitIdle();

	EXPECT_TRUE(laterObservedPin.load());
	EXPECT_EQ(pool.Statistics().completed, 4u);
}

TEST(ThreadPoolSpec, ThrowingWorkspaceBarrierReleasesLaterLanes)
{
	ThreadPool pool(2);
	std::atomic<int> completed = 0;
	pool.SubmitBarrier([]
		{
			throw std::runtime_error("failed pin");
		});
	pool.SubmitSerial("first",[&]
		{
			++completed;
		});
	pool.SubmitBarrier([&]
		{
			++completed;
		});
	pool.SubmitSerial("second",[&]
		{
			++completed;
		});

	pool.WaitIdle();

	EXPECT_EQ(completed.load(), 3);
	EXPECT_EQ(pool.Statistics().failed, 1u);
}
