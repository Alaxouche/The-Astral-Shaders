#pragma once

// Astral Shaders: what Open Shaders' Neural Rendering expects from Open Shaders' own infrastructure,
// mapped onto this fork. The fork is flat-screen only, so everything per eye resolves to the one view.

#include "Globals.h"
#include "State.h"

#include <d3d11.h>

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

namespace NR::Compat
{
	/** @brief Open Shaders' Feature::SceneExposure. */
	struct SceneExposure
	{
		ID3D11ShaderResourceView* adaptedLuminance = nullptr;
		float compensationScale = 1.0f;
		float2 luminanceRange{ 0.0f, 0.0f };
	};

	/** @brief This fork publishes no adapted scene luminance; the pass uses its manual exposure. */
	inline bool FindSceneExposure(SceneExposure&) { return false; }

	/** @brief Open Shaders' State::BindSharedDataCS: the shared data (b5) and, optionally, the feature data (b6). */
	inline void BindSharedDataCS(ID3D11DeviceContext* a_context, bool a_withFeatureData)
	{
		ID3D11Buffer* buffers[]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
		a_context->CSSetConstantBuffers(5, a_withFeatureData ? 2u : 1u, buffers);
	}

	/** @brief The main camera's view data (Open Shaders' Util::GetCameraData(eye)). */
	inline RE::BSGraphics::ViewData CameraViewData()
	{
		return globals::game::shadowState->GetRuntimeData().cameraData.getEye();
	}
}
