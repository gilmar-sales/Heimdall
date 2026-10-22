#include <chrono>
#include <future>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "IndexProgress.hpp"

TEST(IndexProgressSpec, BlockedPublisherDoesNotBlockWorkerUpdates)
{
	using namespace std::chrono_literals;
	using heimdall::lsp::IndexProgress;
	IndexProgress progress(1);
	progress.Update(0, "Reading: first.cpp");
	std::promise<void> entered;
	std::promise<void> release;
	const auto gate = release.get_future().share();
	IndexProgress::Snapshot published;
	std::jthread reporter(
		[&]
		{
			progress.Report([&](const auto& snapshot)
			{
				published = snapshot;
				entered.set_value();
				gate.wait();
			});
		});
	entered.get_future().wait();

	auto update = std::async(std::launch::async,[&]
		{
			progress.Update(0, "Parsing: second.cpp");
		});
	const auto status = update.wait_for(2s);
	release.set_value();
	update.get();
	reporter.join();

	EXPECT_EQ(status, std::future_status::ready);
	ASSERT_EQ(published.activities.size(), 1u);
	EXPECT_EQ(published.activities[0], "Reading: first.cpp");
	progress.Report([&](const auto& snapshot)
		{
			EXPECT_EQ(snapshot.activities[0], "Parsing: second.cpp");
			EXPECT_GT(snapshot.revision, published.revision);
		});
}

TEST(IndexProgressSpec, UpdatesCoalesceToTheLatestActivityPerSlot)
{
	heimdall::lsp::IndexProgress progress(2);

	progress.Update(0, "Reading");
	progress.Update(1, "Resolving macros and includes");
	progress.Update(0, "Parsing");
	progress.Update(0, {});

	progress.Report([](const auto& snapshot)
		{
			ASSERT_EQ(snapshot.activities.size(), 2u);
			EXPECT_TRUE(snapshot.activities[0].empty());
			EXPECT_EQ(snapshot.activities[1], "Resolving macros and includes");
			EXPECT_EQ(snapshot.revision, 4u);
		});
}
