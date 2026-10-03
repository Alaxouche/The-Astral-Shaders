#include "BrandLogo.h"

#include <winrt/base.h>

#include "Globals.h"
#include "Utils/UI.h"

namespace
{
	winrt::com_ptr<ID3D11ShaderResourceView> texture;
	ImVec2 size{};
	bool tried = false;

	void EnsureLoaded()
	{
		if (tried)
			return;
		tried = true;
		if (!globals::d3d::device)
			return;
		ID3D11ShaderResourceView* srv = nullptr;
		if (Util::LoadTextureFromFile(globals::d3d::device, BrandLogo::kPath, &srv, size) && srv)
			texture.attach(srv);
		else
			logger::warn("[BrandLogo] {} not loaded; the menu header shows the title only", BrandLogo::kPath);
	}
}

bool BrandLogo::IsAvailable()
{
	EnsureLoaded();
	return texture && size.x > 0.0f && size.y > 0.0f;
}

float BrandLogo::Aspect()
{
	return IsAvailable() ? size.x / size.y : 0.0f;
}

float BrandLogo::Draw(ImDrawList* a_drawList, const ImVec2& a_topLeft, float a_height, ImU32 a_tint)
{
	if (!IsAvailable() || !a_drawList || a_height <= 0.0f)
		return 0.0f;
	const float width = a_height * Aspect();
	a_drawList->AddImage(reinterpret_cast<ImTextureID>(texture.get()), a_topLeft, ImVec2(a_topLeft.x + width, a_topLeft.y + a_height), ImVec2(0, 0), ImVec2(1, 1), a_tint);
	return width;
}

void BrandLogo::Release()
{
	texture = nullptr;
	size = {};
	tried = false;
}
