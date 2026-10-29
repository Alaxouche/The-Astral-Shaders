#pragma once

// Astral Shaders: names Open Shaders code expects from its own infrastructure (CommonLibVR, VR runtime), mapped onto
// this fork. The fork is flat-screen only, so everything per eye resolves to the one view.

namespace globals::game
{
	/** @brief This fork has no VR runtime. */
	inline constexpr bool isVR = false;
}

namespace Util
{
	/** @brief Open Shaders unwraps CommonLib's proxied D3D pointers here; this fork's are the real ones. */
	template <class T>
	constexpr T* AsReal(T* a_pointer) noexcept
	{
		return a_pointer;
	}
}

#ifndef GET_INSTANCE_MEMBER
/** @brief CommonLibVR's accessor macro: binds a reference to a runtime member of a game object. */
#	define GET_INSTANCE_MEMBER(a_value, a_source) auto& a_value = (a_source)->GetRuntimeData().a_value;
#endif
