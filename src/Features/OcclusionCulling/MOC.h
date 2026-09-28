#pragma once

// -----------------------------------------------------------------------------
// GPU Hi-Z occlusion for CommunityShaders, ported from doodlum's
// codex/hiz-culling-clean (eeba1ce45). Each frame the scene depth is reduced
// into a coarse grid on the GPU and read back (HiZPrepass); the cull walk
// projects each object's bounds and rejects those provably behind the farthest
// depth of every grid cell the bounds touch (HiZTestRect).
//
// Fork changes against the original: the SE-only address-library ids became
// CommonLibSSE-NG accessors (Main::WorldRootCamera, ShadowSceneNode's
// sunShadowDirLight, BSShadowDirectionalLight's fullFrustumCamera), so this
// runs on SE and AE; the reduce shader converts Reverse Z depth back to
// standard Z at the source; the NDC-to-cell mapping uses the dynamic render
// area so upscaling does not shift the grid; the grass-cull GPU consumer API
// was dropped (Grass Optimizations already culls grass in this fork).
// -----------------------------------------------------------------------------

#include <cstdint>

#include <DirectXMath.h>
#include <immintrin.h>

namespace RE
{
	class NiCamera;
	class NiAVObject;
}

namespace MOC
{
	// --- Runtime settings consumed by the Hi-Z occlusion path. The OcclusionCulling
	//     Feature writes these from its serialized settings; MOC reads them. ---
	extern bool  EnableOcclusionTesting;  // master gate for TestObject/TestMultiBound
	extern float OccluderTestMinRadius;   // min world-bound radius for per-object tests
	extern bool  CullSmallShadows;        // shadow-map distance-scaled small-caster cull (shadow only)
	extern float SmallShadowMinSize;      // shadow cull: base bound-radius threshold at the camera
	extern float SmallShadowSlope;        // shadow cull: threshold growth per unit of camera distance

	/** @brief Mark the occlusion feature live. Hi-Z GPU resources are created lazily. */
	void Init();

	/** @brief True once Init() has run. */
	bool IsInitialized();

	/**
	 * @brief Main-scene per-frame Hi-Z step: gate the pass by main-camera identity, CAS-claim the
	 *        frame, and load the published depth snapshot's matrices into the shared test globals.
	 * @return true if this pass is the main world view and occlusion testing is active.
	 */
	bool BuildOccluders(RE::NiCamera* a_camera);

	/**
	 * @brief Hi-Z GPU occlusion frame step: reduce this frame's scene depth into a coarse grid on
	 *        the GPU, harvest the oldest staging-ring readback into the CPU snapshot the cull tests
	 *        against, and queue this frame's copy. RENDER THREAD ONLY (immediate context) -- called
	 *        from Deferred::PrepassPasses after the frame's depth is populated.
	 */
	void HiZPrepass();

	/**
	 * @brief Occlusion query for a scene object.
	 * @return true if the object may be visible (keep it), false if provably occluded.
	 *         Returns true (conservative) for all early-outs (not initialized, testing
	 *         disabled, app-culled, tiny/near bounds, no Hi-Z snapshot yet).
	 */
	bool TestObject(RE::NiAVObject* a_object);

	/**
	 * @brief Occlusion query for a multibound container (rooms, cells, building shells)
	 *        from the engine's TestBaseVisibility path. Tight AABB test.
	 * @return true if possibly visible; false if provably occluded.
	 */
	bool TestMultiBound(void* a_multiBound);

	/**
	 * @brief True only for the RENDER camera (the scene-list accumulation cull); the gate that
	 *        restricts occlusion testing to the main view.
	 */
	bool IsSceneListCamera(const RE::NiCamera* a_camera);

	/** @brief True for the sun shadow-gather camera (stage-1 caster pre-gather). */
	bool IsSunGatherCamera(const RE::NiCamera* a_camera);

	/** @brief Small-caster shadow cull -- drops the shadow only (keep=true; never actors/kAlwaysDraw). */
	bool TestShadowCasterSmall(RE::NiAVObject* a_object);
}
