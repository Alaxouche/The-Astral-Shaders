#include "MenuHeaderRenderer.h"

#include <imgui.h>
#include <imgui_internal.h>

#include "Fonts.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "ForkVersion.h"
#include "Plugin.h"
#include "ShaderCache.h"
#include "State.h"
#include "ThemeManager.h"
#include "Util.h"
#include "Utils/UI.h"
#include "Utils/VectorIcons.h"

namespace
{
	// Logo height relative to the title line, and the gap between logo and title.
	constexpr float kLogoHeightRatio = 1.9f;
	constexpr float kLogoTextGapRatio = 0.45f;

	/** @brief Draws the bottle logo followed by the product title on one line. */
	void RenderTitleRow()
	{
		auto title = std::format("{} {}", Plugin::DISPLAY_NAME, Fork::GetVersionString());

		MenuFonts::FontRoleGuard titleFont(Menu::FontRole::Title);

		const float fontSize = ImGui::GetFontSize();
		const float logoHeight = fontSize * kLogoHeightRatio;
		const float logoWidth = logoHeight * Util::Icons::kBottleAspect;
		const float gap = fontSize * kLogoTextGapRatio;
		const float rowHeight = ImMax(logoHeight, ImGui::GetTextLineHeight());

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		// The Astral logo, loaded once; the alpha channel carries the glow so it sits on any
		// theme. Falls back to the bottle line art when the asset is missing.
		static ID3D11ShaderResourceView* astralLogo = nullptr;
		static ImVec2                    astralLogoSize{};
		static bool                      astralLogoTried = false;
		if (!astralLogoTried) {
			astralLogoTried = true;
			Util::LoadTextureFromFile(globals::d3d::device, "Data/SKSE/Plugins/CommunityShaders/AstralLogo.png", &astralLogo, astralLogoSize);
		}
		if (astralLogo) {
			const float side = logoHeight;
			ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(astralLogo),
				ImVec2(origin.x, origin.y + (rowHeight - side) * 0.5f),
				ImVec2(origin.x + side, origin.y + (rowHeight - side) * 0.5f + side));
		} else
			Util::Icons::DrawBottle(ImGui::GetWindowDrawList(),
			ImVec2(origin.x, origin.y + (rowHeight - logoHeight) * 0.5f),
			logoHeight,
			ImGui::GetColorU32(ImGuiCol_Text));

		const float drawnLogoWidth = astralLogo ? logoHeight : logoWidth;
		ImGui::SetCursorScreenPos(ImVec2(origin.x + drawnLogoWidth + gap, origin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f));
		ImGui::TextUnformatted(title.c_str());
		// Build detail: the git describe of this exact binary, dimmed beside the version.
		ImGui::SameLine();
		ImGui::TextDisabled("%s", Plugin::BUILD_DESCRIBE.data());

		ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rowHeight));
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
	}
}

void MenuHeaderRenderer::RenderHeader(bool isDocked)
{
	if (!globals::menu) {
		logger::error("MenuHeaderRenderer::RenderHeader: globals::menu is null, cannot render header");
		return;
	}

	// When docked the tab already names the window, so only the action row is drawn.
	if (!isDocked) {
		RenderTitleRow();
		ImGui::Spacing();
	}

	auto shaderCache = globals::shaderCache;
	const bool showErrorToggle = shaderCache->GetFailedTasks() > 0;
	const int columns = showErrorToggle ? 4 : 3;

	if (ImGui::BeginTable("##ActionButtons", columns, ImGuiTableFlags_SizingStretchSame)) {
		ImGui::TableNextColumn();
		if (Util::Icons::ButtonWithFlash(T("menu.save_settings", "Save Settings"), Util::Icons::Kind::Save, { -1, 0 })) {
			globals::state->Save();
		}

		ImGui::TableNextColumn();
		if (Util::Icons::Button(T("menu.restore_settings", "Restore Saved Settings"), Util::Icons::Kind::Reload, { -1, 0 })) {
			globals::state->Load();
		}

		ImGui::TableNextColumn();
		if (Util::Icons::Button(T("menu.clear_shader_cache", "Clear Shader Cache"), Util::Icons::Kind::Refresh, { -1, 0 })) {
			Util::RequestClearShaderCacheConfirmation();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T("menu.clear_shader_cache_tooltip",
								  "Clears the shader cache and disk cache (if enabled). "
								  "The Shader Cache is the collection of compiled shaders which replace the vanilla shaders at runtime. "
								  "The Disk Cache is a collection of compiled shaders on disk. "
								  "Clearing will mean that shaders are recompiled only when the game re-encounters them."));
		}

		if (showErrorToggle) {
			ImGui::TableNextColumn();
			if (Util::ErrorButton(T("menu.toggle_error_message", "Toggle Error Message"), { -1, 0 })) {
				shaderCache->ToggleErrorMessages();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T("menu.toggle_error_message_tooltip",
									  "Hide or show the shader failure message. "
									  "Your installation is broken and will likely see errors in game. "
									  "Please double check you have updated all features and that your load order is correct. "
									  "See the log for details."));
			}
		}

		ImGui::EndTable();
	}

	ImGui::Spacing();
	ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal, ThemeManager::Constants::SEPARATOR_THICKNESS);
	ImGui::Spacing();
}
