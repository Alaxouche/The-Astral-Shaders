#include "Ice.h"
#include <DirectXTex.h>

#include "Features/DetailMaps.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/DebugSection.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"
#include "Utils/Serialize.h"
#include "Utils/TextureRules.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.ice."

using namespace Util::TextureRules;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Ice::Settings,
	Enabled,
	Strength,
	Depth,
	Cracks,
	Bubbles,
	Glow,
	FrostRoughness,
	DeepColor,
	FadeStart,
	FadeEnd)

namespace
{
	constexpr uint32_t ReflectionsFlag = static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections);
	constexpr uint32_t TextureSlot = 59;
	constexpr uint32_t PerDrawSlot = 9;
	constexpr size_t MaxEntries = 8192;
	constexpr int32_t NoMatch = -1;
	constexpr int32_t Excluded = -2;
	constexpr std::string_view VolumePath = "Data\\Shaders\\Ice\\IceVolume.dds";
	constexpr std::string_view RulesPath = "Data\\Shaders\\Ice\\Rules";
	constexpr float MaxFade = 16000.0f;
}

void Ice::ClampSettings()
{
	const Settings defaults{};
	auto clamp = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	clamp(settings.Strength, defaults.Strength, 0.0f, 1.0f);
	clamp(settings.Depth, defaults.Depth, 0.0f, 4.0f);
	clamp(settings.Cracks, defaults.Cracks, 0.0f, 4.0f);
	clamp(settings.Bubbles, defaults.Bubbles, 0.0f, 4.0f);
	clamp(settings.Glow, defaults.Glow, 0.0f, 4.0f);
	clamp(settings.FrostRoughness, defaults.FrostRoughness, 0.0f, 1.2f);
	clamp(settings.DeepColor.x, defaults.DeepColor.x, 0.01f, 1.0f);
	clamp(settings.DeepColor.y, defaults.DeepColor.y, 0.01f, 1.0f);
	clamp(settings.DeepColor.z, defaults.DeepColor.z, 0.01f, 1.0f);
	clamp(settings.FadeStart, defaults.FadeStart, 0.0f, MaxFade);
	clamp(settings.FadeEnd, defaults.FadeEnd, settings.FadeStart + 1.0f, MaxFade + 1.0f);
}

void Ice::RestoreDefaultSettings()
{
	settings = {};
}

void Ice::LoadSettings(json& o_json)
{
	settings = o_json;
	ClampSettings();
}

void Ice::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Ice::DrawSettings()
{
	if (!HasContent())
		Util::Text::WrappedWarning("%s", T(TKEY("content_missing"), "The ice volume texture or rules are missing from Data\\Shaders\\Ice."));

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	Util::HoverTip(T(TKEY("enabled_tooltip"), "Shows the inside of clear ice on materials the rules match. Each kind of ice compiles its shader the first time it is seen, then loads from the shader cache."));

	ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &settings.Strength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	Util::HoverTip(T(TKEY("strength_tooltip"), "How much of the ice's surface color gives way to its interior where the surface is clear."));

	ImGui::SliderFloat(T(TKEY("depth"), "Depth"), &settings.Depth, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	Util::HoverTip(T(TKEY("depth_tooltip"), "How deep the interior looks: how far it shifts with the view and how blue it turns."));

	ImGui::ColorEdit3(T(TKEY("deep_color"), "Deep Color"), &settings.DeepColor.x);
	Util::HoverTip(T(TKEY("deep_color_tooltip"), "The color light keeps after passing down through the ice and back. Thicker ice tends toward its square."));

	if (ImGui::TreeNodeEx(T(TKEY("interior"), "Interior"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("cracks"), "Cracks"), &settings.Cracks, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("cracks_tooltip"), "Fracture planes inside the ice."));
		ImGui::SliderFloat(T(TKEY("bubbles"), "Bubbles"), &settings.Bubbles, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("bubbles_tooltip"), "Air trapped in the ice."));
		ImGui::SliderFloat(T(TKEY("glow"), "Glow"), &settings.Glow, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("glow_tooltip"), "How brightly cracks and bubbles catch the light."));
		ImGui::SliderFloat(T(TKEY("frost_roughness"), "Frost Roughness"), &settings.FrostRoughness, 0.0f, 1.2f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("frost_roughness_tooltip"), "PBR ice rougher than this reads as frost or snow and hides its interior. Above 1 nothing is hidden. Vanilla ice has no roughness map and always shows it."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("distance"), "Distance"))) {
		ImGui::SliderFloat(T(TKEY("fade_start"), "Fade Start"), &settings.FadeStart, 0.0f, MaxFade, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("fade_end"), "Fade End"), &settings.FadeEnd, 0.0f, MaxFade, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("fade_tooltip"), "The interior fades out between these distances in game units and costs nothing past the end."));
		ImGui::TreePop();
	}
	ClampSettings();

	if (Util::Button(T(TKEY("reload"), "Reload Rules")))
		Reload();
	Util::HoverTip(T(TKEY("reload_tooltip"), "Rereads Data\\Shaders\\Ice\\Rules\\*.json and the volume texture."));

	ImGui::Text("%s: %zu", T(TKEY("rule_count"), "Rules"), rules.size());
	for (const auto& error : errors)
		ImGui::TextColored(Util::Colors::GetError(), "%s", error.c_str());
}

void Ice::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;
	ImGui::Text("%s: %u", T(TKEY("pass_count"), "Ice draws last frame"), lastPassCount);
	ImGui::Text("%s: %zu", T(TKEY("cached_textures"), "Textures matched so far"), entries.size());
}

void Ice::LoadVolume()
{
	volume = nullptr;
	const std::filesystem::path path(VolumePath);
	DirectX::ScratchImage image;
	if (FAILED(DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image))) {
		errors.push_back(std::format("{}: {}", VolumePath, T(TKEY("volume_load_failed"), "could not be loaded")));
		return;
	}
	const auto& metadata = image.GetMetadata();
	if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 || metadata.IsCubemap()) {
		errors.push_back(std::format("{}: {}", VolumePath, T(TKEY("volume_not_2d"), "is not a plain 2D texture")));
		return;
	}
	if (FAILED(DirectX::CreateShaderResourceView(globals::d3d::device, image.GetImages(), image.GetImageCount(), metadata, volume.put()))) {
		errors.push_back(std::format("{}: {}", VolumePath, T(TKEY("volume_create_failed"), "could not be created on the GPU")));
		volume = nullptr;
		return;
	}
	winrt::com_ptr<ID3D11Resource> resource;
	volume->GetResource(resource.put());
	Util::SetResourceName(resource.get(), "Ice::Volume");
	Util::SetResourceName(volume.get(), "Ice::Volume SRV");
}

void Ice::LoadRules()
{
	rules.clear();
	excludes.clear();

	const auto files = ListFiles(RulesPath, ".json");
	for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
		const auto fileName = Util::PathHelpers::ToUtf8(files[fileIndex].filename());
		json data;
		try {
			std::ifstream stream(files[fileIndex]);
			data = json::parse(stream, nullptr, true, true);
		} catch (const std::exception& e) {
			errors.push_back(std::format("{}: {}", fileName, e.what()));
			continue;
		}
		if (!data.is_object()) {
			errors.push_back(std::format("{}: {}", fileName, T(TKEY("rules_not_object"), "expected a JSON object")));
			continue;
		}

		for (auto& pattern : ReadPatterns(data, "exclude"))
			excludes.push_back(std::move(pattern));

		const auto list = data.find("rules");
		if (list == data.end() || !list->is_array())
			continue;

		for (const auto& item : *list) {
			if (!item.is_object())
				continue;
			Rule rule;
			rule.file = fileIndex;
			rule.match = ReadPatterns(item, "match");
			rule.exclude = ReadPatterns(item, "exclude");
			if (rule.match.empty()) {
				errors.push_back(std::format("{}: {}", fileName, T(TKEY("rule_skipped"), "skipped a rule without match patterns")));
				continue;
			}
			rule.strength = ReadNumber(item, "strength", 1.0f, 0.0f, 1.0f);
			rule.depth = ReadNumber(item, "depth", 0.06f, 0.0f, 1.0f);
			rule.scale = ReadNumber(item, "scale", 2.0f, 0.01f, 64.0f);
			rule.cracks = ReadNumber(item, "cracks", 1.0f, 0.0f, 4.0f);
			rule.bubbles = ReadNumber(item, "bubbles", 1.0f, 0.0f, 4.0f);
			rule.priority = static_cast<int>(ReadNumber(item, "priority", 0.0f, -1000.0f, 1000.0f));
			rules.push_back(std::move(rule));
		}
	}

	std::ranges::stable_sort(rules, [](const Rule& a_left, const Rule& a_right) {
		return a_left.priority != a_right.priority ? a_left.priority > a_right.priority : a_left.file > a_right.file;
	});
	logger::info("[Ice] Loaded {} rules and {} exclusions from {} files", rules.size(), excludes.size(), files.size());
}

void Ice::Reload()
{
	errors.clear();
	LoadVolume();
	LoadRules();
	entries.clear();
	lastDiffuse = nullptr;
	lastRule = NoMatch;
	bindingValid = false;
}

int32_t Ice::MatchRule(const char* a_path) const
{
	if (!a_path || !*a_path)
		return NoMatch;

	const auto path = NormalizePath(a_path);
	if (MatchesAny(path, excludes))
		return Excluded;
	for (size_t index = 0; index < rules.size(); ++index) {
		const auto& rule = rules[index];
		if (MatchesAny(path, rule.match) && !MatchesAny(path, rule.exclude))
			return rule.strength > 0.0f ? static_cast<int32_t>(index) : Excluded;
	}
	return NoMatch;
}

int32_t Ice::FindRule(RE::BSRenderPass* a_pass)
{
	auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
	auto* diffuse = material ? material->diffuseTexture.get() : nullptr;
	if (!diffuse)
		return NoMatch;
	if (diffuse == lastDiffuse)
		return lastRule;

	auto it = entries.find(diffuse);
	if (it == entries.end() || it->second.name != diffuse->name) {
		int32_t rule = MatchRule(diffuse->name.c_str());
		if (rule == NoMatch && material->textureSet)
			rule = MatchRule(material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse));
		if (entries.size() >= MaxEntries)
			entries.clear();
		it = entries.insert_or_assign(diffuse, Entry{ diffuse->name, std::max(rule, NoMatch) }).first;
	}

	lastDiffuse = diffuse;
	lastRule = it->second.rule;
	return lastRule;
}

bool Ice::IsEligible(uint32_t a_descriptor) const
{
	return loaded && settings.Enabled && perDrawCB && HasContent() && settings.Strength > 0.0f && DetailMaps::IsMaterialDescriptor(a_descriptor);
}

bool Ice::BindPass(RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return false;
	if ((globals::state->permutationData.ExtraShaderDescriptor & ReflectionsFlag) != 0)
		return false;

	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	if (a_pass->shaderProperty->flags.any(kDecal, kDynamicDecal))
		return false;

	auto* geometry = a_pass->geometry;
	const float distance = DetailMaps::IsFirstPersonPass() ? 0.0f : std::max(0.0f, geometry->worldBound.center.GetDistance(Util::GetEyePosition()) - geometry->worldBound.radius);
	if (distance >= settings.FadeEnd)
		return false;

	const int32_t ruleIndex = FindRule(a_pass);
	if (ruleIndex < 0 || static_cast<size_t>(ruleIndex) >= rules.size())
		return false;

	const auto& rule = rules[static_cast<size_t>(ruleIndex)];
	const PerDraw perDraw{
		.Strength = rule.strength * settings.Strength,
		.Depth = rule.depth * settings.Depth,
		.Cracks = rule.cracks * settings.Cracks,
		.Bubbles = rule.bubbles * settings.Bubbles,
		.Scale = rule.scale,
		.FrostRoughness = settings.FrostRoughness,
		.FadeEnd = settings.FadeEnd,
		.FadeInvRange = 1.0f / std::max(settings.FadeEnd - settings.FadeStart, 1.0f),
		.Absorption = { -std::log(settings.DeepColor.x), -std::log(settings.DeepColor.y), -std::log(settings.DeepColor.z) },
		.Glow = settings.Glow
	};

	auto* context = globals::d3d::context;
	if (!bindingValid || std::memcmp(&perDraw, &lastPerDraw, sizeof(PerDraw)) != 0) {
		perDrawCB->Update(perDraw);
		lastPerDraw = perDraw;
	}
	if (!bindingValid) {
		ID3D11Buffer* buffer = perDrawCB->CB();
		context->PSSetConstantBuffers(PerDrawSlot, 1, &buffer);
		ID3D11ShaderResourceView* view = volume.get();
		context->PSSetShaderResources(TextureSlot, 1, &view);
	}

	bindingValid = true;
	++passCount;
	return true;
}

void Ice::BindNeutral()
{
	if (!perDrawCB)
		return;
	const PerDraw neutral{};
	auto* context = globals::d3d::context;
	if (!bindingValid || std::memcmp(&neutral, &lastPerDraw, sizeof(PerDraw)) != 0) {
		perDrawCB->Update(neutral);
		lastPerDraw = neutral;
	}
	if (!bindingValid) {
		ID3D11Buffer* buffer = perDrawCB->CB();
		context->PSSetConstantBuffers(PerDrawSlot, 1, &buffer);
		ID3D11ShaderResourceView* view = volume.get();
		context->PSSetShaderResources(TextureSlot, 1, &view);
	}
	bindingValid = true;
}

void Ice::Reset()
{
	lastPassCount = passCount;
	passCount = 0;
	bindingValid = false;
	lastDiffuse = nullptr;
	if (!entries.empty() && globals::state->IsMainOrLoadingMenuOpen())
		entries.clear();
}

void Ice::SetupResources()
{
	perDrawCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<PerDraw>(), "Ice::PerDraw");
	Reload();
}

#undef I18N_KEY_PREFIX
