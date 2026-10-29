#pragma once

#include "Plugin.h"

namespace Fork
{
	/// Astral Shaders' own release line, independent of the Community Shaders base (Plugin::VERSION, which stays the
	/// upstream version so feature .ini versions and plugin checks keep working).
	inline constexpr unsigned VersionMajor = 2;
	inline constexpr unsigned VersionMinor = 0;
	inline constexpr unsigned VersionPatch = 0;

	/** @brief The full user-facing version: {Astral major.minor.patch}.{fork build:03}, e.g. "2.0.0.062". */
	inline std::string GetVersionString()
	{
		return std::format("{}.{}.{}.{:03}", VersionMajor, VersionMinor, VersionPatch, Plugin::FORK_BUILD);
	}
}
