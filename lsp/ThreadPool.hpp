#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
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

                (priority == Priority::Interactive ? m_interactive : m_background).push_back(std::move(task));
            }
            m_cv.notify_one();
        }

        // Blocks until every submitted task has finished.
        void WaitIdle()
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_idle_cv.wait(lock,[&]
                {
                    return m_interactive.empty() && m_background.empty() && m_active == 0;
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
        void WorkerMain()
        {
            while (true)
            {
                Task task;
                {
                    std::unique_lock<std::mutex> lock(m_mu);
                    m_cv.wait(lock,[&]
                        {
                            return m_stopping ||!m_interactive.empty() ||!m_background.empty();
                    });
                    if (m_stopping)
                    {
                        return;
                    }

                    auto& queue = m_interactive.empty() ? m_background : m_interactive;
                    task = std::move(queue.front());
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

        std::mutex m_mu;
        std::condition_variable m_cv;
        std::condition_variable m_idle_cv;
        std::deque<Task> m_interactive;
        std::deque<Task> m_background;
        std::size_t m_active = 0;
        bool m_stopping = false;
        std::vector<std::thread> m_threads;
    };

} // namespace heimdall::lsp
