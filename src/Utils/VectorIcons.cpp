#include "VectorIcons.h"

#include <array>
#include <imgui_internal.h>

#include "Utils/UI.h"

namespace
{
	// The action icons are transcribed from 24x24 artwork with a 2px stroke.
	constexpr float kSourceGrid = 24.0f;
	constexpr float kSourceStroke = 2.0f;

	constexpr float kIconStrokeRatio = kSourceStroke / kSourceGrid;

	float ResolveThickness(float thickness, float reference, float ratio)
	{
		return thickness > 0.0f ? thickness : ImMax(1.0f, reference * ratio);
	}

	/** @brief Maps a point from the 24x24 source grid into the destination square. */
	struct GridMapper
	{
		ImVec2 origin;
		float scale;

		ImVec2 operator()(float x, float y) const
		{
			return ImVec2(origin.x + x * scale, origin.y + y * scale);
		}
	};

	void StrokePolyline(ImDrawList* drawList, const ImVec2* points, int count, ImU32 color, float thickness, bool closed)
	{
		drawList->AddPolyline(points, count, color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, thickness);
	}

	void DrawSaveIcon(ImDrawList* drawList, const GridMapper& g, ImU32 color, float thickness)
	{
		// Outer body: notched top-left corner, clipped top-right corner.
		const std::array<ImVec2, 7> body = { { g(8, 4), g(4, 4), g(4, 20), g(20, 20), g(20, 8), g(16, 4), g(14, 4) } };
		StrokePolyline(drawList, body.data(), static_cast<int>(body.size()), color, thickness, false);

		// Shutter.
		const std::array<ImVec2, 4> shutter = { { g(8, 4), g(8, 8), g(14, 8), g(14, 4) } };
		StrokePolyline(drawList, shutter.data(), static_cast<int>(shutter.size()), color, thickness, true);

		// Label knob.
		drawList->AddCircle(g(12, 14), 2.0f * g.scale, color, 0, thickness);
	}

	void DrawReloadIcon(ImDrawList* drawList, const GridMapper& g, ImU32 color, float thickness)
	{
		// Three-quarter ring opening at the top, then a tail running off to the right.
		drawList->PathArcTo(g(11.5f, 13.5f), 7.5f * g.scale, 0.0f, IM_PI * 1.5f, 32);
		drawList->PathLineTo(g(20, 6));
		drawList->PathStroke(color, ImDrawFlags_None, thickness);

		// Arrow head on the tail.
		const std::array<ImVec2, 3> head = { { g(17, 3), g(20, 6), g(17, 9) } };
		StrokePolyline(drawList, head.data(), static_cast<int>(head.size()), color, thickness, false);
	}

	void DrawRefreshIcon(ImDrawList* drawList, const GridMapper& g, ImU32 color, float thickness)
	{
		drawList->AddCircle(g(12, 12), 10.0f * g.scale, color, 0, thickness);

		// Upper arm: runs right, then turns down.
		drawList->PathLineTo(g(8, 8));
		drawList->PathLineTo(g(14, 8));
		drawList->PathArcTo(g(14, 10), 2.0f * g.scale, -IM_PI * 0.5f, 0.0f, 8);
		drawList->PathLineTo(g(16, 11));
		drawList->PathStroke(color, ImDrawFlags_None, thickness);

		const std::array<ImVec2, 3> upperHead = { { g(10, 6), g(8, 8), g(10, 10) } };
		StrokePolyline(drawList, upperHead.data(), static_cast<int>(upperHead.size()), color, thickness, false);

		// Lower arm: runs left, then turns up.
		drawList->PathLineTo(g(16, 16));
		drawList->PathLineTo(g(10, 16));
		drawList->PathArcTo(g(10, 14), 2.0f * g.scale, IM_PI * 0.5f, IM_PI, 8);
		drawList->PathLineTo(g(8, 13));
		drawList->PathStroke(color, ImDrawFlags_None, thickness);

		const std::array<ImVec2, 3> lowerHead = { { g(14, 18), g(16, 16), g(14, 14) } };
		StrokePolyline(drawList, lowerHead.data(), static_cast<int>(lowerHead.size()), color, thickness, false);
	}

	/** @brief Width of an icon + label group, or of the label alone when there is no icon. */
	float MeasureIconLabel(Util::Icons::Kind kind, const char* label, float iconSize, float gap)
	{
		const float textWidth = ImGui::CalcTextSize(label, nullptr, true).x;
		return kind == Util::Icons::Kind::None ? textWidth : iconSize + gap + textWidth;
	}
}

namespace Util::Icons
{
	void Draw(ImDrawList* drawList, Kind kind, const ImVec2& topLeft, float size, ImU32 color, float thickness)
	{
		if (!drawList || size <= 0.0f || kind == Kind::None)
			return;

		const GridMapper g{ topLeft, size / kSourceGrid };
		const float stroke = ResolveThickness(thickness, size, kIconStrokeRatio);

		switch (kind) {
		case Kind::Save:
			DrawSaveIcon(drawList, g, color, stroke);
			break;
		case Kind::Reload:
			DrawReloadIcon(drawList, g, color, stroke);
			break;
		case Kind::Refresh:
			DrawRefreshIcon(drawList, g, color, stroke);
			break;
		default:
			break;
		}
	}

	bool Button(const char* label, Kind kind, const ImVec2& size)
	{
		const auto& style = ImGui::GetStyle();
		const float iconSize = ImGui::GetFontSize();
		const float gap = style.ItemInnerSpacing.x;
		const float contentWidth = MeasureIconLabel(kind, label, iconSize, gap);

		ImVec2 buttonSize = size;
		if (buttonSize.x == 0.0f)
			buttonSize.x = contentWidth + style.FramePadding.x * 2.0f;
		if (buttonSize.y == 0.0f)
			buttonSize.y = ImGui::GetFrameHeight();

		// An empty label stops ImGui drawing the text itself; the ID still comes from `label`.
		ImGui::PushID(label);
		const bool pressed = ImGui::Button("##IconButton", buttonSize);
		ImGui::PopID();

		const ImVec2 rectMin = ImGui::GetItemRectMin();
		const ImVec2 rectSize = ImGui::GetItemRectSize();
		const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		float cursorX = rectMin.x + ImMax(style.FramePadding.x, (rectSize.x - contentWidth) * style.ButtonTextAlign.x);
		const float centerY = rectMin.y + rectSize.y * 0.5f;

		if (kind != Kind::None) {
			Draw(drawList, kind, ImVec2(cursorX, centerY - iconSize * 0.5f), iconSize, textColor);
			cursorX += iconSize + gap;
		}

		drawList->AddText(ImVec2(cursorX, centerY - ImGui::GetTextLineHeight() * 0.5f), textColor, label,
			ImGui::FindRenderedTextEnd(label));

		return pressed;
	}

	bool ButtonWithFlash(const char* label, Kind kind, const ImVec2& size, int flashDurationMs)
	{
		Util::ButtonFlashGuard flashGuard(label, flashDurationMs);
		const bool pressed = Button(label, kind, size);
		if (pressed)
			Util::NotifyButtonFlash(label);
		return pressed;
	}
}
