#include "OcclusionCulling.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "OcclusionCulling/MOC.h"
#include "Utils/UI.h"

#include <RE/B/BSCullingProcess.h>
#include <RE/B/BSMultiBound.h>
#include <RE/B/BSParabolicCullingProcess.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiCamera.h>

#include <imgui.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	OcclusionCulling::Settings,
	EnableOcclusionTesting,
	OccluderTestMinRadius,
	CullConfirmFrames,
	CullSmallShadows,
	SmallShadowMinSize,
	SmallShadowSlope)

namespace
{
	// Vtable indices as documented in CommonLibSSE-NG's NiCullingProcess / BSCullingProcess.
	constexpr std::size_t kSlotProcess1 = 0x16;
	constexpr std::size_t kSlotProcess2 = 0x17;
	constexpr std::size_t kSlotTestBaseVisibility1 = 0x1A;

	std::uintptr_t ReadSlot(REL::VariantID a_vtable, std::size_t a_slot)
	{
		REL::Relocation<std::uintptr_t*> vtbl{ a_vtable };
		return vtbl.get()[a_slot];
	}

	template <class T>
	bool Attach(const char* a_name, std::uintptr_t a_address)
	{
		stl::detour_thunk<T>(a_address);
		// DetourAttach rewrites func to the trampoline on success; unchanged means it failed.
		const bool ok = T::func.address() != a_address;
		logger::info("[OcclusionCulling] {} body at RVA 0x{:X}: detour {}",
			a_name, a_address - REL::Module::get().base(), ok ? "attached" : "FAILED");
		return ok;
	}

	// BSCullingProcess::Process1 (NiCullingProcess vtable index 0x16): per-object
	// processing / recursion driver. If the object is provably occluded during the
	// main cull pass, skip the original call entirely so neither the object nor its
	// subtree is accumulated.
	// Mirror the ENGINE's own cull side effect when we skip an object: clear its
	// kAccumulated flag exactly like BSCullingProcess does on a frustum cull (gated on
	// recurseToGeometry + updateAccumulateFlag). Without this, downstream consumers can
	// read a STALE accumulated bit from a previous frame on an object we occluded.
	inline void MarkCulledLikeEngine(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object)
	{
		auto* bsp = static_cast<RE::BSCullingProcess*>(a_self);
		if (bsp->recurseToGeometry && a_self->updateAccumulateFlag)
			a_object->GetFlags().reset(RE::NiAVObject::Flag::kAccumulated);
	}

	template <class HookT>
	void Process1_Impl(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg2)
	{
		// Gate on the CAMERA: the main view runs MULTIPLE concurrent cull processes (scene-list
		// jobs + per-pass subtree walks). Testing every SCENE-LIST (accumulation) walk keeps
		// culling uniform across the main view while excluding shadow/reflection/cubemap/first-
		// person culls.
		const bool bracketed = MOC::IsSceneListCamera(a_self->camera);
		// MAIN view: the Hi-Z occlusion test. When the Hi-Z front snapshot isn't published yet
		// the test conservatively keeps the object.
		if (bracketed && a_object && !MOC::TestObject(a_object)) {
			MarkCulledLikeEngine(a_self, a_object);
			return;  // occluded -> do not accumulate / recurse
		}

		// SUN shadow-gather pass: small-caster SHADOW cull. A rejection here removes the
		// caster from every cascade -- its mesh still renders in the main view, only its
		// shadow is dropped. Pure size math; no depth buffer is involved for shadows.
		if (!bracketed && a_object && MOC::IsSunGatherCamera(a_self->camera) && !MOC::TestShadowCasterSmall(a_object)) {
			MarkCulledLikeEngine(a_self, a_object);
			return;
		}

		HookT::func(a_self, a_object, a_arg2);
	}

	struct Process1_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg2)
		{
			Process1_Impl<Process1_Hook>(a_self, a_object, a_arg2);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Same hook for BSParabolicCullingProcess, which OVERRIDES Process1/Process2 with its
	// own bodies -- hooking only BSCullingProcess never sees those calls. Separate hook
	// structs keep each body's original function pointer.
	struct PProcess1_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg2)
		{
			Process1_Impl<PProcess1_Hook>(a_self, a_object, a_arg2);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Shared Process2 (top-level cull entry, vtable index 0x17) logic: drive the once-per-frame
	// Hi-Z snapshot load. BuildOccluders gates on pointer identity with the engine's main
	// world-render camera; occlusion testing itself is gated per-object inside Process1.
	void Process2_Begin(const RE::NiCamera* a_camera)
	{
		auto& feature = globals::features::occlusionCulling;
		if (feature.IsActive() && MOC::IsInitialized() && a_camera)
			MOC::BuildOccluders(const_cast<RE::NiCamera*>(a_camera));
	}

	template <class HookT>
	void Process2_Bracketed(RE::NiCullingProcess* a_self, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
	{
		Process2_Begin(a_camera);
		HookT::func(a_self, a_camera, a_scene, a_visibleSet);
	}

	struct Process2_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
		{
			Process2_Bracketed<Process2_Hook>(a_self, a_camera, a_scene, a_visibleSet);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PProcess2_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
		{
			Process2_Bracketed<PProcess2_Hook>(a_self, a_camera, a_scene, a_visibleSet);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// TestBaseVisibility1(BSMultiBound&) -- the engine's CONTAINER visibility path (rooms,
	// cells, building shells). Multibound nodes never reach Process1, so this is where the
	// high-value tight-AABB occlusion tests belong: one occluded container prunes all its
	// contents. Engine verdict first; we only downgrade visible -> occluded.
	struct TestBaseVis1_Hook
	{
		static bool thunk(RE::BSCullingProcess* a_self, RE::BSMultiBound* a_bound)
		{
			const bool visible = func(a_self, a_bound);
			if (visible && a_bound &&
				MOC::IsSceneListCamera(static_cast<RE::NiCullingProcess*>(a_self)->camera) &&
				!MOC::TestMultiBound(a_bound))
				return false;
			return visible;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PTestBaseVis1_Hook
	{
		static bool thunk(RE::BSCullingProcess* a_self, RE::BSMultiBound* a_bound)
		{
			const bool visible = func(a_self, a_bound);
			if (visible && a_bound &&
				MOC::IsSceneListCamera(static_cast<RE::NiCullingProcess*>(a_self)->camera) &&
				!MOC::TestMultiBound(a_bound))
				return false;
			return visible;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void OcclusionCulling::PostPostLoad()
{
	// Idempotent guard: config-mode reloads must never double-attach the detours.
	static bool s_installed = false;
	if (s_installed)
		return;

	// Sync BEFORE Init so the default settings reach the Hi-Z runtime.
	SyncSettingsToMOC();
	MOC::Init();

	// Detour the Process1/Process2/TestBaseVisibility1 FUNCTION BODIES of both culling-process
	// classes. Body detours are essential: the engine's main cull walk calls Process1 DIRECTLY
	// (devirtualized), so vtable patches never see it -- four vtable entry points measured zero
	// here before this technique, then ~13k main-camera calls per frame with it. The bodies are
	// read out of the vtables at runtime, so no per-runtime id table is needed. Installed
	// unconditionally; runtime behavior is gated by IsActive() inside the thunks.
	const auto baseP1 = ReadSlot(RE::VTABLE_BSCullingProcess[0], kSlotProcess1);
	const auto baseP2 = ReadSlot(RE::VTABLE_BSCullingProcess[0], kSlotProcess2);
	const auto baseTBV = ReadSlot(RE::VTABLE_BSCullingProcess[0], kSlotTestBaseVisibility1);
	const auto paraP1 = ReadSlot(RE::VTABLE_BSParabolicCullingProcess[0], kSlotProcess1);
	const auto paraP2 = ReadSlot(RE::VTABLE_BSParabolicCullingProcess[0], kSlotProcess2);
	const auto paraTBV = ReadSlot(RE::VTABLE_BSParabolicCullingProcess[0], kSlotTestBaseVisibility1);

	bool ok = Attach<Process1_Hook>("BSCullingProcess::Process1", baseP1);
	ok &= Attach<Process2_Hook>("BSCullingProcess::Process2", baseP2);
	ok &= Attach<TestBaseVis1_Hook>("BSCullingProcess::TestBaseVisibility1", baseTBV);
	// A slot the parabolic class does not override resolves to the base body; detouring the
	// same body twice would chain the two thunks, so only attach where the override is real.
	if (paraP1 != baseP1)
		ok &= Attach<PProcess1_Hook>("BSParabolicCullingProcess::Process1", paraP1);
	if (paraP2 != baseP2)
		ok &= Attach<PProcess2_Hook>("BSParabolicCullingProcess::Process2", paraP2);
	if (paraTBV != baseTBV)
		ok &= Attach<PTestBaseVis1_Hook>("BSParabolicCullingProcess::TestBaseVisibility1", paraTBV);

	if (!ok) {
		// A partial install would cull inconsistently between passes; disable outright.
		settings.EnableOcclusionTesting = false;
		SyncSettingsToMOC();
		failedLoadedMessage = "Occlusion Culling could not attach its culling-process detours and has been disabled.";
		logger::error("[OcclusionCulling] detour install incomplete, feature disabled");
	}

	s_installed = true;
	logger::info("[OcclusionCulling] hooks installed (master={})", settings.EnableOcclusionTesting ? "on" : "off");
}

bool OcclusionCulling::IsActive() const
{
	return loaded && settings.EnableOcclusionTesting;
}

void OcclusionCulling::SyncSettingsToMOC()
{
	MOC::EnableOcclusionTesting = loaded && settings.EnableOcclusionTesting;
	MOC::OccluderTestMinRadius = settings.OccluderTestMinRadius;
	MOC::CullConfirmFrames = static_cast<std::uint32_t>(std::clamp(settings.CullConfirmFrames, 1, 4));
	MOC::CullSmallShadows = settings.CullSmallShadows;
	MOC::SmallShadowMinSize = settings.SmallShadowMinSize;
	MOC::SmallShadowSlope = settings.SmallShadowSlope;
}

void OcclusionCulling::DrawSettings()
{
	ImGui::TextWrapped("%s", T("feature.occlusion_culling.desc",
		"GPU Hi-Z occlusion culling: skips drawing scene objects fully hidden behind nearer geometry, using the previous frame's reduced depth."));
	ImGui::Separator();

	bool changed = false;

	changed |= ImGui::Checkbox(T("feature.occlusion_culling.enable_testing", "Enable Occlusion Culling"), &settings.EnableOcclusionTesting);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.occlusion_culling.enable_testing_tooltip",
			"Master toggle. When on, scene objects hidden behind nearer geometry are skipped using the GPU Hi-Z depth."));
	}

	ImGui::BeginDisabled(!settings.EnableOcclusionTesting);

	changed |= ImGui::SliderFloat(T("feature.occlusion_culling.min_test_radius", "Min Tested Object Size"), &settings.OccluderTestMinRadius, 0.0f, 200.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.occlusion_culling.min_test_radius_tooltip",
			"Objects smaller than this (world-bound radius) are never occlusion-tested. Lower = more draw calls saved but more CPU per frame."));
	}

	changed |= ImGui::SliderInt(T("feature.occlusion_culling.confirm_frames", "Cull Confirmation Frames"), &settings.CullConfirmFrames, 1, 4);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.occlusion_culling.confirm_frames_tooltip",
			"An object is only skipped after being occluded this many frames in a row. Higher = no visible pop-out even in fast motion, at the cost of drawing occluded objects a few extra frames."));
	}

	ImGui::Spacing();
	ImGui::TextDisabled("%s", T("feature.occlusion_culling.small_header", "Small-Object Culling"));
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.small_shadows", "Cull Small Object Shadows"), &settings.CullSmallShadows);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.occlusion_culling.small_shadows_tooltip",
			"Drops distant small objects from the SHADOW MAPS only -- their mesh still renders, just its shadow disappears. Cuts shadow-map draw calls (the bulk of the Utility shader time) with no visible change. Threshold grows with distance; actors are never culled. Safe to raise."));
	}
	if (settings.CullSmallShadows) {
		changed |= ImGui::SliderFloat(T("feature.occlusion_culling.small_shadow_slope", "Shadow Cull Distance Growth"), &settings.SmallShadowSlope, 0.0f, 0.08f, "%.3f");
	}

	ImGui::EndDisabled();

	if (changed)
		SyncSettingsToMOC();
}

void OcclusionCulling::LoadSettings(json& o_json)
{
	settings = o_json;
	SyncSettingsToMOC();
}

void OcclusionCulling::SaveSettings(json& o_json)
{
	o_json = settings;
}

void OcclusionCulling::RestoreDefaultSettings()
{
	settings = {};
	SyncSettingsToMOC();
}
