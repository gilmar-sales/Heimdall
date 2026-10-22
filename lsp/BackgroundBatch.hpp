#pragma once

#include <cstddef>
#include <functional>
#include <stop_token>

#include "ThreadPool.hpp"

namespace heimdall::lsp
{
	/**
	 * Runs one background task per item with a bounded sliding window. The caller must be
	 * outside this pool. Concurrent tasks have distinct slots; cancellation joins queued
	 * tasks before returning, and task failures or pool shutdown are reported to the caller.
	 */
	void RunBackgroundBatch(
		ThreadPool& pool,
		std::size_t itemCount,
		std::size_t concurrency,
		std::stop_token stop,
		const std::function<void(std::size_t, std::size_t) > & process);
}
