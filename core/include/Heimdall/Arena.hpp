#pragma once

#include <cstddef>
#include <memory_resource>
#include <utility>

namespace heimdall
{

    // Monotonic bump arena for SyntaxTree nodes, tokens and scratch data.
    // Allocations are never freed individually; Reset() releases everything
    // at once. Not thread-safe by design (one arena per worker thread).
    class Arena
    {
    public:
        explicit Arena(std::size_t initial_capacity = 64 * 1024);
        Arena(const Arena &) = delete;
        Arena &operator= (const Arena &) = delete;

        void * Allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

        template <typename T, typename... Args> T * New(Args &&... args)
        {
            void *mem = Allocate(sizeof(T), alignof(T));
            return new (mem) T(std::forward<Args>(args)...);
        }

        // Releases all memory back to the upstream resource.
        void Reset();

        // Approximate bytes handed out since construction or last Reset().
        std::size_t Used() const noexcept
        {
            return m_used;
        }

    private:
        std::pmr::monotonic_buffer_resource m_resource;
        std::size_t m_used = 0;
    };

} // namespace heimdall
