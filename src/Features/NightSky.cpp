#include "NightSky.h"

#include <algorithm>
#include <cmath>

#include "../I18n/I18n.h"
#include "Globals.h"
#include "Utils/Game.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NightSky::Settings,
	Enabled,
	ReplaceVanillaStars,
	StarBrightness,
	StarDensity,
	Twinkle,
	MilkyWay,
	Airglow,
	Latitude,
	Aurora,
	ReplaceVanillaAurora,
	AuroraIntensity,
	AuroraClearNightChance,
	AuroraColorLow,
	AuroraColorHigh)

namespace
{
	float SmoothRange(float a_edge0, float a_edge1, float a_x)
	{
		if (a_edge1 <= a_edge0)
			return a_x >= a_edge1 ? 1.0f : 0.0f;
		const float t = std::clamp((a_x - a_edge0) / (a_edge1 - a_edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/** @brief Integer hash to 0-1, for the per-night aurora roll. */
	float HashToUnit(std::uint32_t a_value)
	{
		a_value ^= a_value >> 16;
		a_value *= 0x7feb352dU;
		a_value ^= a_value >> 15;
		a_value *= 0x846ca68bU;
		a_value ^= a_value >> 16;
		return static_cast<float>(a_value & 0xFFFFFF) / 16777216.0f;
	}

	bool HasAuroraFlag(const RE::TESWeather* a_weather)
	{
		return a_weather && a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kPermAurora);
	}
}

#define I18N_KEY_PREFIX "feature.night_sky."

void NightSky::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);

	ImGui::SeparatorText(T(TKEY("stars"), "Stars"));
	ImGui::Checkbox(T(TKEY("replace_vanilla_stars"), "Replace Vanilla Stars"), &settings.ReplaceVanillaStars);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("replace_vanilla_stars_tooltip"), "Hides the game's star texture so only the procedural star field shows."));
	ImGui::SliderFloat(T(TKEY("star_brightness"), "Star Brightness"), &settings.StarBrightness, 0.0f, 4.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("star_density"), "Star Density"), &settings.StarDensity, 0.25f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("star_density_tooltip"), "How many of the faint stars are shown. 1 is a dark, clear rural sky."));
	ImGui::SliderFloat(T(TKEY("twinkle"), "Twinkle"), &settings.Twinkle, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("twinkle_tooltip"), "Atmospheric scintillation. Strongest near the horizon, where starlight crosses the most air."));
	ImGui::SliderFloat(T(TKEY("milky_way"), "Milky Way"), &settings.MilkyWay, 0.0f, 3.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("airglow"), "Airglow"), &settings.Airglow, 0.0f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("airglow_tooltip"), "The faint green glow of the upper atmosphere, a band just above the horizon on dark nights."));
	ImGui::SliderFloat(T(TKEY("latitude"), "Latitude"), &settings.Latitude, 0.0f, 90.0f, "%.0f deg");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("latitude_tooltip"), "Height of the celestial pole above the northern horizon; the whole sky turns around it."));

	ImGui::SeparatorText(T(TKEY("aurora"), "Aurora"));
	ImGui::Checkbox(T(TKEY("aurora_enabled"), "Aurora"), &settings.Aurora);
	ImGui::BeginDisabled(!settings.Aurora);
	ImGui::Checkbox(T(TKEY("replace_vanilla_aurora"), "Replace Vanilla Aurora"), &settings.ReplaceVanillaAurora);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("replace_vanilla_aurora_tooltip"), "Hides the game's aurora mesh while the ray-marched aurora is showing."));
	ImGui::SliderFloat(T(TKEY("aurora_intensity"), "Aurora Intensity"), &settings.AuroraIntensity, 0.0f, 4.0f, "%.2f");
	ImGui::SliderFloat(T(TKEY("aurora_clear_night_chance"), "Chance on Clear Nights"), &settings.AuroraClearNightChance, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("aurora_clear_night_chance_tooltip"), "Aurora weathers always show one. This is the chance that any other clear night does too, rolled once per night."));
	ImGui::ColorEdit3(T(TKEY("aurora_color_low"), "Lower Curtain Colour"), &settings.AuroraColorLow.x, ImGuiColorEditFlags_Float);
	ImGui::ColorEdit3(T(TKEY("aurora_color_high"), "Upper Curtain Colour"), &settings.AuroraColorHigh.x, ImGuiColorEditFlags_Float);
	ImGui::EndDisabled();

	const auto frame = GetCommonBufferData();
	ImGui::Spacing();
	ImGui::TextDisabled(T(TKEY("status"), "Star visibility %.2f, night %.2f, aurora %.2f"), frame.StarVisibility, frame.Night, frame.AuroraStrength);
}

#undef I18N_KEY_PREFIX

void NightSky::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.StarBrightness = std::clamp(settings.StarBrightness, 0.0f, 4.0f);
	settings.StarDensity = std::clamp(settings.StarDensity, 0.25f, 2.0f);
	settings.Twinkle = std::clamp(settings.Twinkle, 0.0f, 1.0f);
	settings.MilkyWay = std::clamp(settings.MilkyWay, 0.0f, 3.0f);
	settings.Airglow = std::clamp(settings.Airglow, 0.0f, 3.0f);
	settings.Latitude = std::clamp(settings.Latitude, 0.0f, 90.0f);
	settings.AuroraIntensity = std::clamp(settings.AuroraIntensity, 0.0f, 4.0f);
	settings.AuroraClearNightChance = std::clamp(settings.AuroraClearNightChance, 0.0f, 1.0f);
}

void NightSky::SaveSettings(json& o_json)
{
	o_json = settings;
}

void NightSky::RestoreDefaultSettings()
{
	settings = {};
}

float NightSky::GetAuroraActivity(float a_starVisibility) const
{
	auto* sky = RE::Sky::GetSingleton();
	if (!sky)
		return 0.0f;

	// Weather-driven, following the transition between the outgoing and incoming weather.
	const float incoming = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
	float activity = (HasAuroraFlag(sky->currentWeather) ? incoming : 0.0f) + (HasAuroraFlag(sky->lastWeather) ? 1.0f - incoming : 0.0f);

	// Any other clear night may carry a quieter display, decided once per night.
	if (settings.AuroraClearNightChance > 0.0f) {
		const auto* calendar = RE::Calendar::GetSingleton();
		const float days = calendar ? calendar->GetDaysPassed() : 0.0f;
		// Nights run from dusk to dawn, so the roll changes at noon, never during the display.
		const auto night = static_cast<std::uint32_t>(std::floor(days + 0.5f));
		if (HashToUnit(night * 2654435761u + 0x9E37u) < settings.AuroraClearNightChance)
			activity = std::max(activity, 0.55f * a_starVisibility);
	}
	return std::clamp(activity, 0.0f, 1.0f);
}

NightSky::PerFrame NightSky::GetCommonBufferData() const
{
	PerFrame data{};
	data.Enabled = loaded && settings.Enabled && !Util::IsInterior();

	float starVisibility = 0.0f;
	float night = 0.0f;
	float siderealAngle = 0.0f;
	if (auto* sky = RE::Sky::GetSingleton()) {
		if (sky->stars)
			starVisibility = std::clamp(sky->stars->alpha, 0.0f, 1.0f);

		const float hour = sky->currentGameHour;
		if (const auto* climate = sky->currentClimate) {
			// Climate timings are stored in ten-minute steps.
			const auto& timing = climate->timing;
			const float dusk = SmoothRange(timing.sunset.begin / 6.0f, timing.sunset.end / 6.0f + 0.5f, hour);
			const float dawn = 1.0f - SmoothRange(timing.sunrise.begin / 6.0f - 0.5f, timing.sunrise.end / 6.0f, hour);
			night = std::max(dusk, dawn);
		}
	}
	if (const auto* calendar = RE::Calendar::GetSingleton()) {
		// One sidereal day is 0.99727 solar days, so the stars gain a little on the sun every night.
		const double turns = static_cast<double>(calendar->GetDaysPassed()) * 1.0027379;
		siderealAngle = static_cast<float>((turns - std::floor(turns)) * 2.0 * 3.14159265358979);
	}

	data.ReplaceStars = settings.ReplaceVanillaStars ? 1u : 0u;
	data.StarVisibility = starVisibility;
	data.StarBrightness = settings.StarBrightness;
	data.StarDensity = settings.StarDensity;
	data.Twinkle = settings.Twinkle;
	data.MilkyWay = settings.MilkyWay;
	data.Airglow = settings.Airglow;
	data.AuroraStrength = settings.Aurora ? GetAuroraActivity(starVisibility) * night : 0.0f;
	data.AuroraIntensity = settings.AuroraIntensity;
	data.SiderealAngle = siderealAngle;
	data.Latitude = settings.Latitude * 0.0174532925f;
	data.AuroraColorLow = settings.AuroraColorLow;
	data.Night = night;
	data.AuroraColorHigh = settings.AuroraColorHigh;
	return data;
}

void NightSky::UpdateVanillaAurora(bool a_hide)
{
	auto* sky = RE::Sky::GetSingleton();
	auto* root = sky ? sky->auroraRoot.get() : nullptr;
	if (!root) {
		vanillaAuroraHidden = false;
		return;
	}
	auto& flags = root->GetFlags();
	if (a_hide && !flags.any(RE::NiAVObject::Flag::kHidden)) {
		flags.set(RE::NiAVObject::Flag::kHidden);
		vanillaAuroraHidden = true;
	} else if (!a_hide && vanillaAuroraHidden) {
		flags.reset(RE::NiAVObject::Flag::kHidden);
		vanillaAuroraHidden = false;
	}
}

void NightSky::Prepass()
{
	const bool ours = settings.Enabled && settings.Aurora && settings.ReplaceVanillaAurora && !Util::IsInterior();
	UpdateVanillaAurora(ours);
}
