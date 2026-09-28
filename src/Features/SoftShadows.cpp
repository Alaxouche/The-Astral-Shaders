#include "SoftShadows.h"

#include "I18n/I18n.h"
#include "Utils/UI.h"

#include <imgui.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SoftShadows::Settings,
	Enabled,
	SunAngularDiameter,
	MaxPenumbraTexels)

void SoftShadows::DrawSettings()
{
	ImGui::Checkbox(T("feature.soft_shadows.enable", "Enable Soft Shadows"), &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.soft_shadows.enable_tooltip",
			"PCSS on the sun shadows: the penumbra widens with the distance between a surface and whatever casts the shadow onto it. Requires the shadow filter quality modes (the default)."));
	}

	ImGui::BeginDisabled(!settings.Enabled);

	ImGui::SliderFloat(T("feature.soft_shadows.sun_size", "Sun Angular Size"), &settings.SunAngularDiameter, 0.1f, 5.0f, "%.2f deg");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.soft_shadows.sun_size_tooltip",
			"Apparent diameter of the sun. The real sun is about 0.53 degrees; higher values read as hazier light and softer shadows overall."));
	}

	ImGui::SliderFloat(T("feature.soft_shadows.max_penumbra", "Max Penumbra (texels)"), &settings.MaxPenumbraTexels, 4.0f, 48.0f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T("feature.soft_shadows.max_penumbra_tooltip",
			"Ceiling on the blur radius in shadowmap texels. Also bounds the blocker search, so it is the cost ceiling of the effect."));
	}

	ImGui::EndDisabled();
}

void SoftShadows::LoadSettings(json& o_json)
{
	settings = o_json;
}

void SoftShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SoftShadows::RestoreDefaultSettings()
{
	settings = {};
}

SoftShadows::GPUSettings SoftShadows::GetCommonBufferData() const
{
	return {
		.Enabled = loaded && settings.Enabled ? 1u : 0u,
		// Penumbra math wants the tangent of the angular RADIUS.
		.TanSunRadius = std::tan(0.5f * settings.SunAngularDiameter * 0.01745329252f),
		.MaxPenumbraTexels = settings.MaxPenumbraTexels,
		.pad = 0.0f
	};
}
