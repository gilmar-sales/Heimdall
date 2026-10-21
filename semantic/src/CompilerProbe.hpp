#pragma once

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>

namespace heimdall::detail
{
	struct CompilerProbeResult
	{
		std::string output;
		bool completed = false;
	};

	[[nodiscard]] CompilerProbeResult QueryCompiler(
		std::string_view compiler,
		const std::string& command,
		std::stop_token stop = {},
		std::chrono::milliseconds timeout = std::chrono::seconds(5));
}
