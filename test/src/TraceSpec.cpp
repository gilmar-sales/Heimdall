#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <Heimdall/Trace.hpp>

namespace
{
	using heimdall::trace::Category;

	std::vector<std::string> gEvents;

	void Begin(Category, const char* name, std::string_view detail) noexcept
	{
		gEvents.push_back(std::string("begin:") + name + ":" + std::string(detail));
	}

	void End(Category) noexcept
	{
		gEvents.emplace_back("end");
	}

	void Instant(Category, const char* name, std::string_view) noexcept
	{
		gEvents.push_back(std::string("instant:") + name);
	}

	void Counter(Category, const char* name, std::int64_t value) noexcept
	{
		gEvents.push_back(std::string("counter:") + name + "=" + std::to_string(value));
	}

	void NameThread(const char* name) noexcept
	{
		gEvents.push_back(std::string("thread:") + name);
	}

	constexpr heimdall::trace::Sink kRecordingSink {&Begin, &End, &Instant, &Counter, &NameThread};

	bool OnlyIndex(Category category) noexcept
	{
		return category == Category::Index;
	}

	constexpr heimdall::trace::Sink kFilteredSink
	{
		&Begin, &End, &Instant, &Counter, &NameThread, &OnlyIndex
	};

	class TraceSpec : public testing::Test
	{
	protected:
		void SetUp() override
		{
			gEvents.clear();
			heimdall::trace::InstallSink(&kRecordingSink);
		}

		void TearDown() override
		{
			heimdall::trace::InstallSink(nullptr);
		}
	};
}

TEST_F(TraceSpec, ScopeEmitsBalancedBeginAndEndWithDetail)
{
	{
		const heimdall::trace::Scope outer(Category::Index, "outer",[]
			{
				return std::string_view("file.cpp");
			});
		const heimdall::trace::Scope inner(Category::Parse, "inner");
	}

	const std::vector<std::string> expected
	{
		"begin:outer:file.cpp", "begin:inner:", "end", "end"
	};
	EXPECT_EQ(gEvents, expected);
}

TEST_F(TraceSpec, InstantCounterAndThreadNameReachTheSink)
{
	heimdall::trace::Instant(Category::Lsp, "ready");
	heimdall::trace::Counter(Category::Index, "files", 7);
	heimdall::trace::NameThread("worker");

	const std::vector<std::string> expected
	{
		"instant:ready", "counter:files=7", "thread:worker"
	};
	EXPECT_EQ(gEvents, expected);
}

TEST_F(TraceSpec, EventsAreDroppedWhenNoSinkIsInstalled)
{
	heimdall::trace::InstallSink(nullptr);

	{
		const heimdall::trace::Scope scope(Category::Index, "ignored");
		heimdall::trace::Instant(Category::Lsp, "ignored");
		heimdall::trace::Counter(Category::Index, "ignored", 1);
		heimdall::trace::NameThread("ignored");
	}

	EXPECT_TRUE(gEvents.empty());
	EXPECT_EQ(heimdall::trace::ActiveSink(), nullptr);
}

TEST_F(TraceSpec, ScopeStartedBeforeRemovalStillEnds)
{
	{
		const heimdall::trace::Scope scope(Category::Index, "long");
		heimdall::trace::InstallSink(nullptr);
	}

	const std::vector<std::string> expected
	{
		"begin:long:", "end"
	};
	EXPECT_EQ(gEvents, expected);
}

TEST_F(TraceSpec, DisabledCategorySkipsDetailEvaluationAndAllEvents)
{
	heimdall::trace::InstallSink(&kFilteredSink);
	int evaluations = 0;

	{
		const heimdall::trace::Scope outer(Category::Index, "enabled");
		const heimdall::trace::Scope disabled(Category::MacroDetail, "disabled",[&]
			{
				++evaluations;
				return std::string_view("unused");
			});
		heimdall::trace::Instant(Category::MacroDetail, "ignored");
		heimdall::trace::Counter(Category::MacroDetail, "ignored", 1);
	}

	EXPECT_EQ(evaluations, 0);
	EXPECT_EQ(gEvents, (std::vector<std::string> {"begin:enabled:", "end"}));
}

TEST_F(TraceSpec, LockGuardSeparatesAcquisitionFromOwnership)
{
	std::mutex mutex;

	{
		const heimdall::trace::LockGuard lock(mutex, "cache");
		gEvents.emplace_back("protected");
	}

	EXPECT_EQ(gEvents, (std::vector<std::string>
		{"begin:cache:", "end", "begin:cache:", "protected", "end"}));
	ASSERT_TRUE(mutex.try_lock());
	mutex.unlock();
}

#ifdef HEIMDALL_PROFILING
TEST_F(TraceSpec, DetailExpressionIsSkippedWithoutSink)
{
	heimdall::trace::InstallSink(nullptr);
	int evaluations = 0;
	const auto detail = [&]()->std::string
	{
		++evaluations;
		return "expensive";
	};

	{
		HEIMDALL_TRACE_SCOPE_DETAIL(Index, "lazy", detail());
	}

	EXPECT_EQ(evaluations, 0);
}

TEST_F(TraceSpec, MacroScopeReportsCategoryDetail)
{
	{
		HEIMDALL_TRACE_SCOPE_DETAIL(Index, "macro", std::string("path"));
	}

	const std::vector<std::string> expected
	{
		"begin:macro:path", "end"
	};
	EXPECT_EQ(gEvents, expected);
}
#endif
