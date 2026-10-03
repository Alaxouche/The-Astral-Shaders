#include "TerrainBlending.h"

#include "Deferred.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#include <winrt/base.h>

#define I18N_KEY_PREFIX "feature.terrain_blending."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainBlending::Settings,
	Enabled)

void TerrainBlending::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable"), "Enable Terrain Blending"), (bool*)&settings.Enabled);

	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tooltip"), "Enable seamless blending between terrain and objects."));
	}

	if (deformableTerrainLoaded) {
		ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Warning, "%s",
			T(TKEY("deformable_terrain_gate"), "Dynamic Terrain Deformation is loaded. Blending is held off while it reshapes the landscape, because the two passes would see different geometry. Everything else in the feature keeps running, so nothing else has to be recompiled."));
	}
}

void TerrainBlending::LoadSettings(json& o_json)
{
	settings = o_json;
}

void TerrainBlending::SaveSettings(json& o_json)
{
	o_json = settings;
}

ID3D11VertexShader* TerrainBlending::GetTerrainVertexShader()
{
	if (!terrainVertexShader) {
		logger::debug("Compiling Utility.hlsl");
		terrainVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", { { "RENDER_DEPTH", "" } }, "vs_5_0");
	}
	return terrainVertexShader;
}

ID3D11VertexShader* TerrainBlending::GetTerrainOffsetVertexShader()
{
	if (!terrainOffsetVertexShader) {
		logger::debug("Compiling Utility.hlsl");
		terrainOffsetVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", { { "RENDER_DEPTH", "" }, { "OFFSET_DEPTH", "" } }, "vs_5_0");
	}
	return terrainOffsetVertexShader;
}

ID3D11ComputeShader* TerrainBlending::GetDepthBlendShader()
{
	if (!depthBlendShader) {
		logger::debug("Compiling DepthBlend.hlsl");
		depthBlendShader = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\TerrainBlending\\DepthBlend.hlsl", {}, "cs_5_0");
	}
	return depthBlendShader;
}

void TerrainBlending::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc;
		mainDepth.texture->GetDesc(&texDesc);
		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, NULL, &terrainDepth.texture));
		Util::SetResourceName(terrainDepth.texture, "TerrainBlending::TerrainDepth");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		mainDepth.depthSRV->GetDesc(&srvDesc);
		DX::ThrowIfFailed(device->CreateShaderResourceView(terrainDepth.texture, &srvDesc, &terrainDepth.depthSRV));
		Util::SetResourceName(terrainDepth.depthSRV, "TerrainBlending::TerrainDepth SRV");

		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		mainDepth.views[0]->GetDesc(&dsvDesc);
		DX::ThrowIfFailed(device->CreateDepthStencilView(terrainDepth.texture, &dsvDesc, &terrainDepth.views[0]));
		Util::SetResourceName(terrainDepth.views[0], "TerrainBlending::TerrainDepth DSV");
	}

	{
		auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		blendedDepthTexture = new Texture2D(texDesc, "TerrainBlending::BlendedDepth");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		main.SRV->GetDesc(&srvDesc);
		srvDesc.Format = texDesc.Format;
		blendedDepthTexture->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		main.UAV->GetDesc(&uavDesc);
		uavDesc.Format = texDesc.Format;
		blendedDepthTexture->CreateUAV(uavDesc);

		texDesc.Format = DXGI_FORMAT_R16_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		blendedDepthTexture16 = new Texture2D(texDesc, "TerrainBlending::BlendedDepth16");
		blendedDepthTexture16->CreateSRV(srvDesc);
		blendedDepthTexture16->CreateUAV(uavDesc);

		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		depthSRVBackup = mainDepth.depthSRV;

		auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		prepassSRVBackup = zPrepassCopy.depthSRV;
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
		depthStencilDesc.DepthEnable = true;
		depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		depthStencilDesc.StencilEnable = false;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthStencilDesc, &terrainDepthStencilState));
		Util::SetResourceName(terrainDepthStencilState, "TerrainBlending::DepthStencilState");
	}
}

void TerrainBlending::PostPostLoad()
{
	// Dynamic Terrain Deformation reshapes the landscape mesh every frame. The second terrain depth
	// pass cannot follow it, so the blend compares two different geometries and anything standing on
	// deformed ground sinks into it or floats above it. Checked here because every SKSE plugin has
	// loaded by now.
	deformableTerrainLoaded = GetModuleHandleW(kDeformableTerrainModule) != nullptr;
	if (deformableTerrainLoaded)
		logger::info("[Terrain Blending] Dynamic Terrain Deformation detected, blending gated off for this session");

	Hooks::Install();
}

void TerrainBlending::DataLoaded()
{
	// Land fade is the vanilla substitute for blending. It only gets turned off when blending is
	// actually going to run, so a gated session keeps the game's own transition.
	if (deformableTerrainLoaded)
		return;

	auto bEnableLandFade = RE::GetINISetting("bEnableLandFade:Display");
	bEnableLandFade->data.b = false;
}

TerrainBlending::Settings TerrainBlending::GetCommonBufferData() const
{
	Settings data = settings;
	data.Enabled = IsActive();
	return data;
}

void TerrainBlending::TerrainShaderHacks()
{
	if (renderTerrainDepth) {
		auto renderer = globals::game::renderer;
		auto context = globals::d3d::context;
		if (renderAltTerrain) {
			auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];
			context->OMSetRenderTargets(0, nullptr, dsv);
			context->VSSetShader(GetTerrainOffsetVertexShader(), NULL, NULL);
		} else {
			auto dsv = terrainDepth.views[0];
			context->OMSetRenderTargets(0, nullptr, dsv);
			auto shadowState = globals::game::shadowState;
			auto& currentVertexShader = shadowState->GetRuntimeData().currentVertexShader;
			context->VSSetShader((ID3D11VertexShader*)currentVertexShader->shader, NULL, NULL);
		}
		renderAltTerrain = !renderAltTerrain;
	}
}

void TerrainBlending::ResetDepth()
{
	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Blending - Reset Depth");
	auto context = globals::d3d::context;

	auto dsv = terrainDepth.views[0];
	context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0u);
}

void TerrainBlending::ResetTerrainDepth()
{
	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Blending - Reset Terrain Depth");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Terrain Blending - Reset Terrain Depth");

	auto context = globals::d3d::context;

	auto stateUpdateFlags = globals::game::stateUpdateFlags;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	auto currentVertexShader = *globals::game::currentVertexShader;
	context->VSSetShader((ID3D11VertexShader*)currentVertexShader->shader, NULL, NULL);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void TerrainBlending::RestoreSceneDepthSRVs()
{
	if (!depthSRVBackup || !prepassSRVBackup)
		return;

	auto renderer = globals::game::renderer;
	if (!renderer)
		return;

	auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
	depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV = depthSRVBackup;
	depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV = prepassSRVBackup;
}

void TerrainBlending::BlendPrepassDepths()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Blending - Blend Prepass Depths");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Terrain Blending - Blend Prepass Depths");

	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);

	auto dispatchCount = Util::GetScreenDispatchCount();

	{
		TracyD3D11Zone(globals::state->tracyCtx, "Terrain Blending - Depth Blend CS");

		// Gated: feed the main depth to both inputs. min/max of a value with itself is that
		// value in either depth convention, so blendedDepthTexture becomes an exact R32_FLOAT
		// copy of the scene depth without touching terrainDepth at all.
		ID3D11ShaderResourceView* views[2] = { depthSRVBackup, IsActive() ? terrainDepth.depthSRV : depthSRVBackup };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[2] = { blendedDepthTexture->uav.get(), blendedDepthTexture16->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(GetDepthBlendShader(), nullptr, 0);

		globals::profiler->BeginPass("TerrainBlending::DepthBlend");
		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
		globals::profiler->EndPass();
	}

	ID3D11ShaderResourceView* views[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);

	auto stateUpdateFlags = globals::game::stateUpdateFlags;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	auto renderer = globals::game::renderer;
	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	// The terrain depth seed is only needed when the terrain redirect actually runs.
	if (IsActive())
		context->CopyResource(terrainDepth.texture, mainDepth.texture);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void TerrainBlending::ClearShaderCache()
{
	if (terrainVertexShader) {
		terrainVertexShader->Release();
		terrainVertexShader = nullptr;
	}
	if (terrainOffsetVertexShader) {
		terrainOffsetVertexShader->Release();
		terrainOffsetVertexShader = nullptr;
	}
	if (depthBlendShader) {
		depthBlendShader->Release();
		depthBlendShader = nullptr;
	}
}

namespace
{
	// Shader resource view format for a depth texture, including the float layouts Reverse Z
	// converts the targets to.
	DXGI_FORMAT DepthViewFormat(DXGI_FORMAT a_texture)
	{
		switch (a_texture) {
		case DXGI_FORMAT_R24G8_TYPELESS:
			return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_R32G8X24_TYPELESS:
			return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		case DXGI_FORMAT_R32_TYPELESS:
			return DXGI_FORMAT_R32_FLOAT;
		default:
			return a_texture;
		}
	}

	ID3D11Resource* ResourceOf(ID3D11View* a_view)
	{
		if (!a_view)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		return resource.get();  // the texture outlives this call; only the identity is used
	}
}

void TerrainBlending::RefreshSceneDepthBackup(RE::BSGraphics::DepthStencilData& a_target, ID3D11ShaderResourceView*& a_backup, winrt::com_ptr<ID3D11ShaderResourceView>& a_owned, const char* a_label)
{
	if (!a_target.texture)
		return;

	ID3D11ShaderResourceView* blended = blendedDepthTexture ? blendedDepthTexture->srv.get() : nullptr;

	// The game (or Reverse Z, which rebuilds the depth targets with a float format after
	// SetupResources ran) can replace the texture behind this target. A view of the previous
	// texture, whether the backup or the one currently bound, reads data nothing writes any more.
	if (a_backup && ResourceOf(a_backup) == a_target.texture)
		return;

	if (a_target.depthSRV && a_target.depthSRV != blended && ResourceOf(a_target.depthSRV) == a_target.texture) {
		logger::info("[Terrain Blending] {} depth view changed ({:p} -> {:p}); backup refreshed from the bound view", a_label, static_cast<void*>(a_backup), static_cast<void*>(a_target.depthSRV));
		a_backup = a_target.depthSRV;
		return;
	}

	// Nothing bound on the live texture: make our own view of it.
	D3D11_TEXTURE2D_DESC texDesc{};
	a_target.texture->GetDesc(&texDesc);
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DepthViewFormat(texDesc.Format);
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	winrt::com_ptr<ID3D11ShaderResourceView> view;
	if (FAILED(globals::d3d::device->CreateShaderResourceView(a_target.texture, &srvDesc, view.put()))) {
		logger::error("[Terrain Blending] {} depth texture changed but no view could be created (texture format {})", a_label, static_cast<int>(texDesc.Format));
		return;
	}
	Util::SetResourceName(view.get(), "TerrainBlending::%s SceneDepth SRV", a_label);
	logger::info("[Terrain Blending] {} depth texture changed (format {}); backup replaced by a new view {:p}", a_label, static_cast<int>(texDesc.Format), static_cast<void*>(view.get()));
	a_owned = view;
	a_backup = a_owned.get();
	if (a_target.depthSRV != blended)
		a_target.depthSRV = a_backup;
}

void TerrainBlending::RefreshSceneDepthBackups()
{
	auto renderer = globals::game::renderer;
	auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
	RefreshSceneDepthBackup(depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN], depthSRVBackup, ownedSceneDepthSRV, "Scene");
	RefreshSceneDepthBackup(depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY], prepassSRVBackup, ownedPrepassDepthSRV, "Prepass");
}

void TerrainBlending::Hooks::Main_RenderDepth::thunk(bool a1, bool a2)
{
	ZoneScopedS(8);

	auto& singleton = globals::features::terrainBlending;
	singleton.RefreshSceneDepthBackups();
	auto shaderCache = globals::shaderCache;
	auto renderer = globals::game::renderer;

	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	globals::game::graphicsState->SetCameraData(RE::Main::WorldRootCamera(), 1);

	singleton.eyePosition = Util::GetEyePosition();

	// Deliberately the raw toggle, not IsActive(): this block is what binds blendedDepthTexture, and
	// its R32_FLOAT format is what every TERRAIN_BLENDING consumer was compiled to sample. A gated
	// session still runs it; with no terrain redirected into terrainDepth, which stays cleared to the
	// far plane, the blend returns the main depth untouched and the format contract holds.
	if (shaderCache->IsEnabled() && singleton.settings.Enabled) {
		mainDepth.depthSRV = singleton.blendedDepthTexture->srv.get();
		zPrepassCopy.depthSRV = singleton.blendedDepthTexture->srv.get();

		singleton.renderDepth = true;
		// Gated (Dynamic Terrain Deformation present): no terrain is redirected into
		// terrainDepth, so its clear is dead work -- the blend below reads the main depth twice.
		if (singleton.IsActive())
			singleton.ResetDepth();

		{
			ZoneScopedN("Terrain Depth - Game Render");
			func(a1, a2);
		}

		singleton.renderDepth = false;

		if (singleton.renderTerrainDepth) {
			singleton.renderTerrainDepth = false;
			singleton.ResetTerrainDepth();
		}

		singleton.BlendPrepassDepths();
	} else {
		mainDepth.depthSRV = singleton.depthSRVBackup;
		zPrepassCopy.depthSRV = singleton.prepassSRVBackup;

		{
			ZoneScopedN("Terrain Depth - Game Render");
			func(a1, a2);
		}
	}
}

void TerrainBlending::Hooks::BSBatchRenderer__RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	auto& singleton = globals::features::terrainBlending;
	auto shaderCache = globals::shaderCache;

	if (shaderCache->IsEnabled() && singleton.IsActive()) {
		if (singleton.renderDepth) {
			// Entering or exiting terrain depth section
			bool inTerrain = a_pass->shaderProperty && a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);

			if (inTerrain && a_pass->geometry) {
				if ((a_pass->geometry->worldBound.center.GetDistance(singleton.eyePosition) - a_pass->geometry->worldBound.radius) > 1024.0f) {
					inTerrain = false;
				}
			}

			if (singleton.renderTerrainDepth != inTerrain) {
				if (!inTerrain)
					singleton.ResetTerrainDepth();
				singleton.renderTerrainDepth = inTerrain;
			}

			if (inTerrain) {
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);  // Run terrain twice
			}
		} else if (globals::state->inWorld) {
			if (auto shaderProperty = a_pass->shaderProperty) {
				if (a_pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
					if (shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)) {
						RenderPass call{ a_pass, a_technique, a_alphaTest, a_renderFlags };
						singleton.terrainRenderPasses.push_back(call);
						return;
					}

					// Detect meshes which should not get terrain blending using an unused flag (kNoTransparencyMultiSample)
					if (shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kNoTransparencyMultiSample)) {
						RenderPass call{ a_pass, a_technique, a_alphaTest, a_renderFlags };
						singleton.renderPasses.push_back(call);
						return;
					}
				}
			}
		}
	}
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void TerrainBlending::RenderTerrainBlendingPasses()
{
	ZoneScoped;

	if (!settings.Enabled) {
		renderDepth = false;
		renderTerrainDepth = false;
		renderAltTerrain = false;
		terrainRenderPasses.clear();
		renderPasses.clear();
		auto renderer = globals::game::renderer;
		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		mainDepth.depthSRV = depthSRVBackup;
		zPrepassCopy.depthSRV = prepassSRVBackup;
		return;
	}

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto shadowState = globals::game::shadowState;
	auto stateUpdateFlags = globals::game::stateUpdateFlags;

	context->PSSetShaderResources(55, 1, &terrainDepth.depthSRV);

	const uint64_t terrainPassCount = static_cast<uint64_t>(terrainRenderPasses.size());
	const uint64_t noBlendPassCount = static_cast<uint64_t>(renderPasses.size());

	if (terrainPassCount != 0 || noBlendPassCount != 0) {
		TracyD3D11Zone(globals::state->tracyCtx, "Terrain Blending - Render Passes");
		if (globals::state->frameAnnotations)
			globals::state->BeginPerfEvent("Terrain Blending - Render Passes");

		auto& alphaBlendMode = shadowState->GetRuntimeData().alphaBlendMode;
		auto& alphaBlendWriteMode = shadowState->GetRuntimeData().alphaBlendWriteMode;
		auto& depthStencilDepthMode = shadowState->GetRuntimeData().depthStencilDepthMode;

		// Reset alpha write and enable alpha blending
		alphaBlendWriteMode = 1;
		alphaBlendMode = 1;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

		// Enable rendering for depth below the surface
		context->OMSetDepthStencilState(terrainDepthStencilState, 0xFF);

		for (auto& renderPass : terrainRenderPasses)
			Hooks::BSBatchRenderer__RenderPassImmediately::func(renderPass.a_pass, renderPass.a_technique, renderPass.a_alphaTest, renderPass.a_renderFlags);

		// Reset alpha blending
		alphaBlendMode = 0;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

		// Reset depth testing
		depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTestEqual;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);

		for (auto& renderPass : renderPasses)
			Hooks::BSBatchRenderer__RenderPassImmediately::func(renderPass.a_pass, renderPass.a_technique, renderPass.a_alphaTest, renderPass.a_renderFlags);

		terrainRenderPasses.clear();
		renderPasses.clear();

		if (globals::state->frameAnnotations)
			globals::state->EndPerfEvent();
	}

	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	mainDepth.depthSRV = depthSRVBackup;
	// Point the prepass copy back at its own texture: Deferred copies the final depth into it,
	// and everything after this pass (water included) should read that rather than blended prepass depth.
	auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	zPrepassCopy.depthSRV = prepassSRVBackup;
}
#undef I18N_KEY_PREFIX
