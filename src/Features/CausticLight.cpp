#include "CausticLight.h"

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "Features/LightLimitFix.h"
#include "Features/TerrainBlending.h"
#include "Features/WaterEffects.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#include <DirectXTex.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numeric>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CausticLight::Settings,
	Enabled,
	SunAndMoon,
	LocalLights,
	PoolFloors,
	OcclusionTest,
	Strength,
	FloorStrength,
	RippleContrast,
	Reach,
	MaxDistance,
	MaxLightsPerPixel,
	DebugView)

namespace
{
	// Mask texels are matched to a level within this height (placed water planes are not perfectly level).
	constexpr float kLevelTolerance = 4.0f;
	// Exterior land this far above the water hides it.
	constexpr float kLandAboveWater = 2.0f;
	// The shallowest floor still lit through the surface is this deep at most (the original's kBottomMaxDepth).
	constexpr float kBottomMaxDepth = 400.0f;
	constexpr float kBottomAbsorb = 300.0f;
	// Water objects whose bounds are this tall are sloped (waterfalls, rapids) and are left out.
	constexpr float kMaxFlatThickness = 10.0f;
	constexpr double kScanBudgetMs = 0.3;

	void SanitizeSettings(CausticLight::Settings& s)
	{
		auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
		s.Strength = std::clamp(finite(s.Strength, 4.0f), 0.0f, 20.0f);
		s.FloorStrength = std::clamp(finite(s.FloorStrength, 1.0f), 0.0f, 5.0f);
		s.RippleContrast = std::clamp(finite(s.RippleContrast, 1.0f), 0.0f, 3.0f);
		s.Reach = std::clamp(finite(s.Reach, 800.0f), 100.0f, 2000.0f);
		s.MaxDistance = std::clamp(finite(s.MaxDistance, 3000.0f), 500.0f, 6000.0f);
		s.MaxLightsPerPixel = std::clamp(s.MaxLightsPerPixel, 1u, 64u);
		s.DebugView = std::min(s.DebugView, 2u);
	}

	struct WeatherSample
	{
		float wind = 0.0f;  // 0..1 of TESWeather windSpeed
		float rain = 0.0f;
		float snow = 0.0f;
		float cloud = 0.0f;
	};

	WeatherSample Sample(const RE::TESWeather* weather)
	{
		WeatherSample sample;
		if (!weather)
			return sample;
		sample.wind = weather->data.windSpeed / 255.0f;
		sample.rain = weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy) ? 1.0f : 0.0f;
		sample.snow = weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow) ? 1.0f : 0.0f;
		sample.cloud = weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kCloudy) ? 1.0f : 0.0f;
		return sample;
	}
}

#define I18N_KEY_PREFIX "feature.caustic_light."

void CausticLight::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);

	ImGui::SeparatorText(T(TKEY("sources"), "Light Sources"));
	ImGui::Checkbox(T(TKEY("sun_and_moon"), "Sun and Moon"), &settings.SunAndMoon);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("sun_and_moon_tooltip"), "Outdoors, the directional light reflected off lakes and rivers. Overcast and rainy skies have no beam to reflect."));
	ImGui::Checkbox(T(TKEY("local_lights"), "Lamps, Torches and Spells"), &settings.LocalLights);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("local_lights_tooltip"), "Every light Light Limit Fix places near the water. Needs Light Limit Fix."));
	ImGui::Checkbox(T(TKEY("pool_floors"), "Pool Floors"), &settings.PoolFloors);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("pool_floors_tooltip"), "Light refracted through the surface gathers into moving streaks on the floor below. Sunlit floors are left to Water Effects when it is installed."));

	ImGui::SeparatorText(T(TKEY("look"), "Look"));
	ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &settings.Strength, 0.0f, 20.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("strength_tooltip"), "Multiplier of the physical reflection. 1 is what a real water surface reflects; real footage reads stronger because the eye adapts to the shade."));
	ImGui::SliderFloat(T(TKEY("floor_strength"), "Pool Floor Strength"), &settings.FloorStrength, 0.0f, 5.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("ripple_contrast"), "Ripple Contrast"), &settings.RippleContrast, 0.0f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("ripple_contrast_tooltip"), "0 gives an even glow, 1 the physical pattern, higher values sharper lines."));
	ImGui::SliderFloat(T(TKEY("reach"), "Reach Above Water"), &settings.Reach, 100.0f, 2000.0f, "%.0f");
	ImGui::SliderFloat(T(TKEY("max_distance"), "Maximum Distance"), &settings.MaxDistance, 500.0f, 6000.0f, "%.0f");

	ImGui::SeparatorText(T(TKEY("quality"), "Quality"));
	static constexpr uint32_t kMinLights = 1;
	static constexpr uint32_t kMaxLights = 64;
	ImGui::SliderScalar(T(TKEY("max_lights"), "Lights per Pixel"), ImGuiDataType_U32, &settings.MaxLightsPerPixel, &kMinLights, &kMaxLights, "%u", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("max_lights_tooltip"), "Upper bound on the clustered lights each pixel reflects. Lower is faster in heavily lit places."));
	ImGui::Checkbox(T(TKEY("occlusion"), "Occlusion Test"), &settings.OcclusionTest);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("occlusion_tooltip"), "Blocks reflections whose path from the water is hidden by something on screen, such as the walkway a canal runs under."));

	const char* debugViews[] = { T(TKEY("debug_off"), "Off"), T(TKEY("debug_light"), "Where Reflected Light Lands"), T(TKEY("debug_water"), "Water Found") };
	int debugView = static_cast<int>(settings.DebugView);
	if (ImGui::Combo(T(TKEY("debug_view"), "Debug View"), &debugView, debugViews, IM_ARRAYSIZE(debugViews)))
		settings.DebugView = static_cast<uint32_t>(debugView);

	ImGui::TextDisabled(T(TKEY("status"), "Water levels in view: %d, water objects: %d, last scan %.2f ms"), levelCount, static_cast<int>(waterBoxes.size()), lastScanMs);
	if (!globals::features::lightLimitFix.loaded)
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", T(TKEY("no_llf"), "Light Limit Fix is not loaded: only the sun and the moon are reflected."));
	if (!atlasesReady)
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", T(TKEY("missing_atlases"), "The caustic pattern textures are missing; the effect is off."));

	SanitizeSettings(settings);
}

#undef I18N_KEY_PREFIX

void CausticLight::LoadSettings(json& o_json)
{
	settings = o_json;
	SanitizeSettings(settings);
}

void CausticLight::SaveSettings(json& o_json)
{
	o_json = settings;
}

void CausticLight::RestoreDefaultSettings()
{
	settings = {};
}

void CausticLight::SetupResources()
{
	// A failed D3D call throws, and nothing above a feature's SetupResources catches it: disable instead of crashing.
	try {
		CreateResources();
	} catch (const std::exception& e) {
		logger::error("[Caustic Light] Could not create GPU resources ({}); the feature is disabled", e.what());
		failedLoadedMessage = "Disabled: GPU resources could not be created";
		loaded = false;
		output = nullptr;
		maskTexture = nullptr;
	}
}

void CausticLight::CreateResources()
{
	auto device = globals::d3d::device;

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	Util::SetResourceName(linearSampler.get(), "CausticLight::LinearSampler");

	samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, shadowSampler.put()));
	Util::SetResourceName(shadowSampler.get(), "CausticLight::ShadowSampler");

	constants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<CausticCB>(), "CausticLight::Constants");

	{
		D3D11_TEXTURE2D_DESC mainDesc{};
		globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture->GetDesc(&mainDesc);
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = mainDesc.Width;
		desc.Height = mainDesc.Height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		output = eastl::make_unique<Texture2D>(desc, "CausticLight::Irradiance");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		output->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		output->CreateUAV(uavDesc);
	}

	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kMaskSize;
		desc.Height = kMaskSize;
		desc.MipLevels = 0;  // full chain: coarser mips are the water share of a wider disc (the glossy spread)
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, maskTexture.put()));
		Util::SetResourceName(maskTexture.get(), "CausticLight::WaterMask");
		DX::ThrowIfFailed(device->CreateShaderResourceView(maskTexture.get(), nullptr, maskSRV.put()));
		Util::SetResourceName(maskSRV.get(), "CausticLight::WaterMask SRV");
	}

	// Nearest texels first, so the water next to the camera is known within a frame or two.
	scanOrder.resize(static_cast<std::size_t>(kMaskSize) * kMaskSize);
	std::iota(scanOrder.begin(), scanOrder.end(), std::uint16_t{ 0 });
	std::stable_sort(scanOrder.begin(), scanOrder.end(), [](std::uint16_t a, std::uint16_t b) {
		auto distance = [](std::uint16_t i) {
			const float x = (i % kMaskSize) + 0.5f - kMaskSize * 0.5f;
			const float y = (i / kMaskSize) + 0.5f - kMaskSize * 0.5f;
			return x * x + y * y;
		};
		return distance(a) < distance(b);
	});
	texelHeight.assign(scanOrder.size(), kUnknown);
	texelPending.assign(scanOrder.size(), 1);

	LoadAtlases();
}

void CausticLight::LoadAtlases()
{
	// Register order t3..t8 in CausticLightCS.hlsl.
	constexpr std::array<const char*, 6> files{ "refl_calm.dds", "refl_normal.dds", "refl_rough.dds", "calm.dds", "normal.dds", "rough.dds" };
	atlasesReady = true;
	for (std::size_t i = 0; i < files.size(); ++i) {
		const std::filesystem::path path = std::filesystem::path("Data\\Shaders\\CausticLight") / files[i];
		DirectX::ScratchImage image;
		DirectX::TexMetadata metadata{};
		HRESULT hr = DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
		if (SUCCEEDED(hr))
			hr = DirectX::CreateShaderResourceView(globals::d3d::device, image.GetImages(), image.GetImageCount(), metadata, atlasSRVs[i].put());
		if (FAILED(hr) || !atlasSRVs[i]) {
			logger::warn("[Caustic Light] Could not load {} (HRESULT 0x{:08X}); the effect is off", path.string(), static_cast<std::uint32_t>(hr));
			atlasSRVs[i] = nullptr;
			atlasesReady = false;
			continue;
		}
		Util::SetResourceName(atlasSRVs[i].get(), std::format("CausticLight::{} SRV", files[i]).c_str());
	}
}

void CausticLight::ClearShaderCache()
{
	if (causticCS) {
		causticCS->Release();
		causticCS = nullptr;
	}
	shaderFailed = false;
}

void CausticLight::DataLoaded()
{
	SanitizeSettings(settings);
}

ID3D11ComputeShader* CausticLight::GetCausticCS()
{
	if (!causticCS && !shaderFailed) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::lightLimitFix.loaded)
			defines.push_back({ "LIGHT_LIMIT_FIX", nullptr });
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", nullptr });
		causticCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\CausticLight\\CausticLightCS.hlsl", defines, "cs_5_0"));
		if (!causticCS) {
			shaderFailed = true;
			logger::warn("[Caustic Light] The compute shader failed to compile; the effect is off until the shader cache is cleared");
		}
	}
	return causticCS;
}

void CausticLight::CaptureDirectionalShadowMap()
{
	ID3D11ShaderResourceView* shadowMap = nullptr;
	globals::d3d::context->PSGetShaderResources(4, 1, &shadowMap);
	directionalShadowMap.copy_from(shadowMap);
	if (shadowMap)
		shadowMap->Release();
}

ID3D11ShaderResourceView* CausticLight::GetOutputSRV() const
{
	return ranThisFrame && output ? output->srv.get() : nullptr;
}

bool CausticLight::GatherWaterBoxes()
{
	waterBoxes.clear();
	auto* waterSystem = RE::TESWaterSystem::GetSingleton();
	if (!waterSystem)
		return false;

	const auto& camera = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float horizontal = settings.MaxDistance + kMaskSize * kMaskTexel * 0.5f;
	std::uint64_t signature = 1469598103934665603ull;
	auto mix = [&signature](float v) {
		signature ^= static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(v)));
		signature *= 1099511628211ull;
	};

	for (const auto& waterObject : waterSystem->waterObjects) {
		if (!waterObject)
			continue;
		for (const auto& bound : waterObject->multiBounds) {
			if (!bound)
				continue;
			const auto& center = bound->center;
			const auto& size = bound->size;
			if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) || !std::isfinite(size.x) || !std::isfinite(size.y) ||
				!(size.z <= kMaxFlatThickness))
				continue;
			WaterBox box{ center.x - size.x, center.y - size.y, center.x + size.x, center.y + size.y, center.z };
			const float dx = std::max({ box.minX - camera.x, camera.x - box.maxX, 0.0f });
			const float dy = std::max({ box.minY - camera.y, camera.y - box.maxY, 0.0f });
			if (dx > horizontal || dy > horizontal || std::fabs(camera.z - box.z) > settings.Reach + settings.MaxDistance)
				continue;
			waterBoxes.push_back(box);
			mix(box.minX);
			mix(box.minY);
			mix(box.maxX);
			mix(box.maxY);
			mix(box.z * 8.0f);
		}
	}

	if (signature != waterSignature) {
		// Water objects came or went (a cell attached): every texel is checked again, keeping its old value meanwhile.
		waterSignature = signature;
		scanCursor = 0;
		std::fill(texelPending.begin(), texelPending.end(), std::uint8_t{ 1 });
	}
	return !waterBoxes.empty();
}

void CausticLight::UpdateMask()
{
	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return;
	RE::TESObjectCELL* interior = tes->interiorCell;
	RE::TESWorldSpace* worldSpace = interior ? nullptr : tes->GetRuntimeData2().worldSpace;

	const auto& camera = globals::game::frameBufferCached.GetCameraPosAdjust();
	// The mask moves in steps of 16 texels, so it is not shifted on every step the player takes.
	constexpr int kSnap = 16;
	const int centerX = static_cast<int>(std::floor(camera.x / kMaskTexel));
	const int centerY = static_cast<int>(std::floor(camera.y / kMaskTexel));
	const int originX = static_cast<int>(std::floor(centerX / static_cast<float>(kSnap))) * kSnap - kMaskSize / 2;
	const int originY = static_cast<int>(std::floor(centerY / static_cast<float>(kSnap))) * kSnap - kMaskSize / 2;

	if (!maskValid || interior != maskInteriorCell || worldSpace != maskWorldSpace) {
		std::fill(texelHeight.begin(), texelHeight.end(), kUnknown);
		std::fill(texelPending.begin(), texelPending.end(), std::uint8_t{ 1 });
		maskInteriorCell = interior;
		maskWorldSpace = worldSpace;
		maskOriginX = originX;
		maskOriginY = originY;
		maskValid = true;
		scanCursor = 0;
		maskChanged = true;
	} else if (originX != maskOriginX || originY != maskOriginY) {
		const int dx = originX - maskOriginX;
		const int dy = originY - maskOriginY;
		std::vector<float> shifted(texelHeight.size(), kUnknown);
		std::vector<std::uint8_t> shiftedPending(texelPending.size(), 1);
		for (int y = 0; y < kMaskSize; ++y) {
			const int sy = y + dy;
			if (sy < 0 || sy >= kMaskSize)
				continue;
			for (int x = 0; x < kMaskSize; ++x) {
				const int sx = x + dx;
				if (sx < 0 || sx >= kMaskSize)
					continue;
				const std::size_t to = static_cast<std::size_t>(y) * kMaskSize + x;
				const std::size_t from = static_cast<std::size_t>(sy) * kMaskSize + sx;
				shifted[to] = texelHeight[from];
				shiftedPending[to] = texelPending[from];
			}
		}
		texelHeight = std::move(shifted);
		texelPending = std::move(shiftedPending);
		maskOriginX = originX;
		maskOriginY = originY;
		scanCursor = 0;
		maskChanged = true;
	}

	if (scanCursor >= scanOrder.size())
		return;

	const auto start = std::chrono::steady_clock::now();
	auto elapsedMs = [&start] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
	int computed = 0;
	for (; scanCursor < scanOrder.size(); ++scanCursor) {
		const std::size_t index = scanOrder[scanCursor];
		if (!texelPending[index])
			continue;
		float& height = texelHeight[index];
		if ((computed++ & 31) == 31 && elapsedMs() > kScanBudgetMs)
			break;

		const float x = (maskOriginX + static_cast<int>(index % kMaskSize) + 0.5f) * kMaskTexel;
		const float y = (maskOriginY + static_cast<int>(index / kMaskSize) + 0.5f) * kMaskTexel;
		float best = kNoWater;
		for (const auto& box : waterBoxes) {
			if (x < box.minX || x > box.maxX || y < box.minY || y > box.maxY || box.z <= best)
				continue;
			if (!interior) {
				// Outdoors the cell water covers whole cells: it is open water only where the land is below it.
				float land = 0.0f;
				if (tes->GetLandHeight(RE::NiPoint3(x, y, box.z), land) && std::isfinite(land) && land > box.z + kLandAboveWater)
					continue;
			}
			best = box.z;
		}
		if (height != best)
			maskChanged = true;
		height = best;
		texelPending[index] = 0;
	}
	lastScanMs = elapsedMs();
}

void CausticLight::BuildLevels()
{
	const auto& camera = globals::game::frameBufferCached.GetCameraPosAdjust();
	struct Candidate
	{
		Level level;
		float nearest = FLT_MAX;
	};
	std::vector<Candidate> candidates;
	for (std::size_t i = 0; i < texelHeight.size(); ++i) {
		const float z = texelHeight[i];  // a texel queued again keeps its old height until it is checked
		if (!(z > kNoWater * 0.5f))
			continue;
		const float x0 = (maskOriginX + static_cast<int>(i % kMaskSize)) * kMaskTexel;
		const float y0 = (maskOriginY + static_cast<int>(i / kMaskSize)) * kMaskTexel;
		auto found = std::find_if(candidates.begin(), candidates.end(), [z](const Candidate& c) { return std::fabs(c.level.z - z) <= kLevelTolerance; });
		if (found == candidates.end()) {
			if (candidates.size() >= 16)
				continue;
			candidates.push_back({ { z, x0, y0, x0 + kMaskTexel, y0 + kMaskTexel, 0 }, FLT_MAX });
			found = candidates.end() - 1;
		}
		auto& level = found->level;
		level.minX = std::min(level.minX, x0);
		level.minY = std::min(level.minY, y0);
		level.maxX = std::max(level.maxX, x0 + kMaskTexel);
		level.maxY = std::max(level.maxY, y0 + kMaskTexel);
		level.texels++;
		const float dx = x0 + kMaskTexel * 0.5f - camera.x;
		const float dy = y0 + kMaskTexel * 0.5f - camera.y;
		const float dz = z - camera.z;
		found->nearest = std::min(found->nearest, dx * dx + dy * dy + dz * dz);
	}
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.nearest < b.nearest; });
	levelCount = static_cast<int>(std::min<std::size_t>(candidates.size(), kMaxLevels));
	for (int i = 0; i < levelCount; ++i)
		levels[i] = candidates[i].level;
}

void CausticLight::UploadMask()
{
	auto context = globals::d3d::context;
	std::vector<float> share(static_cast<std::size_t>(kMaskSize) * kMaskSize * 4, 0.0f);
	for (std::size_t i = 0; i < texelHeight.size(); ++i) {
		const float z = texelHeight[i];
		if (!(z > kNoWater * 0.5f))
			continue;
		for (int l = 0; l < levelCount; ++l)
			if (std::fabs(levels[l].z - z) <= kLevelTolerance)
				share[i * 4 + l] = 1.0f;
	}

	std::vector<std::uint8_t> bytes;
	int size = kMaskSize;
	for (UINT mip = 0; size >= 1; ++mip) {
		bytes.resize(static_cast<std::size_t>(size) * size * 4);
		for (std::size_t i = 0; i < bytes.size(); ++i)
			bytes[i] = static_cast<std::uint8_t>(std::lround(std::clamp(share[i], 0.0f, 1.0f) * 255.0f));
		context->UpdateSubresource(maskTexture.get(), mip, nullptr, bytes.data(), static_cast<UINT>(size * 4), 0);
		if (size == 1)
			break;
		const int next = size / 2;
		std::vector<float> reduced(static_cast<std::size_t>(next) * next * 4);
		for (int y = 0; y < next; ++y)
			for (int x = 0; x < next; ++x)
				for (int c = 0; c < 4; ++c) {
					auto at = [&](int sx, int sy) { return share[(static_cast<std::size_t>(sy) * size + sx) * 4 + c]; };
					reduced[(static_cast<std::size_t>(y) * next + x) * 4 + c] = 0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
				}
		share = std::move(reduced);
		size = next;
	}
}

void CausticLight::UpdateWaterMotion(float dt)
{
	WeatherSample weather;
	const bool interior = Util::IsInterior();
	if (!interior) {
		if (auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather) {
			const float t = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
			const WeatherSample current = Sample(sky->currentWeather);
			const WeatherSample last = sky->lastWeather ? Sample(sky->lastWeather) : current;
			weather.wind = std::lerp(last.wind, current.wind, t);
			weather.rain = std::lerp(last.rain, current.rain, t);
			weather.snow = std::lerp(last.snow, current.snow, t);
			weather.cloud = std::lerp(last.cloud, current.cloud, t);
		}
	}

	// A swimmer's wake: the impulse fades over about a second and a half once the player leaves the water.
	bool swimming = false;
	if (auto* player = RE::PlayerCharacter::GetSingleton()) {
		if (auto* state = player->AsActorState(); state && state->IsSwimming()) {
			swimming = true;
			const auto position = player->GetPosition();
			const float k = 1.0f - std::exp(-dt / 0.15f);
			rippleX = std::lerp(rippleX, position.x, swimImpulse > 0.01f ? k : 1.0f);
			rippleY = std::lerp(rippleY, position.y, swimImpulse > 0.01f ? k : 1.0f);
		}
	}
	swimImpulse = swimming ? 1.0f : swimImpulse * std::exp(-dt / 1.5f);

	// Water surface slope (Cox and Munk: wind adds 0.00512 per m/s to the mean-square slope; rain rings, a swimmer's
	// wake and an interior's drips and currents add their own). The slope sets the reflection's spread, the layer
	// crossfade and how fast the pattern plays.
	const float meanSquare = 0.003f + 0.00512f * weather.wind * 15.0f + 0.012f * (weather.rain + 0.3f * weather.snow) +
	                         0.03f * swimImpulse + (interior ? 0.004f : 0.0f);
	const float sigma = std::atan(std::sqrt(meanSquare));
	spread = std::tan(2.0f * std::clamp(sigma, 0.0f, 0.6f));
	const float agitation = std::clamp((sigma * 57.29578f - 3.0f) / 11.0f, 0.0f, 1.0f);
	if (agitation < 0.5f)
		layerWeights = { 1.0f - 2.0f * agitation, 2.0f * agitation, 0.0f };
	else
		layerWeights = { 0.0f, 2.0f - 2.0f * agitation, 2.0f * agitation - 1.0f };
	const float speed = 0.35f + 1.9f * agitation;
	frameClock = std::fmod(frameClock + dt * 8.0f * speed, 64.0f);

	// Caustics are the focused beam: an overcast sky scatters it, precipitation means a closed cloud deck.
	const float precipitation = std::max(weather.rain, weather.snow);
	sunBeam = (1.0f - precipitation) * std::lerp(1.0f, 0.35f, weather.cloud);
}

void CausticLight::Draw()
{
	ranThisFrame = false;
	if (!loaded || !settings.Enabled || !atlasesReady || !output || !maskTexture)
		return;

	const float dt = globals::game::deltaTime ? std::clamp(*globals::game::deltaTime, 0.0f, 0.1f) : 0.0f;
	UpdateWaterMotion(dt);
	frameIndex++;

	if (!GatherWaterBoxes()) {
		levelCount = 0;
		return;
	}
	UpdateMask();
	if (maskChanged) {
		BuildLevels();
		UploadMask();
		maskChanged = false;
	}
	if (levelCount == 0)
		return;

	auto* shader = GetCausticCS();
	if (!shader)
		return;

	auto& lightLimitFix = globals::features::lightLimitFix;
	const bool localLights = settings.LocalLights && lightLimitFix.loaded && lightLimitFix.lights && lightLimitFix.lightIndexList && lightLimitFix.lightGrid;
	const bool localShadows = localLights && lightLimitFix.IsLocalShadowCacheActive() && lightLimitFix.localShadowBuffer;
	const bool sun = settings.SunAndMoon && directionalShadowMap && globals::deferred->directionalShadowLights;

	CausticCB data{};
	for (int i = 0; i < kMaxLevels; ++i) {
		if (i < levelCount) {
			const auto& level = levels[i];
			data.Levels[i] = { level.z, settings.Reach, kBottomMaxDepth, 1.0f };
			data.LevelBounds[i] = { level.minX, level.minY, level.maxX, level.maxY };
		}
	}
	data.Mask = { maskOriginX * kMaskTexel, maskOriginY * kMaskTexel, 1.0f / (kMaskSize * kMaskTexel), kMaskTexel };
	data.Pattern = { frameClock, spread, settings.RippleContrast, settings.PoolFloors ? settings.FloorStrength : 0.0f };
	data.Layers = { layerWeights[0], layerWeights[1], layerWeights[2], settings.Strength };
	data.Ripple = { rippleX, rippleY, swimImpulse, 150.0f };
	data.Sun = { sun ? sunBeam : 0.0f, globals::features::waterEffects.loaded ? 0.0f : 1.0f, 0.0f, 0.0f };
	data.Params = { settings.MaxDistance, static_cast<float>(settings.MaxLightsPerPixel), settings.OcclusionTest ? 1.0f : 0.0f, static_cast<float>(settings.DebugView) };
	data.Params2 = { localLights ? 1.0f : 0.0f, settings.PoolFloors ? 1.0f : 0.0f, kBottomAbsorb, static_cast<float>(frameIndex & 1023) };
	constants->Update(data);

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];

	ID3D11ShaderResourceView* srvs[10]{
		Util::GetCurrentSceneDepthSRV(false),
		normalRoughness.SRV,
		maskSRV.get(),
		atlasSRVs[0].get(),
		atlasSRVs[1].get(),
		atlasSRVs[2].get(),
		atlasSRVs[3].get(),
		atlasSRVs[4].get(),
		atlasSRVs[5].get(),
		sun ? directionalShadowMap.get() : nullptr,
	};
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	ID3D11ShaderResourceView* shadowData = sun ? globals::deferred->directionalShadowLights->srv.get() : nullptr;
	context->CSSetShaderResources(98, 1, &shadowData);
	ID3D11ShaderResourceView* lightSrvs[3]{
		localLights ? lightLimitFix.lights->srv.get() : nullptr,
		localLights ? lightLimitFix.lightIndexList->srv.get() : nullptr,
		localLights ? lightLimitFix.lightGrid->srv.get() : nullptr,
	};
	context->CSSetShaderResources(35, 3, lightSrvs);
	ID3D11ShaderResourceView* localShadowSrvs[2]{
		localShadows ? lightLimitFix.localShadowBuffer->srv.get() : nullptr,
		localShadows ? lightLimitFix.localShadowCache->srv.get() : nullptr,
	};
	context->CSSetShaderResources(102, 2, localShadowSrvs);

	ID3D11SamplerState* samplers[2]{ linearSampler.get(), shadowSampler.get() };
	context->CSSetSamplers(0, 2, samplers);
	ID3D11Buffer* cb = constants->CB();
	context->CSSetConstantBuffers(0, 1, &cb);
	ID3D11UnorderedAccessView* uav = output->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(shader, nullptr, 0);

	const auto dispatch = Util::GetScreenDispatchCount(true);
	globals::profiler->BeginPass("CausticLight");
	context->Dispatch(dispatch.x, dispatch.y, 1);
	globals::profiler->EndPass();

	ID3D11ShaderResourceView* nullSrvs[10]{};
	context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->CSSetShaderResources(98, 1, &nullSrv);
	context->CSSetShaderResources(35, 3, nullSrvs);
	context->CSSetShaderResources(102, 2, nullSrvs);
	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCb);
	ID3D11SamplerState* nullSamplers[2]{};
	context->CSSetSamplers(0, 2, nullSamplers);
	context->CSSetShader(nullptr, nullptr, 0);

	ranThisFrame = true;
}
