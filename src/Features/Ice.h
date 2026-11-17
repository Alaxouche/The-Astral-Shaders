#pragma once

#include "Buffer.h"

/**
 * Gives clear ice depth: fracture planes, bubbles and the ice body under the surface, refracted and absorbed toward a
 * deep color. Chooses materials by texture-path rules and rides Detail Maps' per-draw Lighting permutation.
 */
struct Ice : Feature
{
	virtual inline std::string GetName() override { return "Ice"; }
	virtual std::string GetDisplayName() override { return T("feature.ice.name", "Ice"); }
	virtual inline std::string GetShortName() override { return "Ice"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kMaterials; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.ice.description", "Gives glaciers, icebergs and ice caves real depth: cracks and bubbles inside the ice, refracted by its surface and fading to blue below."),
			{ T("feature.ice.key_feature_1", "Fracture planes and trapped air at two depths, shifting with the view"),
				T("feature.ice.key_feature_2", "Frost and snow on the surface hide the interior; clear ice shows it"),
				T("feature.ice.key_feature_3", "Works on vanilla and PBR ice by texture path, without new textures"),
				T("feature.ice.key_feature_4", "Three texture reads on ice up close, nothing elsewhere") } };
	}

	struct Settings
	{
		bool Enabled = true;
		float Strength = 0.6f;
		float Depth = 1.0f;
		float Cracks = 0.8f;
		float Bubbles = 0.5f;
		float Glow = 0.45f;
		float FrostRoughness = 0.7f;
		float3 DeepColor = { 0.30f, 0.62f, 0.80f };
		float FadeStart = 2000.0f;
		float FadeEnd = 6000.0f;
	} settings;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void Reset() override;
	virtual bool HasDebugSection() const override { return true; }
	virtual void DrawDebugSection() override;

	bool IsEligible(uint32_t a_descriptor) const;
	bool BindPass(RE::BSRenderPass* a_pass);
	/** Turns ice off for a draw that uses the permutation only for Detail Maps. */
	void BindNeutral();
	void InvalidateBindings() { bindingValid = false; }

private:
	struct PerDraw
	{
		float Strength = 0.0f;
		float Depth = 0.0f;
		float Cracks = 0.0f;
		float Bubbles = 0.0f;
		float Scale = 0.0f;
		float FrostRoughness = 0.0f;
		float FadeEnd = 0.0f;
		float FadeInvRange = 0.0f;
		float3 Absorption = { 0.0f, 0.0f, 0.0f };
		float Glow = 0.0f;
	};
	static_assert(sizeof(PerDraw) == 48);

	struct Rule
	{
		std::vector<std::string> match;
		std::vector<std::string> exclude;
		float strength = 1.0f;
		float depth = 0.06f;
		float scale = 2.0f;
		float cracks = 1.0f;
		float bubbles = 1.0f;
		int priority = 0;
		size_t file = 0;
	};

	struct Entry
	{
		RE::BSFixedString name;
		int32_t rule = -1;
	};

	bool HasContent() const { return volume && !rules.empty(); }
	void Reload();
	void LoadVolume();
	void LoadRules();
	int32_t FindRule(RE::BSRenderPass* a_pass);
	int32_t MatchRule(const char* a_path) const;
	void ClampSettings();

	std::vector<Rule> rules;
	std::vector<std::string> excludes;
	std::vector<std::string> errors;
	ankerl::unordered_dense::map<RE::NiSourceTexture*, Entry> entries;
	RE::NiSourceTexture* lastDiffuse = nullptr;
	int32_t lastRule = -1;

	winrt::com_ptr<ID3D11ShaderResourceView> volume;
	eastl::unique_ptr<ConstantBuffer> perDrawCB;
	PerDraw lastPerDraw{};
	bool bindingValid = false;
	bool volumeBound = false;
	uint32_t passCount = 0;
	uint32_t lastPassCount = 0;
};
