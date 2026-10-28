#include "BrandLogo.h"

#include <DirectXTex.h>
#include <winrt/base.h>

#include "Globals.h"
#include "Utils/D3D.h"
#include "Utils/Format.h"

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

		DirectX::ScratchImage image;
		if (FAILED(DirectX::LoadFromWICFile(BrandLogo::kPath, DirectX::WIC_FLAGS_NONE, nullptr, image))) {
			logger::warn("[BrandLogo] {} not loaded; the menu header shows the title only", Util::WStringToString(BrandLogo::kPath));
			return;
		}

		const auto& metadata = image.GetMetadata();
		if (FAILED(DirectX::CreateShaderResourceView(globals::d3d::device, image.GetImages(), image.GetImageCount(), metadata, texture.put())) || !texture) {
			logger::warn("[BrandLogo] could not create the icon texture");
			texture = nullptr;
			return;
		}
		Util::SetResourceName(texture.get(), "BrandLogo::Icon SRV");
		size = ImVec2(static_cast<float>(metadata.width), static_cast<float>(metadata.height));
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

float BrandLogo::Draw(ImDrawList* a_drawList, const ImVec2& a_topLeft, float a_height)
{
	if (!IsAvailable() || !a_drawList || a_height <= 0.0f)
		return 0.0f;
	const float width = a_height * Aspect();
	a_drawList->AddImage(reinterpret_cast<ImTextureID>(texture.get()), a_topLeft, ImVec2(a_topLeft.x + width, a_topLeft.y + a_height));
	return width;
}
