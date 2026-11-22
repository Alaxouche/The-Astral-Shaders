#include "ENBAdaptation.h"

#include "../EffectManager.h"
#include "../SettingManager.h"
#include "../TextureManager.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

void ENBAdaptation::Execute()
{
	outputTextureName = nullptr;

	auto& textureManager = TextureManager::GetSingleton();

	auto* currentSRV = textureManager.GetDownsampleTextureBlurry();
	if (!currentSRV) {
		return;
	}

	SetShaderResourceVariable("TextureCurrent", currentSRV);

	if (!textureCurrent.texture)
		return;

	ExecuteTechnique("Downsample", textureCurrent);

	SetShaderResourceVariable("TextureCurrent", textureCurrent.srv.get());

	const auto [texturePreviousName, textureAdaptationName] = GetTextureNames();

	// Set input texture (previous frame's adaptation value)
	auto* texturePrevious = textureManager.GetCommonTexture(texturePreviousName);
	if (!texturePrevious) {
		return;
	}
	SetShaderResourceVariable("TexturePrevious", texturePrevious->srv.get());

	// Execute adaptation technique, writing to output texture
	auto* textureAdaptation = textureManager.GetCommonTexture(textureAdaptationName);
	if (!textureAdaptation) {
		return;
	}
	if (interiorActive && DrawInteriorAdaptation(textureCurrent.srv.get(), texturePrevious->srv.get(), *textureAdaptation)) {
		// The engine gets the interior pass's result, as it would the preset's.
		outputTextureName = textureAdaptationName;
		return;
	}
	if (ExecuteTechnique("Draw", *textureAdaptation))
		outputTextureName = textureAdaptationName;
}

std::pair<const char*, const char*> ENBAdaptation::GetTextureNames()
{
	const bool swap = (TextureManager::GetSingleton().GetTextureSwap() & 1) != 0;
	return swap ? std::pair{ "TextureAdaptationSwap", "TextureAdaptation" } : std::pair{ "TextureAdaptation", "TextureAdaptationSwap" };
}

ID3D11ShaderResourceView* ENBAdaptation::GetOutputSRV() const
{
	if (!outputTextureName)
		return nullptr;
	auto* texture = TextureManager::GetSingleton().FindCommonTexture(outputTextureName);
	return texture ? texture->srv.get() : nullptr;
}

bool ENBAdaptation::DrawInteriorAdaptation(ID3D11ShaderResourceView* a_current, ID3D11ShaderResourceView* a_previous, TextureManager::Texture& a_target)
{
	if (!a_current || !a_previous || !a_target.rtv)
		return false;

	if (!interiorPS && !interiorPSFailed) {
		interiorPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\Effects11\\AdaptationInteriorPS.hlsl", {}, "ps_5_0"));
		if (!interiorPS) {
			logger::error("[EFFECTS11] Failed to compile AdaptationInteriorPS.hlsl; interiors use the preset's adaptation");
			interiorPSFailed = true;
		}
	}
	if (!interiorPS)
		return false;

	auto context = globals::d3d::context;
	if (!interiorCB) {
		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(InteriorCB);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(globals::d3d::device->CreateBuffer(&cbDesc, nullptr, interiorCB.put())))
			return false;
		Util::SetResourceName(interiorCB.get(), "ENBAdaptation::InteriorCB");
	}

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(interiorCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	*static_cast<InteriorCB*>(mapped.pData) = interiorValues;
	context->Unmap(interiorCB.get(), 0);

	auto& effectManager = EffectManager::GetSingleton();

	D3D11_TEXTURE2D_DESC desc{};
	a_target.texture->GetDesc(&desc);
	D3D11_VIEWPORT viewport{};
	viewport.Width = static_cast<float>(desc.Width);
	viewport.Height = static_cast<float>(desc.Height);
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	ID3D11RenderTargetView* rtv = a_target.rtv.get();
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->OMSetDepthStencilState(nullptr, 0);
	context->RSSetState(effectManager.rasterizerState.get());
	context->OMSetBlendState(effectManager.blendState.get(), nullptr, 0xFFFFFFFF);

	UINT stride = sizeof(float) * 5;
	UINT offset = 0;
	ID3D11Buffer* vertexBuffers[] = { effectManager.quadVertexBuffer.get() };
	context->IASetVertexBuffers(0, 1, vertexBuffers, &stride, &offset);
	context->IASetInputLayout(effectManager.inputLayout.get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	context->VSSetShader(effectManager.copyVertexShader.get(), nullptr, 0);
	context->PSSetShader(interiorPS, nullptr, 0);
	ID3D11ShaderResourceView* srvs[] = { a_current, a_previous };
	context->PSSetShaderResources(0, 2, srvs);
	ID3D11Buffer* constantBuffers[] = { interiorCB.get() };
	context->PSSetConstantBuffers(0, 1, constantBuffers);

	globals::profiler->BeginPass("Effects11::AdaptationInterior");
	context->Draw(4, 0);
	globals::profiler->EndPass();

	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
	ID3D11ShaderResourceView* nullSRVs[2] = {};
	context->PSSetShaderResources(0, 2, nullSRVs);
	return true;
}

void ENBAdaptation::ClearShaderCache()
{
	if (interiorPS) {
		interiorPS->Release();
		interiorPS = nullptr;
	}
	interiorPSFailed = false;
}

void ENBAdaptation::UpdateEffectVariables()
{
	auto& settingManager = SettingManager::GetSingleton();

	if (!idsCached) {
		exterior = { settingManager.GetSettingID("ForceMinMaxValues", "ADAPTATION"), settingManager.GetSettingID("AdaptationTime", "ADAPTATION"),
			settingManager.GetSettingID("AdaptationMin", "ADAPTATION"), settingManager.GetSettingID("AdaptationMax", "ADAPTATION"),
			settingManager.GetSettingID("AdaptationSensitivity", "ADAPTATION") };
		interior = { settingManager.GetSettingID("ForceMinMaxValuesInterior", "ADAPTATION"), settingManager.GetSettingID("AdaptationTimeInterior", "ADAPTATION"),
			settingManager.GetSettingID("AdaptationMinInterior", "ADAPTATION"), settingManager.GetSettingID("AdaptationMaxInterior", "ADAPTATION"),
			settingManager.GetSettingID("AdaptationSensitivityInterior", "ADAPTATION") };
		idUseInterior = settingManager.GetSettingID("UseInteriorValues", "ADAPTATION");
		idsCached = true;
	}

	interiorActive = settingManager.GetValue<bool>(idUseInterior) && EffectManager::GetSingleton().commonData.eInteriorFactor > 0.5f;
	const auto& ids = interiorActive ? interior : exterior;

	auto forceMinMaxValues = settingManager.GetValue<bool>(ids.forceMinMax);

	float adaptationTime = settingManager.GetValue<float>(ids.time);
	float deltaTime = (globals::game::deltaTime) ? (*globals::game::deltaTime) : 0.0f;

	float4 adaptationParameters{};
	adaptationParameters.x = !forceMinMaxValues ? 0.0f : settingManager.GetValue<float>(ids.min);
	adaptationParameters.y = !forceMinMaxValues ? 65535.0f : settingManager.GetValue<float>(ids.max);
	adaptationParameters.z = settingManager.GetValue<float>(ids.sensitivity);
	adaptationParameters.w = std::clamp((adaptationTime > 0.0f) ? (deltaTime / adaptationTime) : 1.0f, 0.0f, 1.0f);

	// The interior pass clamps only when asked, unlike AdaptationParameters whose 0..65535 stands for "no clamp"
	interiorValues.Parameters = adaptationParameters;
	interiorValues.Options = { forceMinMaxValues ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };

	SetVectorVariable("AdaptationParameters", &adaptationParameters, sizeof(adaptationParameters));
}

void ENBAdaptation::CreateEffectTextures()
{
	textureCurrent = CreateTexture(16, 16, DXGI_FORMAT_R32_FLOAT, "ENBAdaptation::TextureCurrent");
}
