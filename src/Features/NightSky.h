#pragma once

/**
 * @brief Physically inspired night sky: a rotating star field, the Milky Way, airglow and auroras.
 *
 * Everything is drawn on the sky dome, so moons and clouds, drawn after it, cover it naturally.
 * Stars follow the game's own star visibility (time of day and weather); auroras follow the
 * weather's aurora flag, with an optional chance on any clear night.
 */
struct NightSky : Feature
{
	/** @brief Persisted Night Sky settings. */
	struct Settings
	{
		bool Enabled = true;
		bool ReplaceVanillaStars = true;
		float StarBrightness = 1.0f;
		float StarDensity = 1.0f;
		float Twinkle = 0.6f;
		float MilkyWay = 1.0f;
		float Airglow = 1.0f;
		float Latitude = 58.0f;

		bool Aurora = true;
		bool ReplaceVanillaAurora = true;
		float AuroraIntensity = 1.0f;
		float AuroraClearNightChance = 0.25f;
		float3 AuroraColorLow = { 0.18f, 1.0f, 0.42f };
		float3 AuroraColorHigh = { 0.85f, 0.18f, 0.55f };
	};

	/** @brief Per-frame data shared with HLSL; mirrors NightSkySettings in SharedData.hlsli. */
	struct alignas(16) PerFrame
	{
		uint Enabled;
		uint ReplaceStars;
		float StarVisibility;
		float StarBrightness;

		float StarDensity;
		float Twinkle;
		float MilkyWay;
		float Airglow;

		float AuroraStrength;
		float AuroraIntensity;
		float SiderealAngle;
		float Latitude;

		float3 AuroraColorLow;
		float Night;

		float3 AuroraColorHigh;
		float padNightSky;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	Settings settings;

	virtual inline std::string GetName() override { return "Night Sky"; }
	virtual std::string GetDisplayName() override { return T("feature.night_sky.name", "Night Sky"); }
	virtual inline std::string GetShortName() override { return "NightSky"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kSkyAndWeather; }
	virtual inline std::string_view GetShaderDefineName() override { return "NIGHT_SKY"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type shaderType) override { return shaderType == RE::BSShader::Type::Sky; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.night_sky.description", "A physically inspired night sky: thousands of coloured, twinkling stars turning around the celestial pole, the Milky Way with its dust lanes, airglow and ray-marched auroras."),
			{ T("feature.night_sky.key_feature_1", "Star field that rotates through the night and twinkles near the horizon"),
				T("feature.night_sky.key_feature_2", "Milky Way band with a bright core and dark dust lanes"),
				T("feature.night_sky.key_feature_3", "Auroral curtains driven by the weather, with an optional chance on clear nights") }
		};
	}

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void Prepass() override;

	/** @brief Returns the per-frame data uploaded to the shared feature buffer. */
	PerFrame GetCommonBufferData() const;

private:
	/** @brief Aurora driven by the weather, 0-1, including the clear-night chance. */
	float GetAuroraActivity(float a_starVisibility) const;
	/** @brief Hides the game's aurora mesh while ours is showing, and only ever unhides what it hid. */
	void UpdateVanillaAurora(bool a_hide);

	bool vanillaAuroraHidden = false;
};
