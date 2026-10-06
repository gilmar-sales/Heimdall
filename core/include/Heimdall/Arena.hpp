#pragma once

#include <cstddef>
#include <memory_resource>
#include <utility>

namespace heimdall
{

    constexpr std::size_t kDefaultInitialCapacity = 65536; // 64 KiB

    // Monotonic bump arena for SyntaxTree nodes, tokens and scratch data.
    // Allocations are never freed individually; Reset() releases everything
    // at once. Not thread-safe by design (one arena per worker thread).
    class Arena
    {
    public:
        explicit Arena(std::size_t initial_capacity = kDefaultInitialCapacity);

        Arena(const Arena&) = delete;

        Arena& operator= (const Arena&) = delete;

        void* Allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

        template <typename T, typename... Args> T * New(Args &&... args)
        {
            void* mem = Allocate(sizeof(T), alignof(T));
            return new (mem) T(std::forward<Args>(args)...);
        }

        // Adapter for std::pmr containers: allocations are forwarded to the
        // arena (and counted in Used()), deallocation is a no-op. Valid for as
        // long as the arena lives and is not moved (arenas are not movable).
        std::pmr::memory_resource* Resource() noexcept
        {
            return &m_adapter;
        }

        // Releases all memory back to the upstream resource.
        void Reset();

        // Approximate bytes handed out since construction or last Reset().
        std::size_t Used() const noexcept
        {
            return m_used;
        }

        std::size_t AllocationCount() const noexcept
        {
            return m_allocations;
        }

    private:
        class Adapter final : public std::pmr::memory_resource
        {
        public:
            explicit Adapter(Arena& arena) noexcept: m_arena(arena) {}

        private:
            void* do_allocate(std::size_t bytes, std::size_t alignment) override
            {
                return m_arena.Allocate(bytes, alignment);
            }

            void do_deallocate(void*, std::size_t, std::size_t) override {}

            bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
            {
                return this == &other;
            }

            Arena& m_arena;
        };

        std::pmr::monotonic_buffer_resource m_resource;
        std::size_t m_used = 0;
        std::size_t m_allocations = 0;
        Adapter m_adapter{*this};
    };

} // namespace heimdall
