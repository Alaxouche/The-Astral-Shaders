#pragma once

#include <imgui.h>

/**
 * @brief The Astral Shaders menu icon, loaded once from disk and drawn as an image.
 *
 * The picture is used exactly as shipped, alpha included: nothing is generated around it. When the
 * file is missing nothing is drawn and callers lay out as if the icon had zero width.
 */
namespace BrandLogo
{
	/** @brief Data-relative path of the icon image. */
	inline constexpr const wchar_t* kPath = L"Data/SKSE/Plugins/CommunityShaders/AstralLogo.png";

	/** @brief True when the image is loaded; the first call loads it. */
	bool IsAvailable();

	/** @brief Width over height of the image; 0 when unavailable. */
	float Aspect();

	/**
	 * @brief Draws the icon with its top-left corner at a_topLeft, scaled to a_height.
	 * @return The width drawn, 0 when the icon is unavailable.
	 */
	float Draw(ImDrawList* a_drawList, const ImVec2& a_topLeft, float a_height);
}
