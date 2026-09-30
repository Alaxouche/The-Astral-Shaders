#pragma once

#include "../../Buffer.h"
#include "../../State.h"

#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>

#define NV_WINDOWS

#pragma warning(push)
#pragma warning(disable: 4471)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_matrix_helpers.h>
#include <sl_reflex.h>
#include <sl_version.h>
#pragma warning(pop)

/**
 * @brief Manages one NVIDIA Streamline instance.
 *
 * Upscaling owns two: a D3D11 instance for DLSS upscaling, Reflex and PCL, and a D3D12 instance
 * for DLSS Frame Generation (DLSS-G), which only exists in D3D12 and therefore runs on the
 * frame-generation proxy swap chain. Each instance loads its own interposer from its own plugin
 * directory; the API-specific behaviour is selected by `renderAPI`.
 */
class Streamline
{
public:
	static constexpr const wchar_t* PluginDir = L"Data\\Shaders\\Upscaling\\Streamline";
	static constexpr const wchar_t* PluginDirDX12 = L"Data\\Shaders\\Upscaling\\StreamlineDX12";
	static constexpr UINT kNvidiaVendorId = 0x10DE;

	Streamline() = default;

	/** @brief Returns the short identifier used for logging. */
	inline std::string GetShortName() { return "Streamline"; }

	// Instance configuration, set before LoadInterposer.
	sl::RenderAPI renderAPI = sl::RenderAPI::eD3D11;
	const wchar_t* pluginDir = PluginDir;
	// Appended to the "[Streamline" log prefix: "" for the D3D11 instance, " DX12" for the other.
	const char* logSuffix = "";

	bool enabledAtBoot = false;
	bool initialized = false;
	bool triedInitialization = false;

	bool featureDLSS = false;
	bool featureDLSSG = false;
	bool featureReflex = false;
	bool featurePCL = false;
	bool reflexSupportedOnCurrentAdapter = false;

	// DLSS-G (D3D12 instance only).
	uint32_t dlssgMaxFramesToGenerate = 1;
	sl::DLSSGStatus lastDLSSGStatus = sl::DLSSGStatus::eOk;
	uint32_t lastDLSSGFramesPresented = 0;

	sl::ViewportHandle viewport{ 0 };
	static constexpr uint32_t MAX_RESOLUTION = 8192;
	HMODULE interposer = NULL;

	// SL Interposer Functions
	PFun_slInit* slInit{};
	PFun_slShutdown* slShutdown{};
	PFun_slIsFeatureSupported* slIsFeatureSupported{};
	PFun_slIsFeatureLoaded* slIsFeatureLoaded{};
	PFun_slSetFeatureLoaded* slSetFeatureLoaded{};
	PFun_slEvaluateFeature* slEvaluateFeature{};
	PFun_slAllocateResources* slAllocateResources{};
	PFun_slFreeResources* slFreeResources{};
	PFun_slSetTag* slSetTag{};
	PFun_slSetTagForFrame* slSetTagForFrame{};
	PFun_slGetFeatureRequirements* slGetFeatureRequirements{};
	PFun_slGetFeatureVersion* slGetFeatureVersion{};
	PFun_slUpgradeInterface* slUpgradeInterface{};
	PFun_slSetConstants* slSetConstants{};
	PFun_slGetNativeInterface* slGetNativeInterface{};
	PFun_slGetFeatureFunction* slGetFeatureFunction{};
	PFun_slGetNewFrameToken* slGetNewFrameToken{};
	PFun_slSetD3DDevice* slSetD3DDevice{};

	// DLSS specific functions
	PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings{};
	PFun_slDLSSGetState* slDLSSGetState{};
	PFun_slDLSSSetOptions* slDLSSSetOptions{};

	// DLSS-G specific functions
	PFun_slDLSSGGetState* slDLSSGGetState{};
	PFun_slDLSSGSetOptions* slDLSSGSetOptions{};

	// Reflex specific functions
	PFun_slReflexGetState* slReflexGetState{};
	PFun_slReflexSleep* slReflexSleep{};
	PFun_slReflexSetOptions* slReflexSetOptions{};
	PFun_slPCLSetMarker* slPCLSetMarker{};

	Util::FrameChecker frameChecker;
	sl::FrameToken* frameToken = nullptr;

	bool isRTXBelow40series = false;

	struct ReflexOptionsCache
	{
		bool valid = false;
		sl::ReflexMode mode = sl::ReflexMode::eOff;
		uint32_t frameLimitUs = 0;
		bool useMarkersToOptimize = false;
	};
	ReflexOptionsCache reflexOptionsCache{};
	uint32_t lastReflexSleepFrame = UINT32_MAX;

	/**
	 * @brief Executes DLSS evaluation for a single viewport with the given resources.
	 * @param vp The viewport handle identifying the DLSS instance.
	 * @param colorIn Input color texture to upscale.
	 * @param colorOut Output texture receiving the upscaled result.
	 * @param depth Depth buffer for temporal reprojection.
	 * @param mvec Per-pixel motion vectors.
	 * @param reactiveMask Reactive mask for temporal stability hints.
	 * @param transparencyMask Mask for transparency and composition handling.
	 * @param extentIn Input resolution extent.
	 * @param extentOut Output resolution extent.
	 * @param outputWidth Target output width for DLSS options.
	 */
	void EvaluateDLSS(sl::ViewportHandle vp,
		ID3D11Resource* colorIn, ID3D11Resource* colorOut, ID3D11Resource* depth,
		ID3D11Resource* mvec, ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask,
		const sl::Extent& extentIn, const sl::Extent& extentOut, uint32_t outputWidth);

	// Cached DLL version info for the D3D11 Streamline plugin directory
	static std::vector<std::pair<std::string, std::string>> dllVersions;

	/** @brief Loads the Streamline interposer DLL and initializes the SDK with feature preferences. */
	void LoadInterposer();

	/**
	 * @brief Queries available Streamline features (DLSS or DLSS-G, Reflex, PCL) on the given adapter.
	 * @param a_adapter The DXGI adapter to check feature support against.
	 */
	void CheckFeatures(IDXGIAdapter* a_adapter);

	/** @brief Binds the feature functions after the D3D device is created. */
	void PostDevice();

	/**
	 * @brief Binds the D3D12 device to the D3D12 instance.
	 * @param a_device The device DLSS-G runs on; must be the one the swap chain is created on.
	 */
	void SetD3DDevice12(ID3D12Device* a_device);

	/**
	 * @brief Clears the driver-profile key with which the NVIDIA App disables DLSS-G for the game.
	 *
	 * With that key set, sl.dlss_g reports eOk and generates nothing. Runs before any D3D device
	 * exists; a no-op without an NVIDIA driver or without the key.
	 */
	static void EnsureDriverProfileAllowsDLSSG();

	/** @brief True when the driver's own frame generation (Smooth Motion) is on for the game's profile. */
	static bool IsSmoothMotionEnabledForProfile();

	/** @brief Acquires a new frame token from Streamline for the current frame. */
	bool EnsureFrameToken();
	/**
	 * @brief Sets camera and jitter constants on the Streamline viewport for the current frame.
	 * @param p_viewport The viewport handle to configure.
	 * @return True if constants were set successfully.
	 */
	bool CheckFrameConstants(sl::ViewportHandle p_viewport);

	/**
	 * @brief Detects whether the GPU is an NVIDIA RTX card below the 40-series generation.
	 * @param a_adapter The DXGI adapter to inspect.
	 * @return True if the adapter is RTX 20xx or 30xx series.
	 */
	bool IsRTXAndBelow40Series(IDXGIAdapter* a_adapter);

	/**
	 * @brief Configures DLSS quality mode and resolution options for a viewport.
	 * @param p_viewport The viewport handle to configure.
	 * @param width The target output width.
	 */
	void SetDLSSOptions(sl::ViewportHandle p_viewport, uint32_t width);

	/**
	 * @brief Dispatches DLSS upscaling for the current frame.
	 * @param a_upscalingTexture The input color texture to upscale.
	 * @param a_reactiveMask Reactive mask for temporal stability hints.
	 * @param a_transparencyCompositionMask Mask for transparency handling.
	 * @param a_motionVectors Per-pixel motion vectors for temporal reprojection.
	 */
	void Upscale(ID3D11Resource* a_upscalingTexture, ID3D11Resource* a_reactiveMask, ID3D11Resource* a_transparencyCompositionMask, ID3D11Resource* a_motionVectors);
	/** @brief Updates Reflex latency reduction state and performs the Reflex sleep call. */
	void UpdateReflex();

	/**
	 * @brief Emits a PCL latency marker for the current frame token.
	 *
	 * DLSS-G needs the full simulation/render/present marker sequence every frame to pace its
	 * generated frames; a missing marker leaves the interpolated frames dropped.
	 */
	void EmitPCLMarker(sl::PCLMarker a_marker);

	/**
	 * @brief Turns DLSS-G on or off for this frame and reads back its status.
	 * @param a_enabled False keeps the feature loaded but presents only rendered frames.
	 */
	void ConfigureDLSSG(bool a_enabled);

	/**
	 * @brief Tags the D3D12 inputs DLSS-G interpolates from.
	 * @param a_cmdList Command list the tags are recorded on.
	 * @param a_depth Shared depth, valid in the render-resolution sub-rectangle.
	 * @param a_mvec Shared motion vectors, same extent as depth.
	 * @param a_hudLessColor Scene without interface, display sized.
	 * @param a_uiColorAndAlpha Interface with alpha, or null when no UI buffer exists this frame.
	 * @param a_width Display width.
	 * @param a_height Display height.
	 */
	void TagDX12Resources(ID3D12GraphicsCommandList* a_cmdList,
		ID3D12Resource* a_depth, ID3D12Resource* a_mvec, ID3D12Resource* a_hudLessColor,
		ID3D12Resource* a_uiColorAndAlpha, uint32_t a_width, uint32_t a_height);

	/** @brief Frees DLSS viewport resources through the Streamline SDK. */
	void DestroyDLSSResources();

private:
	// Persistent per instance: slInit keeps the pointer for the duration of the call.
	std::wstring pluginDirAbsoluteW;
	const wchar_t* pluginPaths[1]{};

	/** @brief Requests Reflex/PCL loading and binds their entry points; shared by both instances. */
	void BindReflexAndPCL();
	bool BindFeatureFunction(sl::Feature a_feature, const char* a_functionName, void*& a_function);
};
