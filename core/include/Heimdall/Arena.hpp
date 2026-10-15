#pragma once

#include <cstddef>
#include <memory_resource>
#include <utility>

namespace heimdall
{

	/** Default initial capacity of an arena, in bytes. (64 KiB)*/
	constexpr std::size_t DEFAULT_INITIAL_CAPACITY = 65536;

	/**
	 * @brief Monotonic allocator for objects and temporary data.
	 *
	 * Allocations are not freed individually; Reset() releases all allocated
	 * memory at once. This type is not thread-safe and is intended for one
	 * worker thread at a time.
	 */
	class Arena
	{
	public:
		/**
		 * @brief Creates an arena with the requested initial capacity.
		 * @param initial_capacity Initial capacity in bytes.
		 */
		explicit Arena(std::size_t initial_capacity = DEFAULT_INITIAL_CAPACITY);

		/** @brief Copy construction is disabled. */
		Arena(const Arena&) = delete;

		/** @brief Copy assignment is disabled. */
		Arena& operator= (const Arena&) = delete;

		/**
		 * @brief Allocates a block of memory from the arena.
		 * @param size Number of bytes to allocate.
		 * @param alignment Required alignment, defaulting to max_align_t.
		 * @return Pointer to the allocated memory.
		 */
		void* Allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

		/**
		 * @brief Constructs an object in arena-allocated memory.
		 * @tparam T Type of object to construct.
		 * @tparam Args Types of the constructor arguments.
		 * @param args Arguments forwarded to T's constructor.
		 * @return Pointer to the newly constructed object.
		 */
		template <typename T, typename... Args>
		T* New(Args &&... args)
		{
			void* mem = Allocate(sizeof(T), alignof(T));
			return new (mem) T(std::forward<Args>(args)...);
		}

		/**
		 * @brief Returns a memory-resource adapter for PMR containers.
		 *
		 * Allocations are forwarded to this arena and counted by Used();
		 * deallocation is a no-op. The resource is valid only while this arena
		 * is alive.
		 * @return Pointer to this arena's memory resource.
		 */
		std::pmr::memory_resource* Resource() noexcept
		{
			return &m_adapter;
		}

		/** @brief Releases all arena memory back to the upstream resource. */
		void Reset();

		/** @returns Approximate bytes allocated since construction or the last Reset(). */
		std::size_t Used() const noexcept
		{
			return m_used;
		}

		/** @returns Number of allocations since construction or the last Reset(). */
		std::size_t AllocationCount() const noexcept
		{
			return m_allocations;
		}

		/** @returns Bytes currently reserved from the upstream allocator, including arena slack. */
		[[nodiscard]] std::size_t ReservedBytes() const noexcept
		{
			return mUpstream.bytes;
		}

		/** @returns Largest upstream reservation since construction or the last reset. */
		[[nodiscard]] std::size_t PeakReservedBytes() const noexcept
		{
			return mUpstream.peak;
		}

	private:
		class Upstream final : public std::pmr::memory_resource
		{
		public:
			std::size_t bytes = 0;
			std::size_t peak  = 0;

		private:
			void* do_allocate(std::size_t size, std::size_t alignment) override
			{
				auto* memory = std::pmr::new_delete_resource()->allocate(size, alignment);
				bytes += size;
				if (bytes > peak)
				{
					peak = bytes;
				}

				return memory;
			}

			void do_deallocate(void* memory, std::size_t size, std::size_t alignment) override
			{
				bytes -= size;
				std::pmr::new_delete_resource()->deallocate(memory, size, alignment);
			}

			bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
			{
				return this == &other;
			}
		};

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

		Upstream mUpstream;
		std::pmr::monotonic_buffer_resource m_resource;
		std::size_t m_used        = 0;
		std::size_t m_allocations = 0;
		Adapter m_adapter {*this};
	};

} // namespace heimdall
