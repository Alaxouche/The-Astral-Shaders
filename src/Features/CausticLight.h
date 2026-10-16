#pragma once

#include "Buffer.h"
#include "Feature.h"

#include <array>
#include <cstdint>
#include <vector>

/**
 * @brief Caustic Light: lamp, torch, sun and moon light reflected off rippling water onto nearby walls, ceilings and
 * bridge undersides, and refracted into bright streaks on pool floors.
 *
 * An integration of CausticLight by qtuna (GPL-3.0). The original standalone plugin draws over the finished frame and
 * has to guess each surface's light; here a compute pass inside the deferred pipeline reads the G-buffer, takes every
 * light from Light Limit Fix's clusters and the sun with its shadow map, and hands the composite an irradiance that
 * is multiplied by the real albedo. Water comes from the engine's water objects and the land height, gathered on the
 * CPU into a small top-down mask, so no Havok rays are cast.
 */
struct CausticLight : Feature
{
	virtual inline std::string GetName() override { return "Caustic Light"; }
	virtual std::string GetDisplayName() override { return T("feature.caustic_light.name", "Caustic Light"); }
	virtual inline std::string GetShortName() override { return "CausticLight"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.caustic_light.description", "Light reflected off rippling water dances on nearby walls, ceilings and bridge undersides, and gathers into moving streaks on pool floors."),
			{ T("feature.caustic_light.key_feature_1", "Every lamp, torch and spell light near the water, plus the sun and the moon"),
				T("feature.caustic_light.key_feature_2", "Follows wind, rain and swimmers; respects shadows on the water"),
				T("feature.caustic_light.key_feature_3", "Finds the water by itself, indoors and outdoors"),
				T("feature.caustic_light.key_feature_4", "Based on CausticLight by qtuna, rebuilt on the G-buffer") }
		};
	}

	/** @brief Persisted user settings. */
	struct Settings
	{
		bool Enabled = true;
		bool SunAndMoon = true;
		bool LocalLights = true;
		bool PoolFloors = true;
		bool OcclusionTest = true;
		/** @brief Multiplier of the physical reflection (1 = physical). */
		float Strength = 4.0f;
		float FloorStrength = 1.0f;
		/** @brief Ripple pattern contrast (0 = an even glow, 1 = physical). */
		float RippleContrast = 1.0f;
		/** @brief Height above the water that reflected light still reaches. */
		float Reach = 800.0f;
		/** @brief Camera distance where the effect has faded out. */
		float MaxDistance = 3000.0f;
		/** @brief Clustered lights considered per pixel. */
		uint32_t MaxLightsPerPixel = 12;
		/** @brief 0 off, 1 where reflected light lands, 2 the water the pass found. */
		uint32_t DebugView = 0;
	};

	Settings settings;

	/** @brief GPU constants, mirrored by cbuffer CausticCB in CausticLightCS.hlsl. */
	struct alignas(16) CausticCB
	{
		float4 Levels[4];
		float4 LevelBounds[4];
		float4 Mask;
		float4 Pattern;
		float4 Layers;
		float4 Ripple;
		float4 Sun;
		float4 Params;
		float4 Params2;
	};
	static_assert(sizeof(CausticCB) % 16 == 0);

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void DataLoaded() override;

	/** @brief Updates the water mask and dispatches the pass. Called from Deferred::DeferredPasses before the composite. */
	void Draw();

	/** @brief The irradiance written this frame, or nullptr when the pass did not run (the composite then adds nothing). */
	ID3D11ShaderResourceView* GetOutputSRV() const;

	/** @brief Keeps the directional shadow cascades bound by the shadow mask pass (PS t4). */
	void CaptureDirectionalShadowMap();

private:
	static constexpr int kMaskSize = 128;
	static constexpr float kMaskTexel = 48.0f;
	static constexpr int kMaxLevels = 4;
	static constexpr float kNoWater = -1.0e9f;
	static constexpr float kUnknown = -2.0e9f;

	/** @brief A flat water surface from the engine's water objects (absolute xy bounds, height). */
	struct WaterBox
	{
		float minX, minY, maxX, maxY, z;
	};

	/** @brief One water height present in the mask and the bounds of its open water. */
	struct Level
	{
		float z = 0.0f;
		float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
		int texels = 0;
	};

	void CreateResources();
	void LoadAtlases();
	bool GatherWaterBoxes();
	void UpdateMask();
	void BuildLevels();
	void UploadMask();
	void UpdateWaterMotion(float dt);
	ID3D11ComputeShader* GetCausticCS();

	eastl::unique_ptr<Texture2D> output;
	eastl::unique_ptr<ConstantBuffer> constants;
	winrt::com_ptr<ID3D11Texture2D> maskTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> maskSRV;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 6> atlasSRVs;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11SamplerState> shadowSampler;
	winrt::com_ptr<ID3D11ShaderResourceView> directionalShadowMap;
	ID3D11ComputeShader* causticCS = nullptr;
	bool atlasesReady = false;
	bool shaderFailed = false;  // a failed compile is not retried every frame (cleared with the shader cache)
	bool ranThisFrame = false;

	// Water mask: kMaskSize^2 texels of kMaskTexel units around the camera, each holding the height of the open water
	// there (kNoWater, or kUnknown before it was first computed). Texels are filled nearest first within a time budget per frame.
	std::vector<float> texelHeight;
	std::vector<std::uint8_t> texelPending;  // 1: (re)compute; the old height is shown meanwhile
	std::vector<std::uint16_t> scanOrder;
	std::size_t scanCursor = 0;
	int maskOriginX = 0;  // texel coordinates of the mask's corner
	int maskOriginY = 0;
	bool maskValid = false;
	bool maskChanged = false;
	std::uint64_t waterSignature = 0;
	RE::TESWorldSpace* maskWorldSpace = nullptr;
	RE::TESObjectCELL* maskInteriorCell = nullptr;
	std::vector<WaterBox> waterBoxes;
	std::array<Level, kMaxLevels> levels{};
	int levelCount = 0;
	double lastScanMs = 0.0;

	// Water motion
	float frameClock = 0.0f;
	float spread = 0.1f;
	std::array<float, 3> layerWeights{ 1.0f, 0.0f, 0.0f };
	float swimImpulse = 0.0f;
	float rippleX = 0.0f;
	float rippleY = 0.0f;
	float sunBeam = 1.0f;
	std::uint32_t frameIndex = 0;
	std::int64_t lastTicks = 0;
};
