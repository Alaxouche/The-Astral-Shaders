#pragma once

#include "Plugin.h"

namespace Fork
{
	/** @brief The full user-facing version: {CS major.minor.patch}.{fork build:03}, e.g. "1.9.0.001". */
	inline std::string GetVersionString()
	{
		return std::format("{}.{}.{}.{:03}", Plugin::VERSION.major(), Plugin::VERSION.minor(), Plugin::VERSION.patch(), Plugin::FORK_BUILD);
	}
}
