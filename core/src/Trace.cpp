#include <Heimdall/Trace.hpp>

#include <atomic>

namespace heimdall::trace
{
	namespace
	{
		std::atomic<const Sink*> gSink
		{
			nullptr
		};
	}

	void InstallSink(const Sink* sink) noexcept
	{
		gSink.store(sink, std::memory_order_release);
	}

	const Sink* ActiveSink() noexcept
	{
		return gSink.load(std::memory_order_acquire);
	}
}
