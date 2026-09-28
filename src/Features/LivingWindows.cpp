#include "LivingWindows.h"

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "Features/ReverseZ.h"
#include "Features/SkySync.h"
#include "Globals.h"
#include "State.h"

#include <RE/M/Main.h>
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <DirectXPackedVector.h>
#include <DirectXTex.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LivingWindows::Settings,
	Enabled,
	GlassOptics,
	Interiors,
	Occupants,
	Curtains,
	OutdoorViews,
	DebugOverlay,
	RoomDepth,
	InteriorBrightness,
	NightLitShare,
	OccupantActivity,
	CurtainShare,
	GlassRoughness,
	GlassReflectance,
	GrimeStrength,
	Waviness,
	RainDroplets,
	DropletStrength,
	Frost,
	FrostStrength,
	Condensation,
	CondensationStrength,
	FadeStart,
	FadeEnd,
	OutdoorBrightness,
	MinRoomRadius,
	MinOccupantRadius,
	LightShafts,
	ShaftIntensity,
	ShaftLength,
	ShaftDensity,
	ExtraWindowTokens,
	ExcludedTokens)

namespace
{
	std::string Lower(std::string_view a_text)
	{
		std::string result(a_text);
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return result;
	}

	/** @brief Splits a path into lowercase alphabetic and numeric runs ("Sol_Window01.dds" -> sol, window, 01, dds). */
	std::vector<std::string> Tokenize(std::string_view a_text)
	{
		std::vector<std::string> tokens;
		std::string current;
		char previousClass = 0;
		for (const unsigned char c : a_text) {
			const char cls = std::isalpha(c) ? 'a' : (std::isdigit(c) ? 'd' : 0);
			if (!current.empty() && (!cls || cls != previousClass)) {
				tokens.push_back(std::move(current));
				current.clear();
			}
			if (cls)
				current.push_back(static_cast<char>(std::tolower(c)));
			previousClass = cls;
		}
		if (!current.empty())
			tokens.push_back(std::move(current));
		return tokens;
	}

	/** @brief Whole-token match: "inn" matches "whiterun\inn01" but never "skinned". */
	bool AnyToken(const std::vector<std::string>& a_tokens, std::initializer_list<std::string_view> a_set)
	{
		for (const auto& token : a_tokens)
			for (const auto candidate : a_set)
				if (token == candidate)
					return true;
		return false;
	}

	/** @brief Word inside a token: vanilla names prefix a region code ("wrwindows01", "swindow01", "whwindow1"). */
	bool AnyTokenContaining(const std::vector<std::string>& a_tokens, std::initializer_list<std::string_view> a_words)
	{
		for (const auto& token : a_tokens)
			for (const auto word : a_words)
				if (token.find(word) != std::string::npos)
					return true;
		return false;
	}

	bool ContainsAny(std::string_view a_text, const std::vector<std::string>& a_needles)
	{
		for (const auto& needle : a_needles)
			if (!needle.empty() && a_text.find(needle) != std::string_view::npos)
				return true;
		return false;
	}

	float SmoothRange(float a_edge0, float a_edge1, float a_x)
	{
		if (a_edge1 <= a_edge0)
			return a_x >= a_edge1 ? 1.0f : 0.0f;
		const float t = std::clamp((a_x - a_edge0) / (a_edge1 - a_edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	// Specific phrases, matched as substrings: shadow-caster proxies, pane masks and closed windows are
	// never glass even when their files carry "window" in the name.
	constexpr std::array<std::string_view, 14> kExcludedPhrases{
		"windowshadow", "window_shadow", "\\masks\\", "/masks/", "_mask.", "shutter", "boarded",
		"closedwindow", "windowclosed", "window_closed", "windowboard", "window_board", "\\effects\\", "\\lod\\"
	};

	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
		{
			auto& livingWindows = globals::features::livingWindows;
			if (livingWindows.loaded)
				livingWindows.UpdatePermutation(a_pass);
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

#define I18N_KEY_PREFIX "feature.living_windows."

void LivingWindows::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("enabled_tooltip"), "Applies to recognized architectural glass. Frames, shutters and boarded windows are left untouched."));

	ImGui::SeparatorText(T(TKEY("glass"), "Glass"));
	ImGui::Checkbox(T(TKEY("glass_optics"), "Old Glass Optics"), &settings.GlassOptics);
	ImGui::SliderFloat(T(TKEY("glass_roughness"), "Glass Roughness"), &settings.GlassRoughness, 0.02f, 0.6f, "%.2f");
	ImGui::SliderFloat(T(TKEY("glass_reflectance"), "Reflectance"), &settings.GlassReflectance, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("grime"), "Grime"), &settings.GrimeStrength, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("waviness"), "Waviness"), &settings.Waviness, 0.0f, 1.0f, "%.2f");
	ImGui::Checkbox(T(TKEY("rain_droplets"), "Rain On Glass"), &settings.RainDroplets);
	ImGui::SliderFloat(T(TKEY("droplet_strength"), "Droplet Strength"), &settings.DropletStrength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("waviness_tooltip"), "Uneven hand-made glass: gently bends reflections across the pane."));

	ImGui::SeparatorText(T(TKEY("interiors"), "Interiors"));
	ImGui::Checkbox(T(TKEY("rooms"), "Rooms Behind Windows"), &settings.Interiors);
	ImGui::SliderFloat(T(TKEY("room_depth"), "Room Depth"), &settings.RoomDepth, 0.4f, 4.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("room_depth_tooltip"), "How deep rooms reach behind the glass, relative to the window width."));
	ImGui::SliderFloat(T(TKEY("interior_brightness"), "Interior Brightness"), &settings.InteriorBrightness, 0.0f, 3.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("night_lit_share"), "Lit Windows at Night"), &settings.NightLitShare, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("night_lit_share_tooltip"), "Share of windows with lights on after dusk. Each lit window switches off at its own hour late at night."));
	ImGui::Checkbox(T(TKEY("curtains"), "Curtains"), &settings.Curtains);
	ImGui::SliderFloat(T(TKEY("curtain_share"), "Windows With Curtains"), &settings.CurtainShare, 0.0f, 1.0f, "%.2f");
	ImGui::Checkbox(T(TKEY("occupants"), "Occupants"), &settings.Occupants);
	ImGui::SliderFloat(T(TKEY("occupant_activity"), "Occupant Activity"), &settings.OccupantActivity, 0.0f, 1.0f, "%.2f");
	ImGui::Checkbox(T(TKEY("outdoor_views"), "Outdoor Views From Inside"), &settings.OutdoorViews);
	ImGui::TextDisabled(T(TKEY("window_mesh_stats"), "Window meshes read: %u from memory, %u from the GPU, %u unreadable, %u pending; %u windows found"),
		windowMeshesFromCpu, windowMeshesFromGpu, windowMeshesUnreadable, static_cast<std::uint32_t>(pendingReadbacks.size()), windowsFound);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("window_mesh_stats_tooltip"), "Each house mesh is split into its separate windows so every window gets its own room, curtains and occupants. Meshes that could not be split fall back to a grid of rooms without curtains or occupants."));
	ImGui::SliderFloat(T(TKEY("outdoor_brightness"), "Outdoor View Brightness"), &settings.OutdoorBrightness, 0.0f, 3.0f, "%.2f");

	ImGui::SeparatorText(T(TKEY("detection"), "Detection"));
	ImGui::SliderFloat(T(TKEY("min_room_radius"), "Minimum Size for Rooms"), &settings.MinRoomRadius, 0.0f, 128.0f, "%.0f");
	ImGui::SliderFloat(T(TKEY("min_occupant_radius"), "Minimum Size for Occupants"), &settings.MinOccupantRadius, 0.0f, 256.0f, "%.0f");

	ImGui::SeparatorText(T(TKEY("shafts_header"), "Light Shafts"));
	ImGui::Checkbox(T(TKEY("shafts_enable"), "Window Light Shafts"), &settings.LightShafts);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("shafts_enable_tooltip"), "Volumetric sun beams entering interiors through the windows this feature tracks. Depth-bounded, so walls occlude them."));
	}
	if (settings.LightShafts) {
		ImGui::SliderFloat(T(TKEY("shafts_intensity"), "Shaft Intensity"), &settings.ShaftIntensity, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("shafts_length"), "Shaft Length"), &settings.ShaftLength, 200.0f, 2500.0f, "%.0f");
		ImGui::SliderFloat(T(TKEY("shafts_density"), "Shaft Density"), &settings.ShaftDensity, 0.1f, 3.0f, "%.2f");
	}
	settings.MinOccupantRadius = std::max(settings.MinOccupantRadius, settings.MinRoomRadius);
	ImGui::SliderFloat(T(TKEY("fade_start"), "Fade Start"), &settings.FadeStart, 256.0f, 16000.0f, "%.0f");
	ImGui::SliderFloat(T(TKEY("fade_end"), "Fade End"), &settings.FadeEnd, 512.0f, 24000.0f, "%.0f");
	settings.FadeEnd = std::max(settings.FadeEnd, settings.FadeStart + 1.0f);
	ImGui::Text(T(TKEY("rule_files"), "Rule files loaded: %d"), static_cast<int>(ruleFileCount));
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("rule_files_tooltip"), "JSON files in Data/SKSE/Plugins/CommunityShaders/LivingWindows with \"windowTokens\" and \"excludeTokens\" lists extend detection for modded buildings."));
	ImGui::Text(T(TKEY("classified_materials"), "Classified materials: %d"), static_cast<int>(classificationCache.size()));
	if (ImGui::Button(T(TKEY("rescan"), "Re-scan Windows"))) {
		LoadRuleFiles();
		RefreshUserTokens();
		classificationDirty = true;
	}
	ImGui::Checkbox(T(TKEY("debug_overlay"), "Show Detected Windows"), &settings.DebugOverlay);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("debug_overlay_tooltip"), "Blue: glass only. Amber: room. Green: room with occupants."));

	if (!(atlasFlags & kAtlasRoom))
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", T(TKEY("missing_atlases"), "Room artwork is missing; only glass optics are available."));
}

#undef I18N_KEY_PREFIX

void LivingWindows::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.FadeEnd = std::max(settings.FadeEnd, settings.FadeStart + 1.0f);
	settings.MinOccupantRadius = std::max(settings.MinOccupantRadius, settings.MinRoomRadius);
	RefreshUserTokens();
	classificationDirty = true;
}

void LivingWindows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LivingWindows::RestoreDefaultSettings()
{
	settings = {};
	RefreshUserTokens();
	classificationDirty = true;
}

void LivingWindows::RefreshUserTokens()
{
	for (auto& token : settings.ExtraWindowTokens)
		token = Lower(token);
	for (auto& token : settings.ExcludedTokens)
		token = Lower(token);
}

void LivingWindows::LoadRuleFiles()
{
	windowRuleTokens.clear();
	excludedRuleTokens.clear();
	ruleFileCount = 0;

	const std::filesystem::path root = "Data\\SKSE\\Plugins\\CommunityShaders\\LivingWindows";
	std::error_code ec;
	if (!std::filesystem::is_directory(root, ec))
		return;

	for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
		if (Lower(entry.path().extension().string()) != ".json")
			continue;
		try {
			std::ifstream stream(entry.path());
			const json rules = json::parse(stream, nullptr, true, true);
			for (const auto& token : rules.value("windowTokens", std::vector<std::string>{}))
				windowRuleTokens.push_back(Lower(token));
			for (const auto& token : rules.value("excludeTokens", std::vector<std::string>{}))
				excludedRuleTokens.push_back(Lower(token));
			++ruleFileCount;
		} catch (const std::exception& e) {
			logger::warn("[Living Windows] Ignoring rule file {}: {}", entry.path().filename().string(), e.what());
		}
	}
	logger::info("[Living Windows] Loaded {} rule files ({} window tokens, {} exclusions)", ruleFileCount, windowRuleTokens.size(), excludedRuleTokens.size());
}

void LivingWindows::LoadAtlases()
{
	struct AtlasSource
	{
		const char* file;
		bool srgb;
		uint flag;
		const char* name;
	};
	// Slot order matches the registers in LivingWindows.hlsli (t104-t109).
	constexpr std::array<AtlasSource, 6> sources{ {
		{ "RoomAtlas.dds", true, kAtlasRoom, "LivingWindows::RoomAtlas" },
		{ "CurtainAtlas.dds", true, kAtlasCurtain, "LivingWindows::CurtainAtlas" },
		{ "OccupantAtlas.png", true, kAtlasOccupant, "LivingWindows::OccupantAtlas" },
		{ "OutdoorAtlasDay.dds", true, kAtlasOutdoorDay, "LivingWindows::OutdoorAtlasDay" },
		{ "OutdoorAtlasNight.dds", true, kAtlasOutdoorNight, "LivingWindows::OutdoorAtlasNight" },
		{ "GlassGrime.png", false, kAtlasGrime, "LivingWindows::GlassGrime" },
	} };

	atlasFlags = 0;
	auto device = globals::d3d::device;
	for (std::size_t i = 0; i < sources.size(); ++i) {
		const auto& source = sources[i];
		atlasSRVs[i] = nullptr;
		const std::filesystem::path path = std::filesystem::path("Data\\Shaders\\LivingWindows") / source.file;

		DirectX::ScratchImage image;
		DirectX::TexMetadata metadata{};
		HRESULT hr = E_FAIL;
		if (Lower(path.extension().string()) == ".dds") {
			hr = DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
		} else {
			hr = DirectX::LoadFromWICFile(path.c_str(), source.srgb ? DirectX::WIC_FLAGS_FORCE_SRGB : DirectX::WIC_FLAGS_FORCE_RGB, &metadata, image);
			if (SUCCEEDED(hr) && metadata.mipLevels <= 1) {
				DirectX::ScratchImage mips;
				if (SUCCEEDED(DirectX::GenerateMipMaps(image.GetImages(), image.GetImageCount(), metadata, DirectX::TEX_FILTER_DEFAULT | DirectX::TEX_FILTER_SEPARATE_ALPHA, 0, mips))) {
					image = std::move(mips);
					metadata = image.GetMetadata();
				}
			}
		}
		if (FAILED(hr)) {
			logger::warn("[Living Windows] Could not load {} (HRESULT 0x{:08X})", path.string(), static_cast<std::uint32_t>(hr));
			continue;
		}

		if (source.srgb) {
			const DXGI_FORMAT srgbFormat = DirectX::MakeSRGB(metadata.format);
			if (srgbFormat != metadata.format && image.OverrideFormat(srgbFormat))
				metadata = image.GetMetadata();
		}

		hr = DirectX::CreateShaderResourceView(device, image.GetImages(), image.GetImageCount(), metadata, atlasSRVs[i].put());
		if (FAILED(hr) || !atlasSRVs[i]) {
			logger::warn("[Living Windows] Could not create a view for {} (HRESULT 0x{:08X})", path.string(), static_cast<std::uint32_t>(hr));
			atlasSRVs[i] = nullptr;
			continue;
		}
		Util::SetResourceName(atlasSRVs[i].get(), std::format("{} SRV", source.name).c_str());
		atlasFlags |= source.flag;
	}
	logger::info("[Living Windows] Artwork atlases ready: flags 0x{:02X}", atlasFlags);
}

void LivingWindows::SetupResources()
{
	LoadRuleFiles();
	RefreshUserTokens();
	LoadAtlases();

	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = sizeof(WindowListCB);
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, windowListBuffer.put())))
		logger::warn("[Living Windows] Could not create the window list buffer; rooms fall back to shape bounds");
	else
		Util::SetResourceName(windowListBuffer.get(), "LivingWindows::WindowList");
}

void LivingWindows::UpdateWeatherSmoothing()
{
	// Once per frame. GetCommonBufferData runs again for every shared-data update (reflections,
	// early prepass, main view, upscaling), so advancing these fades there ran them several times
	// per frame and at a rate that changed with the passes a frame happened to take.
	const float deltaTime = globals::game::deltaTime ? *globals::game::deltaTime : 0.016f;

	// Glass wets and dries over a few seconds instead of snapping with the weather flag.
	bool raining = false;
	if (auto* sky = RE::Sky::GetSingleton())
		raining = sky->mode.get() == RE::Sky::Mode::kFull && sky->IsRaining();
	smoothedRain += ((raining ? 1.0f : 0.0f) - smoothedRain) * std::clamp(deltaTime * 0.25f, 0.0f, 1.0f);

	// Frost and condensation are not part of this build, so the glass never ices or mists and
	// nothing here reads Snow Cover. The GPU fields below stay zero and the shader's frost and
	// condensation terms fold away.
}

void LivingWindows::Prepass()
{
	UpdateWeatherSmoothing();
	if (!settings.Enabled)
		return;
	auto context = globals::d3d::context;
	std::array<ID3D11ShaderResourceView*, 6> srvs{};
	for (std::size_t i = 0; i < srvs.size(); ++i)
		srvs[i] = atlasSRVs[i].get();
	context->PSSetShaderResources(kFirstAtlasSlot, static_cast<UINT>(srvs.size()), srvs.data());
	if (auto* buffer = windowListBuffer.get())
		context->PSSetConstantBuffers(kWindowListSlot, 1, &buffer);
	PollWindowReadbacks();
}

std::vector<LivingWindows::WindowBox> LivingWindows::ClusterWindows(const std::uint8_t* a_vertices, std::uint32_t a_stride, bool a_fullPrecision,
	std::uint32_t a_vertexCount, const std::uint16_t* a_indices, std::uint32_t a_triangleCount)
{
	std::vector<WindowBox> result;
	const auto position = [&](std::uint32_t a_index) {
		const std::uint8_t* vertex = a_vertices + static_cast<std::size_t>(a_index) * a_stride;
		if (a_fullPrecision) {
			float p[3];
			std::memcpy(p, vertex, sizeof(p));
			return RE::NiPoint3{ p[0], p[1], p[2] };
		}
		DirectX::PackedVector::HALF h[3];
		std::memcpy(h, vertex, sizeof(h));
		return RE::NiPoint3{ DirectX::PackedVector::XMConvertHalfToFloat(h[0]), DirectX::PackedVector::XMConvertHalfToFloat(h[1]),
			DirectX::PackedVector::XMConvertHalfToFloat(h[2]) };
	};

	// Triangles sharing vertices are one pane.
	std::vector<std::uint32_t> parent(a_vertexCount);
	for (std::uint32_t i = 0; i < a_vertexCount; ++i)
		parent[i] = i;
	const auto find = [&](std::uint32_t a_index) {
		while (parent[a_index] != a_index) {
			parent[a_index] = parent[parent[a_index]];
			a_index = parent[a_index];
		}
		return a_index;
	};
	for (std::uint32_t t = 0; t < a_triangleCount; ++t) {
		const std::uint32_t a = a_indices[t * 3], b = a_indices[t * 3 + 1], c = a_indices[t * 3 + 2];
		if (a >= a_vertexCount || b >= a_vertexCount || c >= a_vertexCount)
			return result;
		const std::uint32_t ra = find(a), rb = find(b), rc = find(c);
		parent[ra] = rc;
		parent[rb] = rc;
	}

	struct Box
	{
		RE::NiPoint3 min{ FLT_MAX, FLT_MAX, FLT_MAX };
		RE::NiPoint3 max{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
	};
	std::unordered_map<std::uint32_t, Box> panes;
	for (std::uint32_t t = 0; t < a_triangleCount * 3; ++t) {
		const std::uint32_t v = a_indices[t];
		auto& box = panes[find(v)];
		const RE::NiPoint3 p = position(v);
		box.min = { std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z) };
		box.max = { std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z) };
	}
	if (panes.size() > 1024)
		return result;

	// Panes separated by less than a mullion are one window. Measured on the load order's house
	// meshes: mullions and leading leave 5 to 10 units between panes, separate windows 28 or more.
	constexpr float kWindowGap = 16.0f;
	int sweepsLeft = 32;
	std::vector<Box> windows;
	windows.reserve(panes.size());
	for (const auto& [root, box] : panes)
		windows.push_back(box);
	const auto touches = [](const Box& a, const Box& b) {
		return a.min.x - kWindowGap <= b.max.x && b.min.x - kWindowGap <= a.max.x &&
		       a.min.y - kWindowGap <= b.max.y && b.min.y - kWindowGap <= a.max.y &&
		       a.min.z - kWindowGap <= b.max.z && b.min.z - kWindowGap <= a.max.z;
	};
	// Each box absorbs everything it touches in one sweep; sweeps repeat until nothing merges. The
	// sweep count is bounded: a mesh whose boxes chain endlessly must not stall the render thread.
	for (bool merged = true; merged && sweepsLeft-- > 0;) {
		merged = false;
		for (std::size_t i = 0; i < windows.size(); ++i) {
			for (std::size_t j = i + 1; j < windows.size();) {
				if (touches(windows[i], windows[j])) {
					const Box b = windows[j];
					windows[i].min = { std::min(windows[i].min.x, b.min.x), std::min(windows[i].min.y, b.min.y), std::min(windows[i].min.z, b.min.z) };
					windows[i].max = { std::max(windows[i].max.x, b.max.x), std::max(windows[i].max.y, b.max.y), std::max(windows[i].max.z, b.max.z) };
					windows[j] = windows.back();
					windows.pop_back();
					merged = true;
				} else {
					++j;
				}
			}
		}
	}

	for (const auto& box : windows) {
		const RE::NiPoint3 half = (box.max - box.min) * 0.5f;
		const float largest = std::max({ half.x, half.y, half.z }) * 2.0f;
		// Slivers and whole-facade sheets are not windows a room can be framed on.
		if (largest < 8.0f || largest > 1200.0f)
			continue;
		result.push_back({ (box.min + box.max) * 0.5f, half });
	}
	return result;
}

const LivingWindows::ShapeWindows* LivingWindows::GetShapeWindows(RE::BSGeometry* a_geometry)
{
	auto* triShape = a_geometry ? a_geometry->AsTriShape() : nullptr;
	auto* rendererData = a_geometry ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
	if (!triShape || !rendererData)
		return nullptr;

	const auto& vertexDesc = rendererData->vertexDesc;
	const std::uint64_t desc = *reinterpret_cast<const std::uint64_t*>(&vertexDesc);
	const std::uint32_t stride = static_cast<std::uint32_t>(desc & 0xF) * 4;
	const bool fullPrecision = vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
	const std::uint32_t vertexCount = triShape->GetTrishapeRuntimeData().vertexCount;
	const std::uint32_t triangleCount = triShape->GetTrishapeRuntimeData().triangleCount;
	// Glass is a handful of quads; anything skinned, empty or huge is not a window mesh worth reading.
	if (!vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED) ||
		stride < (fullPrecision ? 12u : 6u) || vertexCount == 0 || triangleCount == 0 || triangleCount > 8192)
		return nullptr;

	// The renderer data is freed with its model and its address handed to the next one, so a hit is
	// only trusted while the mesh still has the same size and layout.
	if (auto it = shapeWindowCache.find(rendererData); it != shapeWindowCache.end()) {
		const auto& cached = it->second;
		if (cached.vertexCount == vertexCount && cached.triangleCount == triangleCount && cached.vertexDesc == desc)
			return &cached;
	}
	if (shapeWindowCache.size() > 8192)
		shapeWindowCache.clear();

	if (rendererData->rawVertexData && rendererData->rawIndexData) {
		ShapeWindows result{ vertexCount, triangleCount, desc,
			ClusterWindows(rendererData->rawVertexData, stride, fullPrecision, vertexCount, rendererData->rawIndexData, triangleCount) };
		++windowMeshesFromCpu;
		windowsFound += static_cast<std::uint32_t>(result.windows.size());
		return &shapeWindowCache.insert_or_assign(rendererData, std::move(result)).first->second;
	}

	// Static meshes usually drop their CPU copy once uploaded: read the GPU buffers back instead,
	// a few frames later and without ever waiting on the GPU. Until then the room is framed on the
	// shape's bounds.
	QueueWindowReadback(rendererData, vertexCount, triangleCount, desc, stride, fullPrecision);
	return nullptr;
}

void LivingWindows::QueueWindowReadback(RE::BSGraphics::TriShape* a_data, std::uint32_t a_vertexCount, std::uint32_t a_triangleCount,
	std::uint64_t a_desc, std::uint32_t a_stride, bool a_fullPrecision)
{
	if (pendingReadbacks.contains(a_data) || pendingReadbacks.size() >= 48)
		return;

	const auto giveUp = [&] {
		++windowMeshesUnreadable;
		shapeWindowCache.insert_or_assign(a_data, ShapeWindows{ a_vertexCount, a_triangleCount, a_desc, {} });
	};
	auto* vertexBuffer = reinterpret_cast<ID3D11Buffer*>(a_data->vertexBuffer);
	auto* indexBuffer = reinterpret_cast<ID3D11Buffer*>(a_data->indexBuffer);
	if (!vertexBuffer || !indexBuffer)
		return giveUp();

	D3D11_BUFFER_DESC vertexDesc{};
	D3D11_BUFFER_DESC indexDesc{};
	vertexBuffer->GetDesc(&vertexDesc);
	indexBuffer->GetDesc(&indexDesc);
	if (vertexDesc.ByteWidth < a_vertexCount * a_stride || indexDesc.ByteWidth < a_triangleCount * 3 * sizeof(std::uint16_t))
		return giveUp();

	PendingReadback pending{};
	pending.vertexCount = a_vertexCount;
	pending.triangleCount = a_triangleCount;
	pending.desc = a_desc;
	pending.stride = a_stride;
	pending.fullPrecision = a_fullPrecision;
	pending.frame = globals::state->frameCount;

	auto device = globals::d3d::device;
	D3D11_BUFFER_DESC staging{};
	staging.Usage = D3D11_USAGE_STAGING;
	staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	staging.ByteWidth = vertexDesc.ByteWidth;
	if (FAILED(device->CreateBuffer(&staging, nullptr, pending.vertices.put())))
		return giveUp();
	staging.ByteWidth = indexDesc.ByteWidth;
	if (FAILED(device->CreateBuffer(&staging, nullptr, pending.indices.put())))
		return giveUp();

	auto context = globals::d3d::context;
	context->CopyResource(pending.vertices.get(), vertexBuffer);
	context->CopyResource(pending.indices.get(), indexBuffer);
	pendingReadbacks.emplace(a_data, std::move(pending));
}

void LivingWindows::PollWindowReadbacks()
{
	const std::uint32_t frame = globals::state->frameCount;
	auto context = globals::d3d::context;
	for (auto it = pendingReadbacks.begin(); it != pendingReadbacks.end();) {
		auto& pending = it->second;
		if (frame - pending.frame < 2) {
			++it;
			continue;
		}
		D3D11_MAPPED_SUBRESOURCE vertices{};
		const HRESULT vertexResult = context->Map(pending.vertices.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &vertices);
		if (vertexResult == DXGI_ERROR_WAS_STILL_DRAWING) {
			++it;
			continue;
		}
		D3D11_MAPPED_SUBRESOURCE indices{};
		const HRESULT indexResult = SUCCEEDED(vertexResult) ? context->Map(pending.indices.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &indices) : vertexResult;
		if (indexResult == DXGI_ERROR_WAS_STILL_DRAWING) {
			context->Unmap(pending.vertices.get(), 0);
			++it;
			continue;
		}

		ShapeWindows result{ pending.vertexCount, pending.triangleCount, pending.desc, {} };
		if (SUCCEEDED(vertexResult) && SUCCEEDED(indexResult)) {
			result.windows = ClusterWindows(static_cast<const std::uint8_t*>(vertices.pData), pending.stride, pending.fullPrecision, pending.vertexCount,
				static_cast<const std::uint16_t*>(indices.pData), pending.triangleCount);
			++windowMeshesFromGpu;
			windowsFound += static_cast<std::uint32_t>(result.windows.size());
		} else {
			++windowMeshesUnreadable;
		}
		if (SUCCEEDED(indexResult))
			context->Unmap(pending.indices.get(), 0);
		if (SUCCEEDED(vertexResult))
			context->Unmap(pending.vertices.get(), 0);

		shapeWindowCache.insert_or_assign(it->first, std::move(result));
		it = pendingReadbacks.erase(it);
	}
}

void LivingWindows::PublishWindowList(RE::BSGeometry* a_geometry, const ShapeWindows* a_windows)
{
	auto* buffer = windowListBuffer.get();
	if (!buffer)
		return;

	WindowListCB list{};
	if (a_geometry && a_windows && !a_windows->windows.empty()) {
		const auto& world = a_geometry->world;
		const RE::NiPoint3 eye = Util::GetEyePosition();

		// Past the limit, the windows nearest the camera are the ones anyone can see into.
		const auto& windows = a_windows->windows;
		windowSortScratch.clear();
		for (std::uint32_t i = 0; i < windows.size(); ++i)
			windowSortScratch.emplace_back((world * windows[i].center - eye).SqrLength(), i);
		const std::size_t count = std::min<std::size_t>(windowSortScratch.size(), kMaxWindowsPerDraw);
		std::partial_sort(windowSortScratch.begin(), windowSortScratch.begin() + static_cast<std::ptrdiff_t>(count), windowSortScratch.end());

		const auto& m = world.rotate.entry;
		for (std::size_t k = 0; k < count; ++k) {
			const auto& window = windows[windowSortScratch[k].second];
			const RE::NiPoint3 center = world * window.center;
			const RE::NiPoint3& h = window.halfExtent;
			list.Center[k] = float4(center.x, center.y, center.z, 0.0f);
			{
				// Shaft recording: lateral radius from the two largest extents, pane normal
				// as the axis of the smallest one (windows are thin along their normal).
				const float wx = world.scale * h.x, wy = world.scale * h.y, wz = world.scale * h.z;
				const float mn = std::min({ wx, wy, wz });
				RE::NiPoint3 normal = mn == wx ? RE::NiPoint3{ 1, 0, 0 } : (mn == wy ? RE::NiPoint3{ 0, 1, 0 } : RE::NiPoint3{ 0, 0, 1 });
				normal = world.rotate * normal;
				const float lateral = 0.5f * (wx + wy + wz - mn);
				RecordShaftWindow(center, std::max(lateral, 8.0f), normal);
			}
			list.HalfExtent[k] = float4(
				world.scale * (std::abs(m[0][0]) * h.x + std::abs(m[0][1]) * h.y + std::abs(m[0][2]) * h.z),
				world.scale * (std::abs(m[1][0]) * h.x + std::abs(m[1][1]) * h.y + std::abs(m[1][2]) * h.z),
				world.scale * (std::abs(m[2][0]) * h.x + std::abs(m[2][1]) * h.y + std::abs(m[2][2]) * h.z),
				0.0f);
		}
		list.Count = static_cast<std::uint32_t>(count);
	}

	auto context = globals::d3d::context;
	if (!windowListPublished || std::memcmp(&list, &publishedWindows, sizeof(WindowListCB)) != 0) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;
		std::memcpy(mapped.pData, &list, sizeof(WindowListCB));
		context->Unmap(buffer, 0);
		publishedWindows = list;
		windowListPublished = true;
	}
	context->PSSetConstantBuffers(kWindowListSlot, 1, &buffer);
}

void LivingWindows::PostPostLoad()
{
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[Living Windows] Installed window classification hook");
}

LivingWindows::GPUSettings LivingWindows::GetCommonBufferData()
{
	float hour = 12.0f;
	if (auto* calendar = RE::Calendar::GetSingleton())
		hour = calendar->GetHour();

	float sunriseBegin = 5.0f;
	float sunriseEnd = 7.0f;
	float sunsetBegin = 17.0f;
	float sunsetEnd = 20.0f;
	if (auto* sky = RE::Sky::GetSingleton(); sky && sky->currentClimate) {
		// Climate timings are stored in ten-minute steps.
		const auto& timing = sky->currentClimate->timing;
		sunriseBegin = timing.sunrise.begin / 6.0f;
		sunriseEnd = timing.sunrise.end / 6.0f;
		sunsetBegin = timing.sunset.begin / 6.0f;
		sunsetEnd = timing.sunset.end / 6.0f;
	}
	const float daylight = SmoothRange(sunriseBegin, sunriseEnd, hour) * (1.0f - SmoothRange(sunsetBegin, sunsetEnd, hour));

	// Rain and frost are eased once per frame in Prepass; this only reads them.
	return GPUSettings{
		.Enabled = settings.Enabled,
		.GlassOptics = settings.GlassOptics,
		.Interiors = settings.Interiors && (atlasFlags & kAtlasRoom),
		.Occupants = settings.Occupants,
		.Curtains = settings.Curtains,
		.OutdoorViews = settings.OutdoorViews,
		.DebugOverlay = settings.DebugOverlay,
		.AtlasFlags = atlasFlags,
		.RoomDepth = std::clamp(settings.RoomDepth, 0.1f, 8.0f),
		.InteriorBrightness = std::max(settings.InteriorBrightness, 0.0f),
		.NightLitShare = std::clamp(settings.NightLitShare, 0.0f, 1.0f),
		.OccupantActivity = std::clamp(settings.OccupantActivity, 0.0f, 1.0f),
		.CurtainShare = std::clamp(settings.CurtainShare, 0.0f, 1.0f),
		.GlassRoughness = std::clamp(settings.GlassRoughness, 0.02f, 1.0f),
		.GlassReflectance = std::max(settings.GlassReflectance, 0.0f),
		.GrimeStrength = std::clamp(settings.GrimeStrength, 0.0f, 1.0f),
		.Waviness = std::clamp(settings.Waviness, 0.0f, 1.0f),
		.FadeStart = std::max(settings.FadeStart, 0.0f),
		.FadeEnd = std::max(settings.FadeEnd, settings.FadeStart + 1.0f),
		.OutdoorBrightness = std::max(settings.OutdoorBrightness, 0.0f),
		.NightBlend = 1.0f - daylight,
		.Raining = smoothedRain,
		.GameHour = hour,
		.DropletStrength = settings.RainDroplets ? std::clamp(settings.DropletStrength, 0.0f, 1.0f) : 0.0f,
		// Frost and condensation are out of this build; the shader folds these away at zero.
		.FrostStrength = 0.0f,
		.FrostAmount = 0.0f,
		.CondensationStrength = 0.0f,
		.pad0 = 0.0f
	};
}

LivingWindows::Classification LivingWindows::ClassifyMaterial(const RE::BSLightingShaderMaterialBase* a_material) const
{
	Classification result{};
	auto* textureSet = a_material->textureSet.get();
	if (!textureSet)
		return result;

	const auto texturePath = [textureSet](RE::BSTextureSet::Texture a_slot) {
		const char* raw = textureSet->GetTexturePath(a_slot);
		return Lower(raw ? raw : "");
	};
	const std::string diffuse = texturePath(RE::BSTextureSet::Texture::kDiffuse);
	const std::string glow = texturePath(RE::BSTextureSet::Texture::kGlowMap);
	if (diffuse.empty())
		return result;
	const std::string combined = diffuse + "|" + glow;

	for (const auto phrase : kExcludedPhrases)
		if (combined.find(phrase) != std::string::npos)
			return result;
	if (ContainsAny(combined, settings.ExcludedTokens) || ContainsAny(combined, excludedRuleTokens))
		return result;

	std::vector<std::string> tokens = Tokenize(diffuse);
	const auto glowTokens = Tokenize(glow);
	tokens.insert(tokens.end(), glowTokens.begin(), glowTokens.end());

	const bool userWindow = ContainsAny(combined, settings.ExtraWindowTokens) || ContainsAny(combined, windowRuleTokens);
	// Short words stay whole-token matches; "window" and "glass" are long enough to find inside
	// region-prefixed names without dragging in unrelated textures.
	const bool windowToken = userWindow || AnyTokenContaining(tokens, { "window", "glazing" }) || AnyToken(tokens, { "pane", "panes" });
	const bool glassToken = AnyTokenContaining(tokens, { "glass" });
	const bool nonBuilding = AnyToken(tokens, { "bottle", "bottles", "potion", "potions", "alchemy", "clutter", "weapon", "weapons",
		"armor", "armour", "shield", "crystal", "crystals", "gem", "gems", "soulgem", "ice", "eye", "eyes", "water", "magic", "spell",
		"effect", "effects", "lantern", "lanterns", "chandelier", "candle", "candles", "torch", "lamp", "lamps", "jar", "jars",
		"goblet", "cup", "bowl", "flask", "vial", "lens" });
	const bool architecture = AnyToken(tokens, { "architecture", "farmhouse", "whiterun", "solitude", "windhelm", "riften", "markarth",
		"winterhold", "falkreath", "morthal", "dawnstar", "ravenrock", "college", "castle", "house", "houses", "inn", "shop", "store",
		"temple", "fort", "building", "buildings", "village", "town", "city", "exterior", "facade", "dwemer", "nordic", "imperial",
		"manor", "tower", "chapel", "church", "hall", "keep", "palace", "tavern", "guild", "solstheim", "skaal" });
	const bool hasGlow = a_material->GetFeature() == RE::BSShaderMaterial::Feature::kGlowMap && !glow.empty();

	if (nonBuilding && !userWindow)
		return result;

	if (windowToken) {
		result.tier = 3;
		result.explicitWindow = true;
	} else if (glassToken && architecture) {
		result.tier = 2;
	} else if (glassToken) {
		result.tier = 1;
	} else if (hasGlow && architecture) {
		result.checkMeshName = true;
	}

	if (AnyToken(tokens, { "markarth", "dwemer" }))
		result.family = 4;
	else if (AnyToken(tokens, { "solitude", "castle", "imperial", "palace", "noble", "manor" }))
		result.family = 1;
	else if (AnyToken(tokens, { "riften", "canal" }))
		result.family = 2;
	else if (AnyToken(tokens, { "windhelm", "snowquarter" }))
		result.family = 3;
	else if (AnyToken(tokens, { "inn", "shop", "tavern", "merchant", "store", "guild" }))
		result.family = 5;

	return result;
}

const LivingWindows::Classification& LivingWindows::GetClassification(const RE::BSLightingShaderMaterialBase* a_material)
{
	// hashKey is derived from the material's textures and parameters, so equal keys mean equal
	// verdicts. Materials without one are keyed by their texture set's interned path pointers:
	// a freed material's address is handed to the next one allocated, which would inherit a
	// verdict meant for different textures, while the string pool keeps one entry per path.
	const bool hasHash = a_material->hashKey != std::numeric_limits<std::uint32_t>::max();
	std::uint64_t key;
	if (hasHash) {
		key = static_cast<std::uint64_t>(a_material->hashKey);
	} else {
		std::uint64_t pathKey = 14695981039346656037ull;
		if (auto* textureSet = a_material->textureSet.get()) {
			for (const auto slot : { RE::BSTextureSet::Texture::kDiffuse, RE::BSTextureSet::Texture::kGlowMap }) {
				const auto* path = textureSet->GetTexturePath(slot);
				pathKey ^= reinterpret_cast<std::uint64_t>(path);
				pathKey *= 1099511628211ull;
			}
		}
		key = (1ull << 63) | (pathKey >> 1);
	}

	if (auto it = classificationCache.find(key); it != classificationCache.end())
		return it->second;

	const auto verdict = ClassifyMaterial(a_material);
	if (settings.DebugOverlay && (verdict.tier || verdict.checkMeshName) && loggedClassifications < 512) {
		++loggedClassifications;
		const char* diffuse = a_material->textureSet ? a_material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
		logger::info("[Living Windows] tier {} family {} meshCheck {} '{}'", verdict.tier, verdict.family, verdict.checkMeshName, diffuse ? diffuse : "");
	}
	return classificationCache.emplace(key, verdict).first->second;
}

std::uint32_t LivingWindows::TierFromMeshName(const char* a_name) const
{
	if (!a_name || !*a_name)
		return 0;
	const auto tokens = Tokenize(a_name);
	if (AnyTokenContaining(tokens, { "window" }))
		return 3;
	if (AnyTokenContaining(tokens, { "glass", "glazing" }) || AnyToken(tokens, { "pane", "panes" }))
		return 2;
	return 0;
}

void LivingWindows::RecordShaftWindow(const RE::NiPoint3& a_center, float a_radius, const RE::NiPoint3& a_normal)
{
	// Render-thread only (the forward draw hook); DrawLightShafts consumes and clears later
	// in the same frame on the same thread.
	if (!settings.LightShafts || a_radius <= 0.0f || shaftWindows.size() >= 256)
		return;
	shaftWindows.push_back({ a_center, a_radius, a_normal });
}

namespace
{
	// Must mirror ShaftCB in WindowShafts.hlsl.
	struct alignas(16) ShaftCBData
	{
		float4 CamDir;
		float4 CamRight;
		float4 CamUp;
		float4 Slopes;
		float4 DepthParams;
		float4 SunDir;
		float4 SunColor;
		float4 Dims;
		float4 Dims2;
		float4 Params;
		float4 WindowCenter[16];
		float4 WindowNormal[16];
	};
}

void LivingWindows::DrawLightShafts()
{
	auto windows = std::move(shaftWindows);
	shaftWindows.clear();

	if (!loaded || !settings.Enabled || !settings.LightShafts || windows.empty() || !Util::IsInterior())
		return;

	auto* sky = globals::game::sky;
	if (!sky)
		return;
	const RE::NiPoint3 toSun = globals::features::skySync.GetCelestialDirection(sky, SkySync::Caster::Sun);
	if (toSun.z <= 0.05f)
		return;  // sun below or grazing the horizon: no beams

	auto* device = globals::d3d::device;
	auto* context = globals::d3d::context;
	auto* cam = RE::Main::WorldRootCamera();
	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	auto* deferred = globals::deferred;
	if (!device || !context || !cam || !depthSRV || !deferred || !deferred->linearSampler)
		return;

	// Lazy resources: shaders from the feature file, a half-res beam buffer, additive blend.
	if (!shaftInitTried) {
		shaftInitTried = true;
		const auto path = L"Data\\Shaders\\LivingWindows\\WindowShafts.hlsl";
		shaftRenderCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path, {}, "cs_5_0", "renderShafts")));
		shaftVS.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(path, {}, "vs_5_0", "shaftVS")));
		shaftPS.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(path, {}, "ps_5_0", "shaftPS")));

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(ShaftCBData);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		device->CreateBuffer(&cbDesc, nullptr, shaftCB.put());
		Util::SetResourceName(shaftCB.get(), "LivingWindows::ShaftCB");

		D3D11_BLEND_DESC blendDesc{};
		blendDesc.RenderTarget[0].BlendEnable = true;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		device->CreateBlendState(&blendDesc, shaftBlend.put());
		Util::SetResourceName(shaftBlend.get(), "LivingWindows::ShaftBlend");

		auto* renderer = globals::game::renderer;
		if (renderer) {
			auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
			D3D11_TEXTURE2D_DESC mainDesc{};
			static_cast<ID3D11Texture2D*>(main.texture)->GetDesc(&mainDesc);
			D3D11_TEXTURE2D_DESC texDesc{};
			texDesc.Width = std::max(mainDesc.Width / 2, 1u);
			texDesc.Height = std::max(mainDesc.Height / 2, 1u);
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			texDesc.SampleDesc = { 1, 0 };
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
			if (SUCCEEDED(device->CreateTexture2D(&texDesc, nullptr, shaftTex.put()))) {
				Util::SetResourceName(shaftTex.get(), "LivingWindows::ShaftTex");
				device->CreateUnorderedAccessView(shaftTex.get(), nullptr, shaftUAV.put());
				Util::SetResourceName(shaftUAV.get(), "LivingWindows::ShaftTex UAV");
				device->CreateShaderResourceView(shaftTex.get(), nullptr, shaftSRV.put());
				Util::SetResourceName(shaftSRV.get(), "LivingWindows::ShaftTex SRV");
				shaftTexW = static_cast<int>(texDesc.Width);
				shaftTexH = static_cast<int>(texDesc.Height);
			}
		}
	}
	if (!shaftRenderCS || !shaftVS || !shaftPS || !shaftCB || !shaftBlend || !shaftUAV)
		return;

	// Keep the nearest panes when the frame recorded more than the shader takes.
	const RE::NiPoint3 camPos = cam->world.translate;
	if (windows.size() > kMaxWindowsPerDraw) {
		std::partial_sort(windows.begin(), windows.begin() + kMaxWindowsPerDraw, windows.end(),
			[&camPos](const ShaftWindow& a, const ShaftWindow& b) {
				return (a.center - camPos).SqrLength() < (b.center - camPos).SqrLength();
			});
		windows.resize(kMaxWindowsPerDraw);
	}

	const auto& R = cam->world.rotate.entry;
	const RE::NiPoint3 dir{ R[0][0], R[1][0], R[2][0] };
	const RE::NiPoint3 up{ R[0][1], R[1][1], R[2][1] };
	const RE::NiPoint3 right{ R[0][2], R[1][2], R[2][2] };
	const RE::NiFrustum& fr = cam->GetRuntimeData2().viewFrustum;
	if (fr.bOrtho || !(fr.fFar > fr.fNear))
		return;
	const float projF = fr.fFar / (fr.fFar - fr.fNear);
	const float projNF = fr.fNear * fr.fFar / (fr.fFar - fr.fNear);

	auto* renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC mainDesc{};
	static_cast<ID3D11Texture2D*>(main.texture)->GetDesc(&mainDesc);
	const float2 dynFull = Util::ConvertToDynamic(float2{ static_cast<float>(mainDesc.Width), static_cast<float>(mainDesc.Height) }, true);
	const int dynW = std::clamp(static_cast<int>(dynFull.x), 2, static_cast<int>(mainDesc.Width));
	const int dynH = std::clamp(static_cast<int>(dynFull.y), 2, static_cast<int>(mainDesc.Height));
	const int halfW = std::min(dynW / 2, shaftTexW);
	const int halfH = std::min(dynH / 2, shaftTexH);

	const RE::NiPoint3 sunDir{ -toSun.x, -toSun.y, -toSun.z };  // travel direction of the light

	ShaftCBData data{};
	data.CamDir = float4(dir.x, dir.y, dir.z, globals::features::reverseZ.IsActive() ? 1.0f : 0.0f);
	data.CamRight = float4(right.x, right.y, right.z, 0.0f);
	data.CamUp = float4(up.x, up.y, up.z, 0.0f);
	data.Slopes = float4(0.5f * (fr.fRight - fr.fLeft), 0.5f * (fr.fRight + fr.fLeft), 0.5f * (fr.fTop - fr.fBottom), 0.5f * (fr.fTop + fr.fBottom));
	data.DepthParams = float4(projF, projNF, settings.ShaftLength * 4.0f, 8.0f);
	data.SunDir = float4(sunDir.x, sunDir.y, sunDir.z, 0.6f);
	data.SunColor = float4(1.0f * settings.ShaftIntensity, 0.92f * settings.ShaftIntensity, 0.78f * settings.ShaftIntensity, 0.15f / std::max(settings.ShaftLength, 1.0f));
	data.Dims = float4(static_cast<float>(halfW), static_cast<float>(halfH), static_cast<float>(dynW), static_cast<float>(dynH));
	data.Dims2 = float4(static_cast<float>(shaftTexW), static_cast<float>(shaftTexH), 0.0f, 0.0f);
	data.Params = float4(settings.ShaftLength, settings.ShaftDensity, static_cast<float>(windows.size()), static_cast<float>(globals::game::graphicsState->frameCount & 1023u));
	for (std::size_t i = 0; i < windows.size(); ++i) {
		const RE::NiPoint3 rel = windows[i].center - camPos;
		data.WindowCenter[i] = float4(rel.x, rel.y, rel.z, windows[i].radius);
		data.WindowNormal[i] = float4(windows[i].normal.x, windows[i].normal.y, windows[i].normal.z, 0.0f);
	}

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(shaftCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return;
	std::memcpy(mapped.pData, &data, sizeof(data));
	context->Unmap(shaftCB.get(), 0);

	TracyD3D11Zone(globals::state->tracyCtx, "LivingWindows - Light Shafts");
	globals::profiler->BeginPass("LivingWindows::LightShafts");

	// Pass 1: half-res beam accumulation.
	ID3D11Buffer* cb = shaftCB.get();
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetShaderResources(0, 1, &depthSRV);
	ID3D11UnorderedAccessView* uav = shaftUAV.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(shaftRenderCS.get(), nullptr, 0);
	context->Dispatch((halfW + 7) / 8, (halfH + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	// Pass 2: additive fullscreen composite onto the main target.
	context->OMSetRenderTargets(1, &main.RTV, nullptr);
	D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mainDesc.Width), static_cast<float>(mainDesc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->OMSetBlendState(shaftBlend.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(nullptr, 0);
	context->IASetInputLayout(nullptr);
	context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(shaftVS.get(), nullptr, 0);
	context->PSSetShader(shaftPS.get(), nullptr, 0);
	context->PSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView* srv = shaftSRV.get();
	context->PSSetShaderResources(0, 1, &srv);
	ID3D11SamplerState* sampler = deferred->linearSampler;
	context->PSSetSamplers(0, 1, &sampler);
	context->Draw(3, 0);

	context->PSSetShaderResources(0, 1, &nullSRV);
	context->VSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	globals::profiler->EndPass();
}

void LivingWindows::UpdatePermutation(RE::BSRenderPass* a_pass)
{
	auto& descriptor = globals::state->permutationData.ExtraFeatureDescriptor;
	auto& bound = globals::state->permutationData.LivingWindowBound;
	descriptor &= ~kDescriptorMask;
	bound = float4(0.0f, 0.0f, 0.0f, 0.0f);
	if (!settings.Enabled || !a_pass || !a_pass->shaderProperty || !a_pass->shaderProperty->material)
		return;

	if (classificationDirty.exchange(false)) {
		classificationCache.clear();
		loggedClassifications = 0;
	}

	const auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
	const auto& verdict = GetClassification(material);

	std::uint32_t tier = verdict.tier;
	if (!tier && verdict.checkMeshName && a_pass->geometry)
		tier = TierFromMeshName(a_pass->geometry->name.c_str());
	if (!tier)
		return;

	if (a_pass->geometry) {
		const auto& worldBound = a_pass->geometry->worldBound;
		const float radius = worldBound.radius;
		if (radius > 0.0f && radius < settings.MinRoomRadius)
			tier = 1;
		else if (radius > 0.0f && radius < settings.MinOccupantRadius)
			tier = std::min<std::uint32_t>(tier, 2);

		// The shader frames rooms from this sphere rather than from the glass texture, which is
		// nearly always an atlas of separate panes. A shape about the size of one room is a single
		// window and gets one room over all of its panes; a much larger one is a facade carrying
		// many windows and gets a repeating grid of rooms. The geometry name settles the band in
		// between. Room sizes per family are those of PIXL WindowLife (GPL-3.0) and must match
		// FamilyRoomSize in LivingWindows.hlsli.
		if (radius > 0.0f) {
			static constexpr std::array<std::pair<float, float>, 6> kFamilyRoomSize{ {
				{ 110.0f, 140.0f }, { 132.0f, 174.0f }, { 118.0f, 138.0f }, { 104.0f, 154.0f }, { 128.0f, 128.0f }, { 126.0f, 146.0f } } };
			const auto [roomWidth, roomHeight] = kFamilyRoomSize[std::min<std::size_t>(verdict.family, kFamilyRoomSize.size() - 1)];
			const float ratio = radius / (0.5f * std::sqrt(roomWidth * roomWidth + roomHeight * roomHeight));
			bool singleWindow = ratio <= 2.75f;
			if (ratio > 2.75f && ratio < 4.25f) {
				const char* geometryName = a_pass->geometry->name.c_str();
				const auto nameTokens = Tokenize(geometryName ? geometryName : "");
				const bool windowName = AnyTokenContaining(nameTokens, { "window", "glass", "pane", "glazing" });
				const bool facadeName = AnyTokenContaining(nameTokens, { "facade", "wall", "house", "building", "exterior" });
				singleWindow = windowName && !facadeName;
			}
			bound = float4(worldBound.center.x, worldBound.center.y, worldBound.center.z, singleWindow ? radius : -radius);
			if (singleWindow)
				RecordShaftWindow(worldBound.center, radius * 0.45f, RE::NiPoint3{ 0.0f, 0.0f, 0.0f });
		}

		// A house mesh carries the glass of all its windows in one shape, so the shape's bounds span
		// the facade. Each window is found in the mesh itself and framed on its own, which is what
		// gives every window of a house its own room, curtains and occupants.
		if (tier >= 2 && settings.Interiors)
			PublishWindowList(a_pass->geometry, GetShapeWindows(a_pass->geometry));
	}

	descriptor |= kCandidateBit | (tier << kTierShift) | (static_cast<std::uint32_t>(verdict.family) << kFamilyShift) |
	              (verdict.explicitWindow ? kExplicitBit : 0);
}
