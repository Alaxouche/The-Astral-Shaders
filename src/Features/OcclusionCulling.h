#pragma once

#include "Feature.h"

/**
 * @brief Per-object GPU Hi-Z occlusion culling, ported from doodlum's codex/hiz-culling-clean.
 *
 * Installs body detours on BSCullingProcess::Process1/Process2/TestBaseVisibility1 and the
 * BSParabolicCullingProcess overrides so that (a) once per frame, before the main cull walk,
 * the camera Hi-Z snapshot's view/projection is loaded, and (b) each processed scene object is
 * occlusion-tested against the GPU-reduced Hi-Z depth and skipped if provably hidden. Distant
 * small objects are additionally dropped from the shadow maps only.
 *
 * Body detours rather than vtable patches: the engine's main cull walk calls Process1 directly
 * (devirtualized), so vtable hooks never see it -- measured here as zero registrations across
 * four vtable entry points, then ~13k main-camera Process1 calls per frame once the bodies were
 * detoured (2026-09-29, AE 1.6.1170). The bodies are read out of the two vtables at install
 * time, so no per-runtime address-library id is needed.
 */
struct OcclusionCulling : Feature
{
	struct Settings
	{
		bool EnableOcclusionTesting = true;
		// Only objects with at least this world-bound radius are occlusion-tested.
		float OccluderTestMinRadius = 0.0f;
		// An occluded verdict must hold this many consecutive frames before the object is
		// actually skipped, so single-frame mistakes never reach the screen.
		int CullConfirmFrames = 2;
		// Distance-scaled small-object SHADOW cull (radius < min + slope*camDist), never actors.
		// Drops the object's shadow only (mesh still renders); cuts shadow-map draws, which are
		// the bulk of Utility. Invisible by design -> on by default.
		bool  CullSmallShadows = true;
		float SmallShadowMinSize = 0.0f;
		float SmallShadowSlope = 0.03f;
	};

	Settings settings;

	virtual std::string GetName() override { return "Occlusion Culling"; }
	virtual std::string GetDisplayName() override { return T("feature.occlusion_culling.name", "Occlusion Culling"); }
	virtual std::string GetShortName() override { return "OcclusionCulling"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }

	/** @brief Returns a description and list of key features for the UI summary. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.occlusion_culling.description", "Skips drawing scene objects fully hidden behind nearer geometry, using the previous frame's GPU-reduced depth. Also drops distant small objects from the shadow maps only."),
			{ T("feature.occlusion_culling.key_feature_1", "GPU Hi-Z depth reduction with a stall-free readback ring"),
				T("feature.occlusion_culling.key_feature_2", "Conservative per-object and room/cell container tests"),
				T("feature.occlusion_culling.key_feature_3", "Distance-scaled small-object shadow culling"),
				T("feature.occlusion_culling.key_feature_4", "Never culls actors, LOD or near geometry") } };
	};

	/** @brief Installs the culling-process body detours and marks the Hi-Z runtime live. */
	virtual void PostPostLoad() override;

	/** @brief Draws the ImGui settings panel for Occlusion Culling configuration. */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Pushes the current settings into the MOC runtime globals. */
	void SyncSettingsToMOC();

	/** @brief True when the feature is loaded and the master toggle is on. */
	bool IsActive() const;
};
