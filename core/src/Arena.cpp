#include <Heimdall/Arena.hpp>

namespace heimdall
{

    Arena::Arena(std::size_t initial_capacity) : m_resource(initial_capacity) {}

    void * Arena::Allocate(std::size_t size, std::size_t alignment)
    {
        void *mem = m_resource.allocate(size, alignment);
        m_used +=(size + alignment - 1) & ~(alignment - 1);

        return mem;
    }

    void Arena::Reset()
    {
        m_resource.release();
        m_used = 0;
    }

} // namespace heimdall
