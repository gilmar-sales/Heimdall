#pragma once

#include <cstdint>

#include <Heimdall/TypeModel.hpp>

namespace heimdall::detail
{
	enum class ConversionResult
	{
		Valid,
		Invalid,
		Unknown
	};

	[[nodiscard]] ConversionResult CheckImplicitConversion(
		const TypeModel& types,
		std::uint32_t expression,
		TypeId target);
}
