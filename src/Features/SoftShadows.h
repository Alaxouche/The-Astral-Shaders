#pragma once

#include "Feature.h"

/**
 * @brief Percentage-Closer Soft Shadows for the sun cascades.
 *
 * Replaces the fixed-radius PCF of the engine's sun shadow mask (Utility.hlsl,
 * RENDER_SHADOWMASK, quality filter modes) with a PCSS kernel: a blocker search
 * around the receiver estimates the average occluder distance, the penumbra width
 * follows from the sun's angular size and that distance, and the PCF radius widens
 * accordingly -- contact shadows stay crisp while distant occluders cast soft ones.
 * Runtime-gated through the feature buffer, so no new shader permutations exist and
 * toggling it never touches the shader cache.
 */
struct SoftShadows : Feature
{
	struct Settings
	{
		bool Enabled = true;
		// Apparent angular diameter of the sun, degrees. The real sun is ~0.53; larger reads
		// as an overcast, stylised softness.
		float SunAngularDiameter = 1.0f;
		// Penumbra ceiling in shadowmap texels; bounds both the blocker search and the PCF
		// radius, so it is also the cost ceiling.
		float MaxPenumbraTexels = 16.0f;
	};

	Settings settings;

	/** @brief GPU-facing mirror of the settings (SharedData.hlsli PCSSSettings). */
	struct GPUSettings
	{
		uint32_t Enabled = 1;
		float    TanSunRadius = 0.0087f;
		float    MaxPenumbraTexels = 16.0f;
		float    pad = 0.0f;
	};
	STATIC_ASSERT_ALIGNAS_16(GPUSettings);

	virtual std::string GetName() override { return "Soft Shadows"; }
	virtual std::string GetDisplayName() override { return T("feature.soft_shadows.name", "Soft Shadows"); }
	virtual std::string GetShortName() override { return "SoftShadows"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kShadows; }

	/** @brief Returns a description and list of key features for the UI summary. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.soft_shadows.description", "Distance-dependent sun shadow softness (PCSS): shadows are sharp at the contact point and widen with the distance to what casts them."),
			{ T("feature.soft_shadows.key_feature_1", "Blocker search drives a physically based penumbra width"),
				T("feature.soft_shadows.key_feature_2", "Contact shadows stay crisp, canopy and tower shadows go soft"),
				T("feature.soft_shadows.key_feature_3", "Runtime toggle, no shader cache impact") } };
	};

	/** @brief Draws the ImGui settings panel for Soft Shadows configuration. */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Returns the settings as the GPU consumes them. */
	GPUSettings GetCommonBufferData() const;
};
