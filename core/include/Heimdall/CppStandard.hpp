#pragma once

#include <cstdint>

namespace heimdall
{

	// Supported source-language dialects. Separate from the compiler standard
	// required to build Heimdall itself.
	enum class CppStandard : std::uint8_t
	{
		Cpp20,
		Cpp23,
		Cpp26
	};

} // namespace heimdall
