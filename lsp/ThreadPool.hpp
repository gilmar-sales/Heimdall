#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace heimdall::lsp
{

	// Fixed-size worker pool with two priorities. Interactive requests
	// (completion, hover, goto, format) always run before background work
	// (compiler probing), so a slow background task can delay but never starve
	// a keystroke-driven request.
	//
	// Latency notes (<50ms goal):
	// - Tasks are move-only (`move_only_function`), so Dispatch can move the
	//   request body into the worker without an extra copy or a copyable
	//   `std::function` allocation on the submit path.
	// - A single mutex guards both queues; submit is O(1) and never does I/O
	//   or parsing while holding it. Workers pop interactives first.
	class ThreadPool
	{
	public:
		enum class Priority
		{
			Interactive,
			Background
		};

		/** Queue and execution counters, including tasks waiting in document lanes. */
		struct Metrics
		{
			std::size_t queued        = 0;
			std::size_t peakQueued    = 0;
			std::size_t active        = 0;
			std::uint64_t submitted   = 0;
			std::uint64_t completed   = 0;
			std::uint64_t failed      = 0;
			std::uint64_t cancelled   = 0;
			std::uint64_t queueNs     = 0;
			std::uint64_t executionNs = 0;
		};

		/** Returns a consistent snapshot without waiting for tasks to finish. */
		[[nodiscard]] Metrics Statistics() const
		{
			const std::lock_guard lock(m_mu);
			return mMetrics;
		}

		using Task = std::move_only_function<void() >;

		explicit ThreadPool(std::size_t threads)
		{
			m_threads.reserve(threads);
			for (std::size_t i = 0; i < std::max<std::size_t>(1, threads); ++i)
			{
				m_threads.emplace_back([this]
					{
						WorkerMain();
					});
			}
		}

		ThreadPool(const ThreadPool&) = delete;

		ThreadPool& operator= (const ThreadPool&) = delete;

		~ThreadPool()
		{
			Shutdown();
		}

		void Submit(Task task, Priority priority = Priority::Interactive)
		{
			{
				const std::lock_guard<std::mutex> lock(m_mu);
				if (m_stopping)
				{
					return;
				}

				(priority == Priority::Interactive ? m_interactive : m_background)
				.push_back(TimedTask(std::move(task)));
			}
			m_cv.notify_one();
		}

		/** Serializes tasks with the same key while independent keys run concurrently. */
		void SubmitSerial(std::string key, Task task)
		{
			{
				const std::lock_guard lock(m_mu);
				if (m_stopping)
				{
					return;
				}

				auto lane = m_lanes.try_emplace(key).first;
				lane->second.tasks.push_back({++mSerialSubmitted, TimedTask(std::move(task))});
				ScheduleLane(lane->first, lane->second);
			}
			m_cv.notify_one();
		}

		/** Pins workspace state after prior serial tasks and before later ones, without waiting. */
		void SubmitBarrier(Task task)
		{
			{
				const std::lock_guard lock(m_mu);
				if (m_stopping)
				{
					return;
				}

				mBarriers.push_back({mSerialSubmitted, TimedTask(std::move(task))});
				ScheduleBarrier();
			}
			m_cv.notify_one();
		}

		/** Releases queued captures immediately when their version is cancelled. */
		void SubmitCancellable(Task task, std::stop_token stop, Task cancelled)
		{
			struct Pending
			{
				std::mutex mutex;
				Task task;
				Task cancelled;
				std::optional<std::stop_callback<std::function<void() >>> callback;
			};

			auto pending       = std::make_shared<Pending>();
			pending->task      = std::move(task);
			pending->cancelled = std::move(cancelled);
			pending->callback.emplace(stop,
				[weak = std::weak_ptr(pending)]
				{
					if (auto pending = weak.lock())
					{
						Task cancelled;
						{
							const std::lock_guard lock(pending->mutex);
							if (pending->task)
							{
								pending->task = {};
								cancelled     = std::move(pending->cancelled);
							}
						}
						if (cancelled)
						{
							cancelled();
						}
					}
				});
			Submit(
				[this, pending]
				{
					Task task;
					{
						const std::lock_guard lock(pending->mutex);
						task               = std::move(pending->task);
						pending->cancelled = {};
					}
					if (task)
					{
						task();
					}
					else
					{
						const std::lock_guard lock(m_mu);
						++mMetrics.cancelled;
					}
				});
		}

		// Blocks until every submitted task has finished.
		void WaitIdle()
		{
			std::unique_lock<std::mutex> lock(m_mu);
			m_idle_cv.wait(lock,
				[&]
				{
					return m_interactive.empty() && m_background.empty() &&
					m_active == 0 && m_lanes.empty() && mBarriers.empty();
				});
		}

		// Drops queued tasks, lets running ones finish, joins the workers.
		void Shutdown()
		{
			{
				const std::lock_guard<std::mutex> lock(m_mu);
				m_stopping = true;
				m_interactive.clear();
				m_background.clear();
				m_lanes.clear();
				mBarriers.clear();
				mMetrics.queued = 0;
			}
			m_cv.notify_all();
			m_idle_cv.notify_all();
			for (auto& thread : m_threads)
			{
				if (thread.joinable())
				{
					thread.join();
				}
			}

			m_threads.clear();
		}

	private:
		using Clock = std::chrono::steady_clock;

		Task TimedTask(Task task)
		{
			++mMetrics.submitted;
			++mMetrics.queued;
			mMetrics.peakQueued = std::max(mMetrics.peakQueued, mMetrics.queued);
			return[this, task = std::move(task), queued = Clock::now()] mutable
			{
				const auto start = Clock::now();
				{
					const std::lock_guard lock(m_mu);
					if (mMetrics.queued != 0)
					{
						--mMetrics.queued;
					}

					++mMetrics.active;
					mMetrics.queueNs += static_cast<std::uint64_t>(
						std::chrono::duration_cast<std::chrono::nanoseconds>(start - queued).count());
				}
				try
				{
					task();
				}
				catch (...)
				{
					FinishTimedTask(start, true);
					throw;
				}

				FinishTimedTask(start, false);
			};
		}

		void FinishTimedTask(Clock::time_point start, bool failed)
		{
			const auto elapsed =
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
			const std::lock_guard lock(m_mu);
			--mMetrics.active;
			++mMetrics.completed;
			mMetrics.failed += failed;
			mMetrics.executionNs += static_cast<std::uint64_t>(elapsed);
		}

		struct SerialTask
		{
			std::uint64_t ticket = 0;
			Task task;
		};

		struct Lane
		{
			std::deque<SerialTask> tasks;
			bool scheduled = false;
		};

		void ScheduleLane(const std::string& key, Lane& lane)
		{
			if (lane.scheduled || lane.tasks.empty() || mBarrierActive ||
				(!mBarriers.empty() && lane.tasks.front().ticket > mBarriers.front().ticket))
			{
				return;
			}

			lane.scheduled = true;
			m_interactive.push_back([this, key]
				{
					RunLane(key);
				});
		}

		void ScheduleBarrier()
		{
			if (mBarrierActive || mBarriers.empty() || mBarriers.front().ticket > mSerialCompleted)
			{
				return;
			}

			mBarrierActive = true;
			auto task      = std::move(mBarriers.front().task);
			m_interactive.push_front(
				[this, task = std::move(task)] mutable
				{
					try
					{
						task();
					}
					catch (...) {}

					{
						const std::lock_guard lock(m_mu);
						mBarrierActive = false;
						if (!mBarriers.empty())
						{
							mBarriers.pop_front();
						}

						ScheduleBarrier();
						for (auto& [key, lane] : m_lanes)
						{
							ScheduleLane(key, lane);
						}
					}
					m_cv.notify_all();
				});
		}

		void RunLane(const std::string& key)
		{
			SerialTask next;
			{
				const std::lock_guard lock(m_mu);
				const auto lane = m_lanes.find(key);
				if (lane == m_lanes.end())
				{
					return;
				}

				next = std::move(lane->second.tasks.front());
				lane->second.tasks.pop_front();
			}
			try
			{
				next.task();
			}
			catch (...) {}

			{
				const std::lock_guard lock(m_mu);
				mSerialFinished.insert(next.ticket);
				while (mSerialFinished.erase(mSerialCompleted + 1) != 0)
				{
					++mSerialCompleted;
				}

				const auto lane = m_lanes.find(key);
				if (lane == m_lanes.end())
				{
					return;
				}

				lane->second.scheduled = false;
				ScheduleBarrier();
				if (lane->second.tasks.empty())
				{
					m_lanes.erase(lane);
				}
				else
				{
					ScheduleLane(lane->first, lane->second);
				}
			}
			m_cv.notify_all();
		}

		void WorkerMain()
		{
			while (true)
			{
				Task task;
				{
					std::unique_lock<std::mutex> lock(m_mu);
					m_cv.wait(
						lock,[&]
						{
							return m_stopping ||!m_interactive.empty() ||!m_background.empty();
						});
					if (m_stopping)
					{
						return;
					}

					auto& queue = m_interactive.empty() ? m_background : m_interactive;
					task        = std::move(queue.front());
					queue.pop_front();
					++m_active;
				}
				try
				{
					task();
				}
				catch (...)
				{
					// A failing handler must not take the whole server down.
				}

				{
					const std::lock_guard<std::mutex> lock(m_mu);
					--m_active;
					if (m_interactive.empty() && m_background.empty() && m_active == 0)
					{
						m_idle_cv.notify_all();
					}
				}
			}
		}

		mutable std::mutex m_mu;
		Metrics mMetrics;
		std::condition_variable m_cv;
		std::condition_variable m_idle_cv;
		std::deque<Task> m_interactive;
		std::deque<Task> m_background;
		std::unordered_map<std::string, Lane> m_lanes;
		std::deque<SerialTask> mBarriers;
		std::unordered_set<std::uint64_t> mSerialFinished;
		std::uint64_t mSerialSubmitted = 0;
		std::uint64_t mSerialCompleted = 0;
		bool mBarrierActive            = false;
		std::size_t m_active           = 0;
		bool m_stopping                = false;
		std::vector<std::thread> m_threads;
	};

} // namespace heimdall::lsp
