#pragma once

#include <d3d11.h>
#include <imgui.h>

/**
 * @brief The Astral Shaders logo, loaded once from disk and drawn as an image.
 *
 * The picture is used exactly as shipped: no generated glow, gradient or outline. When the
 * file is missing nothing is drawn and callers lay out as if the logo had zero width.
 */
namespace BrandLogo
{
	/** @brief Data-relative path of the logo image. */
	inline constexpr const char* kPath = "Data/SKSE/Plugins/CommunityShaders/AstralLogo.png";

	/** @brief True when the image loaded (or has not been tried yet and loads now). */
	bool IsAvailable();

	/** @brief Width over height of the image; 0 when unavailable. */
	float Aspect();

	/**
	 * @brief Draws the logo with its top-left corner at a_topLeft, scaled to a_height.
	 * @param a_tint Multiplied into the image; use the alpha for a watermark.
	 * @return The width drawn, 0 when the logo is unavailable.
	 */
	float Draw(ImDrawList* a_drawList, const ImVec2& a_topLeft, float a_height, ImU32 a_tint = IM_COL32_WHITE);

	/** @brief Releases the texture; safe to call when nothing was loaded. */
	void Release();
}
