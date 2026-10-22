#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <Heimdall/Trace.hpp>

namespace heimdall::lsp
{
	/** Latest indexing activities, copied before invoking a potentially blocking publisher. */
	class IndexProgress
	{
	public:
		/** An owned, immutable-at-publication view of the latest activities. */
		struct Snapshot
		{
			std::vector<std::string> activities;
			std::uint64_t revision = 0;
		};

		/** Creates one activity slot per concurrent indexing task. */
		explicit IndexProgress(std::size_t slots) : mActivities(slots) {}

		/** Replaces a slot's activity without performing output or waiting for a publisher. */
		void Update(std::size_t slot, std::string activity)
		{
			HEIMDALL_TRACE_LOCK(mMutex, "IndexProgress::Update");
			mActivities[slot] = std::move(activity);
			++mRevision;
		}

		/** Publishes an owned snapshot with no progress mutex held during the callback. */
		void Report(const std::function<void(const Snapshot&) > & publish)
		{
			const auto snapshot = [&]
			{
				HEIMDALL_TRACE_LOCK(mMutex, "IndexProgress::Snapshot");
				return Snapshot {mActivities, mRevision};
			}
			();
			publish(snapshot);
		}

	private:
		std::mutex mMutex;
		std::vector<std::string> mActivities;
		std::uint64_t mRevision = 0;
	};
}
