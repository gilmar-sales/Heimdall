#include "BackgroundBatch.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <Heimdall/Trace.hpp>

namespace heimdall::lsp
{
	void RunBackgroundBatch(
		ThreadPool& pool,
		std::size_t itemCount,
		std::size_t concurrency,
		std::stop_token stop,
		const std::function<void(std::size_t, std::size_t) > & process)
	{
		if (itemCount == 0 || stop.stop_requested())
		{
			return;
		}

		concurrency = std::min(itemCount, std::max<std::size_t>(1, concurrency));
		std::vector<std::size_t> available;
		available.reserve(concurrency);
		for (std::size_t slot = concurrency; slot != 0; --slot)
		{
			available.push_back(slot - 1);
		}

		std::mutex mutex;
		std::condition_variable_any changed;
		std::size_t active = 0;
		std::exception_ptr failure;
		bool abandoned = false;

		const auto release = [&](std::size_t slot, std::exception_ptr error, bool ran)
		{
			{
				const std::lock_guard lock(mutex);
				available.push_back(slot);
				--active;
				abandoned |=!ran;
				if (error && !failure)
				{
					failure = std::move(error);
				}

				changed.notify_all();
			}
		};
		struct Completion
		{
			const decltype(release) & finish;
			std::size_t slot;
			std::exception_ptr error;
			bool ran  = false;
			bool owns = true;

			Completion(const decltype(release) & callback, std::size_t taskSlot) noexcept
			: finish(callback), slot(taskSlot) {}

			Completion(Completion&& other) noexcept
			: finish(other.finish), slot(other.slot), error(std::move(other.error)),
				ran(other.ran), owns(std::exchange(other.owns, false)) {}

			~Completion()
			{
				if (owns)
				{
					finish(slot, std::move(error), ran);
				}
			}
		};

		for (std::size_t index = 0; index < itemCount; ++index)
		{
			std::unique_lock lock(mutex);
			{
				HEIMDALL_TRACE_SCOPE(Scheduler, "IndexDispatchWait");
				changed.wait(lock, stop,[&]
					{
						return !available.empty() || failure || abandoned;
					});
			}
			if (stop.stop_requested() || failure || abandoned)
			{
				break;
			}

			const auto slot = available.back();
			available.pop_back();
			++active;
			lock.unlock();
			Completion completion(release, slot);
			try
			{
				const bool accepted = pool.TrySubmit([&, index, slot,
					completion = std::move(completion)]() mutable noexcept
					{
						completion.ran = true;
						try
						{
							if (!stop.stop_requested())
							{
								process(index, slot);
							}
						}
						catch (...)
						{
							completion.error = std::current_exception();
						}
						}, ThreadPool::Priority::Background);
				if (!accepted)
				{
					break;
				}
			}
			catch (...)
			{
				const std::lock_guard failureLock(mutex);
				if (!failure)
				{
					failure = std::current_exception();
				}

				break;
			}
		}

		{
			std::unique_lock lock(mutex);
			HEIMDALL_TRACE_SCOPE(Scheduler, "IndexCompletionWait");
			changed.wait(lock,[&]
				{
					return active == 0;
				});
		}
		if (failure)
		{
			std::rethrow_exception(failure);
		}

		if (abandoned)
		{
			throw std::runtime_error("Background indexing task discarded by pool shutdown");
		}
	}
}
