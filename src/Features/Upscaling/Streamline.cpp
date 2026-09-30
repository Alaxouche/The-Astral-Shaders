#include "Streamline.h"
#include "Features/ReverseZ.h"

#include <algorithm>
#include <cmath>
#include <dxgi.h>
#include <dxgi1_3.h>

#include "../../Deferred.h"
#include "../../Hooks.h"
#include "../../State.h"
#include "../../Util.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"
#include "Utils/NvApiDrs.h"

void LoggingCallback(sl::LogType type, const char* msg)
{
	// Remove trailing newlines from the raw message
	std::string rawMsg(msg);
	while (!rawMsg.empty() && (rawMsg.back() == '\n' || rawMsg.back() == '\r'))
		rawMsg.pop_back();

	// Remove leading bracketed metadata
	const char* p = msg;
	while (*p == '[') {
		const char* close = strchr(p, ']');
		if (!close)
			break;
		p = close + 1;
		// Skip whitespace after each bracketed section
		while (*p == ' ' || *p == '\t') ++p;
	}
	// Now p points to the first non-bracketed section (file/line info or message)
	std::string cleanMsg(p);
	// Trim leading/trailing whitespace and newlines
	size_t start = cleanMsg.find_first_not_of(" \t\r\n");
	size_t end = cleanMsg.find_last_not_of(" \t\r\n");
	if (start != std::string::npos && end != std::string::npos)
		cleanMsg = cleanMsg.substr(start, end - start + 1);
	else
		cleanMsg.clear();

	// If the cleaned message is empty or only bracketed tokens, log the raw message
	bool onlyBrackets = true;
	for (char c : cleanMsg) {
		if (c != '[' && c != ']' && c != ' ' && c != '\t') {
			onlyBrackets = false;
			break;
		}
	}
	if (cleanMsg.empty() || onlyBrackets) {
		logger::info("[StreamlineSDK:RAW] {}", rawMsg);
		return;
	}

	// Use a clear prefix
	const char* prefix = "[StreamlineSDK]";
	switch (type) {
	case sl::LogType::eInfo:
		logger::info("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eWarn:
		logger::warn("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eError:
		logger::error("{} {}", prefix, cleanMsg);
		break;
	}
}

std::vector<std::pair<std::string, std::string>> Streamline::dllVersions = {};

void Streamline::LoadInterposer()
{
	triedInitialization = true;

	const std::filesystem::path pluginDirPath(pluginDir);
	std::wstring interposerPath = (pluginDirPath / L"sl.interposer.dll").wstring();
	interposer = LoadLibraryW(interposerPath.c_str());
	if (interposer == nullptr) {
		DWORD errorCode = GetLastError();
		logger::info("[Streamline{}] Failed to load interposer: Error Code {:x}", logSuffix, errorCode);
		return;
	} else {
		logger::info("[Streamline{}] Interposer loaded at address: {:p}", logSuffix, static_cast<void*>(interposer));
	}

	// The diagnostics table lists the D3D11 directory; the D3D12 one holds the same plugins plus DLSS-G.
	if (renderAPI == sl::RenderAPI::eD3D11) {
		Streamline::dllVersions = Util::EnumerateDllVersions(pluginDirPath);
		for (const auto& [name, versionStr] : Streamline::dllVersions)
			logger::info("[Streamline] {} version: {}", name, versionStr);
	} else {
		for (const auto& [name, versionStr] : Util::EnumerateDllVersions(pluginDirPath))
			logger::info("[Streamline{}] {} version: {}", logSuffix, name, versionStr);
	}

	logger::info("[Streamline{}] Initializing Streamline", logSuffix);

	sl::Preferences pref;

	// D3D11 instance: DLSS upscaling + Reflex/PCL. D3D12 instance: DLSS-G + Reflex/PCL (DLSS-G requires Reflex).
	sl::Feature featuresDX11[] = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL };
	sl::Feature featuresDX12[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
	if (renderAPI == sl::RenderAPI::eD3D11) {
		pref.featuresToLoad = featuresDX11;
		pref.numFeaturesToLoad = _countof(featuresDX11);
	} else {
		pref.featuresToLoad = featuresDX12;
		pref.numFeaturesToLoad = _countof(featuresDX12);
	}

	// Set log level from settings
	switch (globals::features::upscaling.settings.streamlineLogLevel) {
	case 2:
		pref.logLevel = sl::LogLevel::eVerbose;
		break;
	case 1:
		pref.logLevel = sl::LogLevel::eDefault;
		break;
	case 0:
	default:
		pref.logLevel = sl::LogLevel::eOff;
		break;
	}
	pref.logMessageCallback = LoggingCallback;
	pref.showConsole = false;
	std::error_code pluginPathError;
	auto pluginDirAbsolute = std::filesystem::absolute(pluginDirPath, pluginPathError);
	if (pluginPathError)
		pluginDirAbsolute = pluginDirPath;
	pluginDirAbsoluteW = pluginDirAbsolute.wstring();
	pluginPaths[0] = pluginDirAbsoluteW.c_str();
	pref.pathsToPlugins = pluginPaths;
	pref.numPathsToPlugins = 1;
	logger::info("[Streamline{}] Plugin search path: {}", logSuffix, pluginDirAbsolute.string());

	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";

	pref.renderAPI = renderAPI;
	pref.flags = sl::PreferenceFlags::eUseManualHooking;
	// DLSS-G inputs are tagged per frame token on the present command list.
	if (renderAPI == sl::RenderAPI::eD3D12)
		pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;

	// Hook up all of the functions exported by the SL Interposer Library
	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(interposer, "slIsFeatureSupported");
	slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)GetProcAddress(interposer, "slIsFeatureLoaded");
	slSetFeatureLoaded = (PFun_slSetFeatureLoaded*)GetProcAddress(interposer, "slSetFeatureLoaded");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
	slAllocateResources = (PFun_slAllocateResources*)GetProcAddress(interposer, "slAllocateResources");
	slFreeResources = (PFun_slFreeResources*)GetProcAddress(interposer, "slFreeResources");
	slSetTag = (PFun_slSetTag*)GetProcAddress(interposer, "slSetTag");
	slSetTagForFrame = (PFun_slSetTagForFrame*)GetProcAddress(interposer, "slSetTagForFrame");
	slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GetProcAddress(interposer, "slGetFeatureRequirements");
	slGetFeatureVersion = (PFun_slGetFeatureVersion*)GetProcAddress(interposer, "slGetFeatureVersion");
	slUpgradeInterface = (PFun_slUpgradeInterface*)GetProcAddress(interposer, "slUpgradeInterface");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetNativeInterface = (PFun_slGetNativeInterface*)GetProcAddress(interposer, "slGetNativeInterface");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");

	if (SL_FAILED(res, slInit(pref, sl::kSDKVersion))) {
		logger::critical("[Streamline{}] Failed to initialize Streamline", logSuffix);
	} else {
		initialized = true;
		featureDLSS = false;
		featureDLSSG = false;
		featureReflex = false;
		featurePCL = false;
		reflexSupportedOnCurrentAdapter = false;
		reflexOptionsCache = {};
		lastReflexSleepFrame = UINT32_MAX;
		logger::info("[Streamline{}] Successfully initialized Streamline", logSuffix);
	}
}

void Streamline::CheckFeatures(IDXGIAdapter* a_adapter)
{
	logger::info("[Streamline{}] Checking features", logSuffix);
	DXGI_ADAPTER_DESC adapterDesc;
	a_adapter->GetDesc(&adapterDesc);
	reflexSupportedOnCurrentAdapter = adapterDesc.VendorId == kNvidiaVendorId;

	sl::AdapterInfo adapterInfo;
	adapterInfo.deviceLUID = (uint8_t*)&adapterDesc.AdapterLuid;
	adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

	auto checkFeatureAvailability = [&](sl::Feature feature, const char* featureName, bool& outAvailable) {
		outAvailable = false;
		bool loaded = false;
		if (SL_FAILED(result, slIsFeatureLoaded(feature, loaded))) {
			logger::warn("[Streamline{}] {} load-state query failed: {}", logSuffix, featureName, magic_enum::enum_name(result));
			return;
		}
		if (!loaded) {
			logger::info("[Streamline{}] {} feature is not loaded", logSuffix, featureName);
			sl::FeatureRequirements featureRequirements;
			sl::Result requirementsResult = slGetFeatureRequirements(feature, featureRequirements);
			if (requirementsResult != sl::Result::eOk) {
				logger::info("[Streamline{}] {} feature failed to load due to: {}", logSuffix, featureName, magic_enum::enum_name(requirementsResult));
			}
			return;
		}

		logger::info("[Streamline{}] {} feature is loaded", logSuffix, featureName);
		outAvailable = slIsFeatureSupported(feature, adapterInfo) == sl::Result::eOk;
	};

	if (renderAPI == sl::RenderAPI::eD3D11) {
		checkFeatureAvailability(sl::kFeatureDLSS, "DLSS", featureDLSS);

		if (featureDLSS) {
			isRTXBelow40series = IsRTXAndBelow40Series(a_adapter);

			if (isRTXBelow40series)
				logger::info("[Streamline] Older RTX GPU detected, DLSS 4.0 will be used instead of DLSS 4.5");
			else
				logger::info("[Streamline] Newer RTX GPU detected, DLSS 4.5 will be used instead of DLSS 4.0");
		}

		logger::info("[Streamline] DLSS {} available", featureDLSS ? "is" : "is not");
	} else {
		checkFeatureAvailability(sl::kFeatureDLSS_G, "DLSS-G", featureDLSSG);
		logger::info("[Streamline{}] DLSS-G {} available", logSuffix, featureDLSSG ? "is" : "is not");
	}

	if (reflexSupportedOnCurrentAdapter) {
		checkFeatureAvailability(sl::kFeatureReflex, "Reflex", featureReflex);
		checkFeatureAvailability(sl::kFeaturePCL, "PCL", featurePCL);
		logger::info("[Streamline{}] Reflex {} available", logSuffix, featureReflex ? "is" : "is not");
		logger::info("[Streamline{}] PCL {} available", logSuffix, featurePCL ? "is" : "is not");
	} else {
		featureReflex = false;
		featurePCL = false;
		logger::info("[Streamline{}] Reflex/PCL disabled on non-NVIDIA adapter", logSuffix);
	}

	reflexOptionsCache = {};
	lastReflexSleepFrame = UINT32_MAX;
}

bool Streamline::BindFeatureFunction(sl::Feature a_feature, const char* a_functionName, void*& a_function)
{
	a_function = nullptr;
	const sl::Result bindResult = slGetFeatureFunction(a_feature, a_functionName, a_function);
	if (bindResult != sl::Result::eOk)
		logger::warn("[Streamline{}] {} bind failed with {}", logSuffix, a_functionName, magic_enum::enum_name(bindResult));
	return bindResult == sl::Result::eOk && a_function != nullptr;
}

void Streamline::BindReflexAndPCL()
{
	slReflexGetState = nullptr;
	slReflexSleep = nullptr;
	slReflexSetOptions = nullptr;
	slPCLSetMarker = nullptr;
	featureReflex = false;
	featurePCL = false;

	if (!slGetFeatureFunction || !reflexSupportedOnCurrentAdapter) {
		if (!reflexSupportedOnCurrentAdapter)
			logger::info("[Streamline{}] Skipping Reflex/PCL binding on non-NVIDIA adapter", logSuffix);
		return;
	}

	if (slSetFeatureLoaded) {
		// Reflex/PCL availability can change after device bind; request explicit load here.
		const auto requestFeatureLoad = [&](sl::Feature feature, const char* featureName) {
			const sl::Result loadResult = slSetFeatureLoaded(feature, true);
			if (loadResult != sl::Result::eOk)
				logger::warn("[Streamline{}] Failed to request {} load: {}", logSuffix, featureName, magic_enum::enum_name(loadResult));
		};

		requestFeatureLoad(sl::kFeatureReflex, "Reflex");
		requestFeatureLoad(sl::kFeaturePCL, "PCL");
	}

	// Keep runtime controls strict: only advertise Reflex/PCL as available when required entry points bind.
	bool reflexFnsBound = true;
	reflexFnsBound &= BindFeatureFunction(sl::kFeatureReflex, "slReflexGetState", (void*&)slReflexGetState);
	reflexFnsBound &= BindFeatureFunction(sl::kFeatureReflex, "slReflexSleep", (void*&)slReflexSleep);
	reflexFnsBound &= BindFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", (void*&)slReflexSetOptions);
	featureReflex = reflexFnsBound && slReflexSetOptions && slReflexSleep;

	if (!featureReflex) {
		logger::warn("[Streamline{}] Reflex functions are missing; Reflex runtime controls will be disabled", logSuffix);
	} else {
		logger::info("[Streamline{}] Reflex runtime controls are available", logSuffix);
	}

	bool pclFnBound = BindFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", (void*&)slPCLSetMarker);
	featurePCL = pclFnBound && slPCLSetMarker;
	if (!featurePCL) {
		logger::warn("[Streamline{}] PCL marker function is unavailable; marker optimization requests will be ignored", logSuffix);
	} else {
		logger::info("[Streamline{}] PCL marker interface is available", logSuffix);
	}
}

void Streamline::PostDevice()
{
	// Hook up all of the feature functions using the sl function slGetFeatureFunction

	if (renderAPI == sl::RenderAPI::eD3D12) {
		slDLSSGGetState = nullptr;
		slDLSSGSetOptions = nullptr;
		dlssgMaxFramesToGenerate = 1;

		if (featureDLSSG && slGetFeatureFunction) {
			bool dlssgFnsBound = true;
			dlssgFnsBound &= BindFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", (void*&)slDLSSGGetState);
			dlssgFnsBound &= BindFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)slDLSSGSetOptions);
			featureDLSSG = dlssgFnsBound && slDLSSGGetState && slDLSSGSetOptions;

			if (!featureDLSSG) {
				logger::warn("[Streamline{}] DLSS-G functions missing; DLSS-G will not be used", logSuffix);
			} else {
				logger::info("[Streamline{}] DLSS-G runtime controls are available", logSuffix);

				sl::DLSSGState state{};
				if (SL_FAILED(result, slDLSSGGetState(viewport, state, nullptr))) {
					logger::warn("[Streamline{}] slDLSSGGetState failed querying numFramesToGenerateMax: {}", logSuffix, magic_enum::enum_name(result));
				} else {
					dlssgMaxFramesToGenerate = std::max<uint32_t>(1, state.numFramesToGenerateMax);
					logger::info("[Streamline{}] DLSS-G supports up to {}x frame generation", logSuffix, dlssgMaxFramesToGenerate + 1);
				}
			}
		}

		BindReflexAndPCL();

		reflexOptionsCache = {};
		lastReflexSleepFrame = UINT32_MAX;
		return;
	}

	if (featureDLSS) {
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", (void*&)slDLSSGetOptimalSettings);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetState", (void*&)slDLSSGetState);
		slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", (void*&)slDLSSSetOptions);
	}

	BindReflexAndPCL();

	reflexOptionsCache = {};
	lastReflexSleepFrame = UINT32_MAX;
}

void Streamline::SetD3DDevice12(ID3D12Device* a_device)
{
	if (!initialized || !slSetD3DDevice || !a_device)
		return;
	if (SL_FAILED(result, slSetD3DDevice(static_cast<void*>(a_device))))
		logger::error("[Streamline{}] slSetD3DDevice(device) failed: {}", logSuffix, magic_enum::enum_name(result));
	else
		logger::info("[Streamline{}] D3D12 device bound", logSuffix);
}

void Streamline::EnsureDriverProfileAllowsDLSSG()
{
	Util::NvApiDrs::Api drs{};
	uint32_t disableValue = 0;
	// No profile/setting means driver defaults, which allow DLSS-G.
	if (!drs.TryGetSkyrimSetting(Util::NvApiDrs::kKeyDLSSGDisable, disableValue) || disableValue == 0)
		return;

	// The NVIDIA App may re-assert the key later, so this runs every boot.
	logger::info("[Streamline DX12] Driver profile disables DLSS-G (DRS key {:#x}={}); resetting to driver default",
		Util::NvApiDrs::kKeyDLSSGDisable, disableValue);

	Util::NvApiDrs::SessionHandle session{};
	Util::NvApiDrs::ProfileHandle profile{};
	if (!drs.TryOpenSkyrimProfile(session, profile))
		return;

	Util::NvApiDrs::Setting newSetting{};
	newSetting.version = Util::NvApiDrs::kSettingVersion;
	newSetting.settingId = Util::NvApiDrs::kKeyDLSSGDisable;
	newSetting.settingType = 0;
	newSetting.u32CurrentValue = 0;
	if (drs.SetSetting(session, profile, &newSetting) != 0 || drs.SaveSettings(session) != 0)
		logger::warn(
			"[Streamline DX12] Failed to reset the DRS key; DLSS-G will report eOk but generate no frames. "
			"Disable the DLSS override for Skyrim in the NVIDIA App, or clear key {:#x} with NVIDIA Profile Inspector.",
			Util::NvApiDrs::kKeyDLSSGDisable);
	else
		logger::info("[Streamline DX12] DRS key reset; if frame generation does not engage this session, restart the game");

	drs.DestroySession(session);
}

bool Streamline::IsSmoothMotionEnabledForProfile()
{
	Util::NvApiDrs::Api drs{};
	uint32_t enableValue = 0;
	return drs.TryGetSkyrimSetting(Util::NvApiDrs::kKeySmoothMotionEnable, enableValue) && enableValue != 0;
}

/**
 * @brief Updates and sets camera and frame constants for the current Streamline frame.
 *
 * Populates and submits camera parameters, projection matrices, motion vector settings, and other per-frame constants to the Streamline SDK for the current frame. Uses cached framebuffer data and global state to ensure correct configuration for upscaling and frame generation features.
 */
bool Streamline::EnsureFrameToken()
{
	if (!initialized || !slGetNewFrameToken || !globals::state)
		return false;

	if (!frameChecker.IsNewFrame())
		return frameToken != nullptr;

	if (SL_FAILED(result, slGetNewFrameToken(frameToken, &globals::state->frameCount))) {
		logger::error("[Streamline{}] Could not get frame token: {}", logSuffix, magic_enum::enum_name(result));
		frameToken = nullptr;
		return false;
	}

	return frameToken != nullptr;
}

bool Streamline::CheckFrameConstants(sl::ViewportHandle p_viewport)
{
	if (!initialized)
		return false;

	if (!EnsureFrameToken())
		return false;

	sl::Constants slConstants = {};

	slConstants.cameraAspectRatio = (float)globals::game::graphicsState->screenWidth / (float)globals::game::graphicsState->screenHeight;

	slConstants.cameraFOV = Util::GetVerticalFOVRad();
	slConstants.cameraNear = *globals::game::cameraNear;
	slConstants.cameraFar = *globals::game::cameraFar;

	auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse().Transpose();
	auto cameraViewToClip = globals::game::frameBufferCached.GetCameraProjUnjittered().Transpose();

	slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
	slConstants.cameraPinholeOffset = { 0.f, 0.f };
	slConstants.cameraRight = { viewMatrix._11, viewMatrix._12, viewMatrix._13 };
	slConstants.cameraUp = { viewMatrix._21, viewMatrix._22, viewMatrix._23 };
	slConstants.cameraFwd = { viewMatrix._31, viewMatrix._32, viewMatrix._33 };
	slConstants.cameraPos = *(sl::float3*)&globals::game::frameBufferCached.GetCameraPosAdjust();
	slConstants.cameraViewToClip = *(sl::float4x4*)&cameraViewToClip;
	slConstants.depthInverted = globals::features::reverseZ.IsActive() ? sl::Boolean::eTrue : sl::Boolean::eFalse;

	recalculateCameraMatrices(slConstants);

	auto& upscaling = globals::features::upscaling;
	auto jitter = upscaling.jitter;
	slConstants.jitterOffset = { -jitter.x, -jitter.y };
	slConstants.reset = sl::Boolean::eFalse;

	slConstants.mvecScale = { 1.0f, 1.0f };
	slConstants.motionVectors3D = sl::Boolean::eFalse;
	slConstants.motionVectorsInvalidValue = FLT_MIN;
	slConstants.orthographicProjection = sl::Boolean::eFalse;
	slConstants.motionVectorsDilated = sl::Boolean::eFalse;
	slConstants.motionVectorsJittered = sl::Boolean::eFalse;

	if (SL_FAILED(res, slSetConstants(slConstants, *frameToken, p_viewport))) {
		logger::error("[Streamline{}] Could not set constants", logSuffix);
		return false;
	}

	return true;
}

bool Streamline::IsRTXAndBelow40Series(IDXGIAdapter* a_adapter)
{
	DXGI_ADAPTER_DESC adapterDesc = {};

	a_adapter->GetDesc(&adapterDesc);

	UINT vendorId = adapterDesc.VendorId;
	UINT deviceId = adapterDesc.DeviceId;

	// Check if NVIDIA
	if (vendorId != kNvidiaVendorId)
		return false;

	// RTX 30 series (Ampere) - 0x2200-0x25FF
	if (deviceId >= 0x2200 && deviceId <= 0x2600)
		return true;

	// RTX 20 series (Turing with RT cores) - 0x1E00-0x1FFF
	if (deviceId >= 0x1E00 && deviceId <= 0x1FFF)
		return true;

	return false;
}

void Streamline::SetDLSSOptions(sl::ViewportHandle p_viewport, uint32_t width)
{
	sl::DLSSOptions dlssOptions{};

	// Map quality mode to DLSS mode
	uint32_t qualityMode = globals::features::upscaling.settings.qualityMode;
	switch (qualityMode) {
	case 1:
		dlssOptions.mode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssOptions.mode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssOptions.mode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssOptions.mode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssOptions.mode = sl::DLSSMode::eDLAA;
		break;
	}
	dlssOptions.outputWidth = width;
	dlssOptions.outputHeight = (uint)globals::game::graphicsState->screenHeight;

	// Detect HDR from kMAIN format at runtime
	{
		auto renderer = globals::game::renderer;
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		D3D11_TEXTURE2D_DESC mainDesc;
		static_cast<ID3D11Texture2D*>(main.texture)->GetDesc(&mainDesc);
		bool isHDR = mainDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM;
		dlssOptions.colorBuffersHDR = isHDR ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	}
	dlssOptions.useAutoExposure = sl::Boolean::eTrue;

	std::optional<sl::DLSSPreset> customPreset;
	switch (globals::features::upscaling.settings.presetDLSS) {
	case 1:
		customPreset = sl::DLSSPreset::ePresetJ;
		break;
	case 2:
		customPreset = sl::DLSSPreset::ePresetK;
		break;
	case 3:
		customPreset = sl::DLSSPreset::ePresetL;
		break;
	case 4:
		customPreset = sl::DLSSPreset::ePresetM;
		break;
	}

	if (customPreset.has_value()) {
		dlssOptions.dlaaPreset = customPreset.value();
		dlssOptions.ultraQualityPreset = customPreset.value();
		dlssOptions.qualityPreset = customPreset.value();
		dlssOptions.balancedPreset = customPreset.value();
		dlssOptions.performancePreset = customPreset.value();
		dlssOptions.ultraPerformancePreset = customPreset.value();
	} else if (isRTXBelow40series) {
		dlssOptions.dlaaPreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.ultraQualityPreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.qualityPreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.balancedPreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.performancePreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.ultraPerformancePreset = sl::DLSSPreset::ePresetM;
	} else {
		dlssOptions.dlaaPreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.ultraQualityPreset = sl::DLSSPreset::ePresetJ;
		dlssOptions.qualityPreset = sl::DLSSPreset::ePresetM;
		dlssOptions.balancedPreset = sl::DLSSPreset::ePresetM;
		dlssOptions.performancePreset = sl::DLSSPreset::ePresetM;
		dlssOptions.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
	}

	dlssOptions.preExposure = 1.0f;
	dlssOptions.sharpness = 0.0f;

	if (SL_FAILED(result, slDLSSSetOptions(p_viewport, dlssOptions))) {
		logger::critical("[Streamline] Could not enable DLSS");
	}
}

void Streamline::EvaluateDLSS(sl::ViewportHandle vp,
	ID3D11Resource* colorIn, ID3D11Resource* colorOut, ID3D11Resource* depth,
	ID3D11Resource* mvec, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask,
	const sl::Extent& extentIn, const sl::Extent& extentOut, uint32_t outputWidth)
{
	auto context = globals::d3d::context;

	sl::Resource colorInRes = { sl::ResourceType::eTex2d, colorIn, 0 };
	sl::Resource colorOutRes = { sl::ResourceType::eTex2d, colorOut, 0 };
	sl::Resource depthRes = { sl::ResourceType::eTex2d, depth, 0 };
	sl::Resource mvecRes = { sl::ResourceType::eTex2d, mvec, 0 };
	sl::Resource reactiveMaskRes = { sl::ResourceType::eTex2d, reactiveMask, 0 };
	sl::Resource transparencyMaskRes = { sl::ResourceType::eTex2d, transparencyMask, 0 };

	if (!CheckFrameConstants(vp))
		return;

	const bool emitPCLMarkers =
		globals::features::upscaling.settings.reflexUseMarkersToOptimize &&
		reflexOptionsCache.useMarkersToOptimize &&
		featurePCL;
	const auto emitPCLMarker = [&](sl::PCLMarker marker, const char* stageName, uint32_t stageIndex) {
		if (!emitPCLMarkers || !slPCLSetMarker || !frameToken)
			return;
		const sl::Result markerResult = slPCLSetMarker(marker, *frameToken);
		if (markerResult != sl::Result::eOk) {
			static bool markerErrorLogged[2] = { false, false };
			const uint32_t boundedStageIndex = std::min(stageIndex, 1u);
			if (markerErrorLogged[boundedStageIndex])
				return;
			markerErrorLogged[boundedStageIndex] = true;
			logger::warn(
				"[Streamline] slPCLSetMarker({}) failed: {}",
				stageName,
				magic_enum::enum_name(markerResult));
		}
	};

	SetDLSSOptions(vp, outputWidth);

	sl::ResourceTag tags[] = {
		{ &colorInRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &extentIn },
		{ &colorOutRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &extentOut },
		{ &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &reactiveMaskRes, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &extentIn },
		{ &transparencyMaskRes, sl::kBufferTypeTransparencyHint, sl::ResourceLifecycle::eValidUntilPresent, &extentIn }
	};

	slSetTag(vp, tags, _countof(tags), context);

	sl::ViewportHandle view(vp);
	const sl::BaseStructure* inputs[] = { &view };

	auto state = globals::state;
	if (state->frameAnnotations)
		state->BeginPerfEvent("DLSS Evaluate");

	emitPCLMarker(sl::PCLMarker::eRenderSubmitStart, "DLSS-EvaluateStart", 0);
	sl::Result evalResult = slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), context);
	emitPCLMarker(sl::PCLMarker::eRenderSubmitEnd, "DLSS-EvaluateEnd", 1);

	if (state->frameAnnotations)
		state->EndPerfEvent();

	if (evalResult != sl::Result::eOk) {
		static bool evalErrorLogged = false;
		if (!evalErrorLogged) {
			evalErrorLogged = true;
			logger::error("[Streamline] slEvaluateFeature failed result={}", (int)evalResult);
		}
	}
}

void Streamline::Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors)
{
	auto renderer = globals::game::renderer;
	auto& depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	auto renderSize = Util::ConvertToDynamic(screenSize);

	// DLSS input and output must not alias. Always write to the intermediate texture,
	// then either sharpen or copy the result back to kMAIN.
	auto& upscaling = globals::features::upscaling;
	ID3D11Resource* colorOut =
		upscaling.sharpenerTexture ? upscaling.sharpenerTexture->resource.get() : a_upscalingTexture;

	sl::Extent extentIn{ 0, 0, (uint)renderSize.x, (uint)renderSize.y };
	sl::Extent extentOut{ 0, 0, (uint)screenSize.x, (uint)screenSize.y };

	EvaluateDLSS(viewport,
		a_upscalingTexture, colorOut,
		depthTexture.texture, a_motionVectors, a_reactiveMask, a_transparencyCompositionMask,
		extentIn, extentOut, (uint)screenSize.x);
}

void Streamline::UpdateReflex()
{
	if (!initialized || !reflexSupportedOnCurrentAdapter || !featureReflex || !slReflexSetOptions)
		return;

	const auto applyReflexOptionsIfChanged = [&](const sl::ReflexOptions& options, const char* onFailMessage) {
		if (reflexOptionsCache.valid &&
			reflexOptionsCache.mode == options.mode &&
			reflexOptionsCache.frameLimitUs == options.frameLimitUs &&
			reflexOptionsCache.useMarkersToOptimize == options.useMarkersToOptimize) {
			return;
		}

		if (SL_FAILED(result, slReflexSetOptions(options))) {
			logger::error("[Streamline{}] {}: {}", logSuffix, onFailMessage, magic_enum::enum_name(result));
			return;
		}

		reflexOptionsCache.valid = true;
		reflexOptionsCache.mode = options.mode;
		reflexOptionsCache.frameLimitUs = options.frameLimitUs;
		reflexOptionsCache.useMarkersToOptimize = options.useMarkersToOptimize;
	};

	const auto& upscaling = globals::features::upscaling;

	// The D3D11 instance stands down while the frame-generation swap chain is active: with
	// DLSS-G the D3D12 instance drives Reflex, and FSR3 FG never took Reflex markers on this path.
	if (renderAPI == sl::RenderAPI::eD3D11 && upscaling.IsFrameGenerationDx12PathActive()) {
		sl::ReflexOptions disabledOptions{};
		disabledOptions.mode = sl::ReflexMode::eOff;
		disabledOptions.frameLimitUs = 0u;
		disabledOptions.useMarkersToOptimize = false;
		applyReflexOptionsIfChanged(disabledOptions, "Failed to disable Reflex while frame-generation DX12 path is active");
		return;
	}

	auto& settings = globals::features::upscaling.settings;

	sl::ReflexOptions options{};
	if (renderAPI == sl::RenderAPI::eD3D12) {
		// DLSS-G requires at least low latency mode while it generates frames.
		const bool needReflex = upscaling.ShouldPrepareFrameGeneration() || settings.reflexLowLatencyMode;
		if (needReflex)
			options.mode = settings.reflexLowLatencyBoost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
		else
			options.mode = sl::ReflexMode::eOff;
	} else if (!settings.reflexLowLatencyMode) {
		options.mode = sl::ReflexMode::eOff;
	} else {
		options.mode = settings.reflexLowLatencyBoost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
	}

	const float originalReflexFPSLimit = settings.reflexFPSLimit;
	float reflexFPSLimit = originalReflexFPSLimit;
	if (!std::isfinite(reflexFPSLimit)) {
		reflexFPSLimit = 60.0f;
		settings.reflexFPSLimit = reflexFPSLimit;
		logger::warn("[Streamline{}] reflexFPSLimit is not finite ({}), using {}", logSuffix, originalReflexFPSLimit, reflexFPSLimit);
	}
	const float fpsLimit = std::clamp(reflexFPSLimit, 20.0f, 240.0f);
	options.frameLimitUs = settings.reflexUseFPSLimit ? static_cast<uint32_t>(std::lround(1000000.0 / static_cast<double>(fpsLimit))) : 0u;
	options.useMarkersToOptimize = settings.reflexUseMarkersToOptimize && featurePCL;

	applyReflexOptionsIfChanged(options, "Failed to apply Reflex options");

	if (!slReflexSleep)
		return;

	if (options.mode == sl::ReflexMode::eOff && options.frameLimitUs == 0)
		return;

	const uint32_t currentFrame = globals::state ? globals::state->frameCount : 0;
	// PollInputDevices can run more than once; sleep must happen once per frame token.
	if (lastReflexSleepFrame == currentFrame)
		return;

	if (!EnsureFrameToken())
		return;

	lastReflexSleepFrame = currentFrame;
	if (SL_FAILED(result, slReflexSleep(*frameToken))) {
		logger::warn("[Streamline{}] Reflex sleep call failed: {}", logSuffix, magic_enum::enum_name(result));
	}

	// The frame's simulation begins right after the Reflex sleep returns; the matching
	// eSimulationEnd and the render/present markers are emitted on the present path.
	if (renderAPI == sl::RenderAPI::eD3D12)
		EmitPCLMarker(sl::PCLMarker::eSimulationStart);
}

void Streamline::EmitPCLMarker(sl::PCLMarker a_marker)
{
	if (!initialized || !featurePCL || !slPCLSetMarker)
		return;
	if (!EnsureFrameToken())
		return;

	if (SL_FAILED(result, slPCLSetMarker(a_marker, *frameToken))) {
		static bool errorLogged = false;
		if (!errorLogged) {
			errorLogged = true;
			logger::warn("[Streamline{}] slPCLSetMarker({}) failed: {}", logSuffix,
				magic_enum::enum_name(a_marker), magic_enum::enum_name(result));
		}
	}
}

void Streamline::ConfigureDLSSG(bool a_enabled)
{
	if (!initialized || !slDLSSGSetOptions)
		return;

	sl::DLSSGOptions options{};
	options.mode = a_enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
	options.numFramesToGenerate = std::clamp<uint32_t>(
		globals::features::upscaling.settings.dlssgFramesToGenerate, 1, dlssgMaxFramesToGenerate);

	if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
		static bool errorLogged = false;
		if (!errorLogged) {
			errorLogged = true;
			logger::error("[Streamline{}] slDLSSGSetOptions failed: {}", logSuffix, magic_enum::enum_name(result));
		}
	}

	if (slDLSSGGetState && a_enabled) {
		sl::DLSSGState state{};
		if (SL_FAILED(stateResult, slDLSSGGetState(viewport, state, &options))) {
			static uint32_t stateFailCount = 0;
			if (++stateFailCount % 60 == 0) {
				logger::warn("[Streamline{}] slDLSSGGetState has failed {} times: {}", logSuffix, stateFailCount, magic_enum::enum_name(stateResult));
			}
		} else {
			lastDLSSGStatus = state.status;
			lastDLSSGFramesPresented = state.numFramesActuallyPresented;

			static sl::DLSSGStatus lastLoggedStatus = sl::DLSSGStatus::eOk;
			if (state.status != sl::DLSSGStatus::eOk && state.status != lastLoggedStatus) {
				lastLoggedStatus = state.status;
				logger::warn("[Streamline{}] DLSS-G not generating frames this session: status={}", logSuffix,
					magic_enum::enum_name(state.status));
			} else if (state.status == sl::DLSSGStatus::eOk && lastLoggedStatus != sl::DLSSGStatus::eOk) {
				lastLoggedStatus = sl::DLSSGStatus::eOk;
				logger::info("[Streamline{}] DLSS-G status recovered to eOk, now generating frames", logSuffix);
			}

			// status eOk does not confirm interpolation; numFramesActuallyPresented does.
			if (globals::features::upscaling.settings.streamlineLogLevel >= 2) {
				static uint32_t callCount = 0;
				if (++callCount % 120 == 0) {
					logger::info("[Streamline{}] DLSS-G presented {} frames since last query (requested {}x)", logSuffix,
						state.numFramesActuallyPresented, options.numFramesToGenerate + 1);
				}
			}
		}
	}
}

void Streamline::TagDX12Resources(ID3D12GraphicsCommandList* a_cmdList,
	ID3D12Resource* a_depth, ID3D12Resource* a_mvec, ID3D12Resource* a_hudLessColor,
	ID3D12Resource* a_uiColorAndAlpha, uint32_t a_width, uint32_t a_height)
{
	if (!initialized || !slSetTagForFrame || !frameToken || !a_cmdList || !a_depth || !a_mvec || !a_hudLessColor)
		return;

	sl::Extent extent{ 0, 0, a_width, a_height };

	// Depth and motion vectors only carry data in the render-resolution sub-rectangle of their
	// display-sized targets; declare that extent, matching the FSR FG dispatch.
	const float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	const float2 renderSizeF = screenSize * globals::features::upscaling.resolutionScale;
	sl::Extent renderExtent{ 0, 0, static_cast<uint32_t>(renderSizeF.x), static_cast<uint32_t>(renderSizeF.y) };

	sl::Resource depthRes = { sl::ResourceType::eTex2d, a_depth, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource mvecRes = { sl::ResourceType::eTex2d, a_mvec, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource hudLessRes = { sl::ResourceType::eTex2d, a_hudLessColor, D3D12_RESOURCE_STATE_COMMON };
	sl::Resource uiRes = { sl::ResourceType::eTex2d, a_uiColorAndAlpha, D3D12_RESOURCE_STATE_COMMON };

	// The UI alpha tag lets DLSS-G composite the interface onto generated frames instead of
	// interpolating its pixels as scene motion. It stays the last entry so the count below can
	// drop it when no UI buffer exists this frame.
	sl::ResourceTag tags[] = {
		{ &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
		{ &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent },
		{ &hudLessRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extent },
		{ &uiRes, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &extent },
	};
	const uint32_t numTags = a_uiColorAndAlpha ? _countof(tags) : _countof(tags) - 1;

	slSetTagForFrame(*frameToken, viewport, tags, numTags, a_cmdList);
}

/**
 * @brief Releases DLSS resources and disables DLSS for the current viewport.
 *
 * Sets the DLSS mode to off and frees all DLSS-related resources associated with the viewport.
 */
void Streamline::DestroyDLSSResources()
{
	// DLSS entry points are only resolved when DLSS is available; calling them otherwise faults.
	if (!featureDLSS || !slDLSSSetOptions)
		return;

	sl::DLSSOptions dlssOptions{};
	dlssOptions.mode = sl::DLSSMode::eOff;

	slDLSSSetOptions(viewport, dlssOptions);
	slFreeResources(sl::kFeatureDLSS, viewport);
}
