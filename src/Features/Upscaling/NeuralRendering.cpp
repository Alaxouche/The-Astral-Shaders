#include "NeuralRendering.h"

#include "Buffer.h"
#include "Features/ReverseZ.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "NeuralRendering/D3D12Interop.h"
#include "NeuralRendering/Lifecycle.h"
#include "State.h"
#include "Streamline.h"
#include "Utils/D3D.h"
#include "Utils/UI.h"

#include <imgui.h>

#define I18N_KEY_PREFIX "feature.upscaling.neural_rendering."

namespace
{
	constexpr wchar_t kColorTransferShader[] = L"Data\\Shaders\\Upscaling\\NeuralRendering\\ColorTransferCS.hlsl";
	constexpr wchar_t kGuidesShader[] = L"Data\\Shaders\\Upscaling\\NeuralRendering\\GuidesCS.hlsl";

	/** @brief Radius, in texels, of the filter that splits the tone difference into two bands. */
	constexpr float kToneRadius = 1.0f;

	/** @brief Active-status line; malformed braces in a translation fall back to the English text. */
	std::string FormatActiveStatus(const std::string& runtime)
	{
		try {
			return std::vformat(T(TKEY("status_active"), "Active (runtime {})"), std::make_format_args(runtime));
		} catch (const std::format_error&) {
			return std::format("Active (runtime {})", runtime);
		}
	}

	/** @brief NGX result for a feature the system, hardware or graphics API does not support. */
	constexpr uint32_t kNgxFeatureNotSupported = 0xBAD00001u;

	/** @brief Failure line for a frame NGX rejected, naming the hardware gate when that is the cause. */
	std::string DescribeEvaluationFailure(uint32_t result)
	{
		if (result == kNgxFeatureNotSupported)
			return std::format("{} (NGX 0x{:08X})",
				T(TKEY("failure_unsupported_gpu"), "this nvngx_dlssnr.dll refuses this graphics card. NVIDIA's own build only creates the feature on the GPUs it was released for; pressing Retry will not change that"),
				result);
		return std::format("{} (NGX 0x{:08X})",
			T(TKEY("failure_evaluation"), "the NVIDIA runtime failed to process a frame. Update the GPU driver, then press Retry"),
			result);
	}

	/** @brief Which history resets a frame needs: the request, the first frame, or a frame gap. */
	bool NeedsHistoryReset(bool requested, uint32_t lastFrame, uint32_t frame)
	{
		return requested || lastFrame == UINT32_MAX || lastFrame + 1 != frame;
	}

	/** @brief Compile-once holder for one compute shader; a failure is kept until Reset(). */
	struct ComputeShader
	{
		winrt::com_ptr<ID3D11ComputeShader> shader;
		bool failed = false;

		ID3D11ComputeShader* Get(const wchar_t* path, const std::vector<std::pair<const char*, const char*>>& defines, const char* entry, const char* name)
		{
			if (!shader && !failed) {
				shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(path, defines, "cs_5_0", entry)));
				failed = !shader;
				if (shader)
					Util::SetResourceName(shader.get(), "%s", name);
			}
			return shader.get();
		}

		void Reset()
		{
			shader = nullptr;
			failed = false;
		}
	};
}

struct NeuralRendering::Impl
{
	NR::D3D12Interop interop;
	NR::Runtime runtime;

	// Shared with the NR device: the proxy the model reads, its guides, and what it returns.
	std::unique_ptr<WrappedResource> color, depth, motion, output;
	NR::FrameParameters frame;

	// D3D11 only: the untouched scene, the composited result, and the tone filter's scratch.
	std::unique_ptr<Texture2D> original, resolved, toneData;

	winrt::com_ptr<ID3D11DeviceContext1> context;
	winrt::com_ptr<ID3DDeviceContextState> isolated;
	ComputeShader prepareColor, prepareToneData, compositeColor, guides;

	struct alignas(16) ColorTransferData
	{
		uint32_t width = 0, height = 0;
		float toneLowStrength = 1.0f, toneHighStrength = 1.0f;
		float toneRadius = kToneRadius;
		uint32_t hasToneData = 0;
		float pad[2]{};
	};
	static_assert(sizeof(ColorTransferData) == 32);

	struct alignas(16) GuidesData
	{
		uint32_t width = 0, height = 0;
		uint32_t pad[2]{};
	};
	static_assert(sizeof(GuidesData) == 16);

	std::unique_ptr<ConstantBuffer> colorBuffer, guidesBuffer;

	uint32_t width = 0, height = 0, lastFrame = UINT32_MAX;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	bool ready = false, failed = false;
	float toneLowStrength = 1.0f, toneHighStrength = 1.0f;

	~Impl()
	{
		// Static destruction, where the interop wait can block on a GPU that is already gone.
		if (NR::processTerminating.load(std::memory_order_relaxed))
			return;
		try {
			interop.Drain();
		} catch (...) {
			logger::warn("[NeuralRendering] Device unavailable during resource retirement");
		}
	}

	void Initialize(bool allowUnvalidatedBuild)
	{
		interop.Initialize();
		winrt::com_ptr<ID3D11Device1> device;
		winrt::check_hresult(globals::d3d::device->QueryInterface(device.put()));
		winrt::check_hresult(globals::d3d::context->QueryInterface(context.put()));
		const auto level = globals::d3d::device->GetFeatureLevel();
		winrt::check_hresult(device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
			__uuidof(ID3D11Device), nullptr, isolated.put()));
		Util::SetResourceName(isolated.get(), "NeuralRendering::ContextState");
		colorBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ColorTransferData>(), "NeuralRendering::ColorTransfer CB");
		guidesBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GuidesData>(), "NeuralRendering::Guides CB");

		std::error_code pathError;
		auto directory = std::filesystem::absolute(std::filesystem::path(globals::features::upscaling.streamline.pluginDir), pathError);
		if (pathError)
			directory = std::filesystem::path(globals::features::upscaling.streamline.pluginDir);
		runtime.Initialize(interop.Device(), directory, allowUnvalidatedBuild);
		ready = true;
	}

	/**
	 * @brief Releases the per-frame pass resources, keeping the D3D12 device and NGX instance.
	 *        Rebuilding those costs a driver-level re-init; the textures do not.
	 */
	void ReleasePassResources()
	{
		interop.Drain();
		runtime.ResetFeatures();
		color.reset();
		depth.reset();
		motion.reset();
		output.reset();
		original.reset();
		resolved.reset();
		toneData.reset();
		frame = {};
		width = height = 0;
		format = DXGI_FORMAT_UNKNOWN;
		lastFrame = UINT32_MAX;
	}

	/** @brief True while pass resources for a render size exist and can be released. */
	bool HasPassResources() const { return width != 0; }

	void EnsureResources(uint32_t w, uint32_t h, DXGI_FORMAT colorFormat, bool force)
	{
		if (!force && width == w && height == h && format == colorFormat)
			return;
		ReleasePassResources();

		D3D11_TEXTURE2D_DESC colorDesc{};
		colorDesc.Width = w;
		colorDesc.Height = h;
		colorDesc.Format = colorFormat;
		colorDesc.MipLevels = colorDesc.ArraySize = colorDesc.SampleDesc.Count = 1;
		colorDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = colorFormat;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		original = std::make_unique<Texture2D>(colorDesc, "NeuralRendering::OriginalHDR");
		original->CreateSRV(srv);

		colorDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = colorFormat;
		uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		resolved = std::make_unique<Texture2D>(colorDesc, "NeuralRendering::ResolvedHDR");
		resolved->CreateUAV(uav);

		D3D11_TEXTURE2D_DESC toneDesc = colorDesc;
		toneDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		toneDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		srv.Format = uav.Format = toneDesc.Format;
		toneData = std::make_unique<Texture2D>(toneDesc, "NeuralRendering::ToneData");
		toneData->CreateSRV(srv);
		toneData->CreateUAV(uav);

		color = interop.CreateTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, "NeuralRendering::HDRInput");
		depth = interop.CreateTexture(w, h, DXGI_FORMAT_R32_FLOAT, "NeuralRendering::Depth");
		motion = interop.CreateTexture(w, h, DXGI_FORMAT_R16G16_FLOAT, "NeuralRendering::Motion");
		output = interop.CreateTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, "NeuralRendering::HDROutput");

		width = w;
		height = h;
		format = colorFormat;
		logger::debug("[NeuralRendering] Render-resolution pass resources {}x{}", w, h);
	}

	void Transition(ID3D12GraphicsCommandList* commands, bool enter)
	{
		ID3D12Resource* resources[]{ color->resource.get(), depth->resource.get(), motion->resource.get(), output->resource.get() };
		D3D12_RESOURCE_BARRIER barriers[4]{};
		for (uint32_t i = 0; i < 4; ++i) {
			const auto state = i == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barriers[i].Transition = { resources[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				enter ? D3D12_RESOURCE_STATE_COMMON : state, enter ? state : D3D12_RESOURCE_STATE_COMMON };
		}
		commands->ResourceBarrier(4, barriers);
	}

	/** @brief True when the two tone strengths differ, so the difference must be split in bands. */
	bool NeedsToneData() const
	{
		return toneLowStrength != toneHighStrength;
	}

	/** @brief Binds the constant buffers Common/Color.hlsli reads in a compute shader. */
	void BindSharedBuffers()
	{
		ID3D11Buffer* perFrame = *globals::game::perFrame;
		context->CSSetConstantBuffers(12, 1, &perFrame);
		ID3D11Buffer* sharedBuffers[] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
		context->CSSetConstantBuffers(5, 2, sharedBuffers);
	}

	void UpdateColorBuffer(bool withToneData)
	{
		ColorTransferData data;
		data.width = width;
		data.height = height;
		data.toneLowStrength = toneLowStrength;
		data.toneHighStrength = toneHighStrength;
		data.hasToneData = withToneData ? 1u : 0u;
		colorBuffer->Update(data);
		auto* buffer = colorBuffer->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);
	}

	void PrepareColor(ID3D11ComputeShader* shader)
	{
		context->ClearState();
		UpdateColorBuffer(false);
		BindSharedBuffers();
		ID3D11ShaderResourceView* inputs[]{ original->srv.get() };
		ID3D11UnorderedAccessView* outputs[]{ color->uav };
		context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(outputs), outputs, nullptr);
		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		context->ClearState();
	}

	void PrepareToneData(ID3D11ComputeShader* shader)
	{
		context->ClearState();
		UpdateColorBuffer(false);
		BindSharedBuffers();
		ID3D11ShaderResourceView* inputs[]{ nullptr, color->srv, output->srv };
		context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		auto* target = toneData->uav.get();
		context->CSSetUnorderedAccessViews(2, 1, &target, nullptr);
		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		context->ClearState();
	}

	void CompositeColor(ID3D11ComputeShader* shader, bool withToneData)
	{
		context->ClearState();
		UpdateColorBuffer(withToneData);
		BindSharedBuffers();
		ID3D11ShaderResourceView* inputs[]{ original->srv.get(), color->srv, output->srv, nullptr,
			withToneData ? toneData->srv.get() : nullptr };
		ID3D11UnorderedAccessView* outputs[]{ resolved->uav.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(outputs), outputs, nullptr);
		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		context->ClearState();
	}

	void EncodeGuides(ID3D11ComputeShader* shader, ID3D11ShaderResourceView* motionSRV, ID3D11ShaderResourceView* depthSRV)
	{
		context->ClearState();
		GuidesData data;
		data.width = width;
		data.height = height;
		guidesBuffer->Update(data);
		auto* buffer = guidesBuffer->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);
		ID3D11ShaderResourceView* inputs[]{ motionSRV, depthSRV };
		ID3D11UnorderedAccessView* outputs[]{ motion->uav, depth->uav };
		context->CSSetShaderResources(0, ARRAYSIZE(inputs), inputs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(outputs), outputs, nullptr);
		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		// A reset frame has no valid previous frame, so its motion must read as none.
		if (frame.reset) {
			constexpr float zero[4]{};
			context->ClearUnorderedAccessViewFloat(motion->uav, zero);
		}
		context->ClearState();
	}

	struct Shaders
	{
		ID3D11ComputeShader* prepare = nullptr;
		ID3D11ComputeShader* toneData = nullptr;
		ID3D11ComputeShader* composite = nullptr;
		ID3D11ComputeShader* guides = nullptr;
	};

	/** @brief Runs the pass on kMAIN's render-resolution region; false when NGX rejects the frame. */
	bool Draw(ID3D11Texture2D* target, ID3D11ShaderResourceView* motionSRV, ID3D11ShaderResourceView* depthSRV,
		const Shaders& shaders, bool reset, const NR::Tuning& tuning)
	{
		// The pass runs in its own device context state, so none of its bindings reach the
		// engine's state and none of the engine's reach the pass.
		struct ContextScope
		{
			ID3D11DeviceContext1* context;
			winrt::com_ptr<ID3DDeviceContextState> previous;
			ContextScope(ID3D11DeviceContext1* ctx, ID3DDeviceContextState* isolated) :
				context(ctx)
			{
				context->SwapDeviceContextState(isolated, previous.put());
				context->ClearState();
			}
			~ContextScope()
			{
				context->ClearState();
				context->SwapDeviceContextState(previous.get(), nullptr);
			}
		} scope(context.get(), isolated.get());

		globals::state->BeginPerfEvent("Neural Rendering");

		const D3D11_BOX box{ 0, 0, 0, width, height, 1 };
		context->CopySubresourceRegion(original->resource.get(), 0, 0, 0, 0, target, 0, &box);

		frame.reset = reset;
		PrepareColor(shaders.prepare);
		EncodeGuides(shaders.guides, motionSRV, depthSRV);

		// Resetting persistent NR history must not overlap its prior GPU evaluation.
		if (frame.reset)
			interop.Drain();

		bool success = true;
		{
			auto* commands = interop.Begin();
			Transition(commands, true);
			NR::GuideParameters guideParameters;
			guideParameters.depth = { 0, 0, width, height };
			guideParameters.motion = { 0, 0, width, height };
			// The engine's motion vectors are normalized UV displacement; NR reads input pixels.
			guideParameters.motionScaleX = static_cast<float>(width);
			guideParameters.motionScaleY = static_cast<float>(height);
			success = runtime.Evaluate(commands, 0, color->resource.get(), depth->resource.get(),
				motion->resource.get(), output->resource.get(), width, height, guideParameters, frame, tuning);
			Transition(commands, false);
			interop.End();
		}

		if (success) {
			const bool withToneData = NeedsToneData();
			if (withToneData)
				PrepareToneData(shaders.toneData);
			CompositeColor(shaders.composite, withToneData);
			context->CopySubresourceRegion(target, 0, 0, 0, 0, resolved->resource.get(), 0, &box);
		}

		globals::state->EndPerfEvent();
		return success;
	}
};

NeuralRendering::NeuralRendering() :
	impl(std::make_unique<Impl>()) {}
NeuralRendering::~NeuralRendering() = default;

void NeuralRendering::SetupResources() { retryRequested = recreate = resetHistory = true; }
void NeuralRendering::ResetHistory() { resetHistory = true; }
void NeuralRendering::ClearShaderCache() { retryRequested = clearShaders = resetHistory = true; }

void NeuralRendering::PublishStatus(Status::State state, std::string text)
{
	Status next;
	next.state = state;
	next.text = std::move(text);
	next.runtimeVersion = impl->runtime.Version();
	next.width = impl->width;
	next.height = impl->height;
	std::scoped_lock lock(statusMutex);
	status = std::move(next);
	publishedState.store(state, std::memory_order_relaxed);
}

void NeuralRendering::PublishFailure(const std::string& detail)
{
	PublishStatus(Status::State::kFailed, std::format("{} {}", T(TKEY("status_failed"), "Neural Rendering stopped:"), detail));
}

NeuralRendering::Status NeuralRendering::GetStatus() const
{
	std::scoped_lock lock(statusMutex);
	auto snapshot = status;
	if (snapshot.text.empty())
		snapshot.text = T(TKEY("status_off"), "Off");
	snapshot.appliedFrames = appliedFrames.load(std::memory_order_relaxed);
	snapshot.ngxResult = lastNgxResult.load(std::memory_order_relaxed);
	return snapshot;
}

void NeuralRendering::DrawSettings(bool& enabled, bool& allowUnvalidatedBuild, NR::Tuning& tuning)
{
	ImGui::PushID("NeuralRendering");

	if (ImGui::Checkbox(T(TKEY("enable"), "Enable Neural Rendering"), &enabled))
		retryRequested = resetHistory = true;
	ImGui::TextWrapped("%s", T(TKEY("description"),
								 "DLSS 5 neural rendering: one pass at render resolution, composed back into the scene before DLSS/FSR and frame generation. Requires an NVIDIA RTX GPU and the NVIDIA runtime nvngx_dlssnr.dll version 310.8, which is not included: place your own copy in Data/Shaders/Upscaling/Streamline."));

	if (ImGui::Checkbox(T(TKEY("allow_unvalidated"), "Allow Unvalidated Runtime Build"), &allowUnvalidatedBuild))
		RequestRetry();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("allow_unvalidated_tooltip"),
							  "Only two builds of the runtime had their output checked in game. Any other 310.8 build, such as a model converted for an older GPU, is refused unless this is ticked. With it ticked the colours of such a build are taken on trust: untick it if the image turns blue or red."));
	}

	int style = static_cast<int>(std::min(tuning.style, NR::Tuning::kMaxStyle));
	const std::array<const char*, NR::Tuning::kMaxStyle + 1> styleLabels{
		T(TKEY("style_0"), "Style 0"),
		T(TKEY("style_1"), "Style 1"),
		T(TKEY("style_2"), "Style 2"),
	};
	bool changed = ImGui::Combo(T(TKEY("style"), "Style"), &style, styleLabels.data(), static_cast<int>(styleLabels.size()));
	bool recreateTuning = changed;
	if (changed)
		tuning.style = static_cast<uint32_t>(style);

	changed |= ImGui::SliderFloat(T(TKEY("intensity"), "Intensity"), &tuning.intensity, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat(T(TKEY("local_tone"), "Local Tone Strength"), &tuning.localToneStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat(T(TKEY("local_structure"), "Local Structure Strength"), &tuning.localStructureStrength, NR::Tuning::kMinStrength, NR::Tuning::kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();
	changed |= ImGui::SliderFloat(T(TKEY("skin_structure"), "Skin Structure Strength"), &tuning.skinStructureStrength, NR::Tuning::kAutomaticSkinStructure, NR::Tuning::kMaxStrength,
		tuning.skinStructureStrength == NR::Tuning::kAutomaticSkinStructure ? T(TKEY("skin_auto"), "Auto") : "%.2f", ImGuiSliderFlags_AlwaysClamp);
	recreateTuning |= ImGui::IsItemDeactivatedAfterEdit();

	const bool autoMaskChanged = ImGui::Checkbox(T(TKEY("use_auto_mask"), "Use Auto Mask"), &tuning.useAutoMask);
	changed |= autoMaskChanged;
	recreateTuning |= autoMaskChanged;

	if (ImGui::Button(T(TKEY("restore_defaults"), "Restore NR Defaults"))) {
		tuning = {};
		changed = recreateTuning = true;
	}
	if (changed)
		tuning.Sanitize();
	// The runtime latches the appearance controls when the feature is created.
	if (recreateTuning)
		recreate = resetHistory = true;

	ImGui::SameLine();
	if (ImGui::Button(T(TKEY("reset_history"), "Reset NR History")))
		resetHistory = true;
	if (enabled) {
		ImGui::SameLine();
		if (ImGui::Button(T(TKEY("retry"), "Retry NR")))
			RequestRetry();
	}

	const auto current = GetStatus();
	// The pass is only reached while the game world renders. In the main menu nothing has
	// published a status yet, and "Off" next to a ticked box reads as a fault.
	if (enabled && current.state == Status::State::kOff)
		ImGui::TextWrapped("%s", T(TKEY("status_waiting"), "Waiting for the game world: the pass starts once a save is loaded."));
	else
		ImGui::TextWrapped("%s", current.text.c_str());
	if (current.state == Status::State::kActive)
		ImGui::TextDisabled("%ux%u, %u frames", current.width, current.height, current.appliedFrames);

	ImGui::PopID();
}

void NeuralRendering::DrawBeforeUpscaling(bool enabled, bool allowUnvalidatedBuild, const NR::Tuning& tuning, float2 renderSize, bool worldRendered)
{
	const uint32_t frameCount = globals::state->frameCount;
	const auto action = NR::DecideFrame({ enabled, worldRendered, impl->failed,
		retryRequested.load(), impl->ready, impl->lastFrame, frameCount });

	if (action == NR::FrameAction::ReleasePassResources) {
		// A latched failure may be a wedged queue, so its resources wait for Retry's draining teardown.
		if (impl->HasPassResources() && !impl->failed) {
			std::string failure;
			try {
				impl->ReleasePassResources();
			} catch (const winrt::hresult_error& error) {
				failure = winrt::to_string(error.message());
			} catch (const std::exception& error) {
				failure = error.what();
			}
			if (!failure.empty()) {
				LatchFailure();
				PublishFailure(failure);
				logger::error("[NeuralRendering] {}", failure);
				retryRequested = resetHistory = true;
				return;
			}
		}
		if (!impl->failed && publishedState.load(std::memory_order_relaxed) != Status::State::kOff) {
			logger::debug("[NeuralRendering] Disabled");
			PublishStatus(Status::State::kOff, T(TKEY("status_off"), "Off"));
		}
		retryRequested = resetHistory = true;
		return;
	}

	if (action == NR::FrameAction::SkipNoWorld) {
		if (publishedState.load(std::memory_order_relaxed) == Status::State::kOff)
			PublishStatus(Status::State::kStarting, T(TKEY("status_starting"), "Starting..."));
		resetHistory = true;
		return;
	}

	try {
		retryRequested = false;
		if (action == NR::FrameAction::RebuildThenRun) {
			// Retire both APIs before releasing a failed runtime and its shared resources.
			impl->interop.Drain();
			impl = std::make_unique<Impl>();
			resetHistory = true;
		}
		auto& work = *impl;
		if (action == NR::FrameAction::SkipLatched || action == NR::FrameAction::SkipDuplicate)
			return;

		// A rebuilt runtime starts uninitialized, so both actions end in Initialize().
		if (action == NR::FrameAction::InitializeThenRun || action == NR::FrameAction::RebuildThenRun) {
			work.Initialize(allowUnvalidatedBuild);
			const auto luid = work.interop.AdapterLuid();
			logger::debug("[NeuralRendering] D3D12 device on renderer adapter LUID {:08X}:{:08X}", static_cast<uint32_t>(luid.HighPart), static_cast<uint32_t>(luid.LowPart));
		}
		if (clearShaders.exchange(false)) {
			work.prepareColor.Reset();
			work.prepareToneData.Reset();
			work.compositeColor.Reset();
			work.guides.Reset();
		}

		auto* renderer = globals::game::renderer;
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		auto& depthStencil = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		if (!main.texture || !motionVector.SRV || !depthStencil.depthSRV)
			throw std::runtime_error("the scene colour, motion or depth target is missing");

		D3D11_TEXTURE2D_DESC desc{};
		main.texture->GetDesc(&desc);
		const auto w = static_cast<uint32_t>(renderSize.x);
		const auto h = static_cast<uint32_t>(renderSize.y);
		if (!NR::IsSupportedOutput(desc, w, h, 1))
			throw std::runtime_error(std::format("unsupported scene target: {}x{}, DXGI format {}, array {}, samples {}, render size {}x{}",
				desc.Width, desc.Height, static_cast<uint32_t>(desc.Format), desc.ArraySize, desc.SampleDesc.Count, w, h));

		work.EnsureResources(w, h, desc.Format, recreate.exchange(false));

		std::vector<std::pair<const char*, const char*>> guideDefines;
		if (globals::features::reverseZ.IsActive())
			guideDefines.push_back({ "REVERSE_Z", nullptr });

		Impl::Shaders shaders;
		shaders.prepare = work.prepareColor.Get(kColorTransferShader, {}, "Prepare", "NeuralRendering::PrepareColor CS");
		shaders.toneData = work.prepareToneData.Get(kColorTransferShader, {}, "PrepareToneData", "NeuralRendering::PrepareToneData CS");
		shaders.composite = work.compositeColor.Get(kColorTransferShader, {}, "Composite", "NeuralRendering::Composite CS");
		shaders.guides = work.guides.Get(kGuidesShader, guideDefines, "main", "NeuralRendering::Guides CS");
		if (!shaders.prepare || !shaders.toneData || !shaders.composite || !shaders.guides)
			throw std::runtime_error("a Neural Rendering shader failed to compile; see the log, then clear the shader cache");

		auto boundedTuning = tuning;
		boundedTuning.Sanitize();
		work.toneLowStrength = boundedTuning.localToneStrength;
		work.toneHighStrength = boundedTuning.localStructureStrength;

		const bool reset = NeedsHistoryReset(resetHistory.exchange(false), work.lastFrame, frameCount);
		const bool applied = work.Draw(main.texture, motionVector.SRV, depthStencil.depthSRV, shaders, reset, boundedTuning);
		lastNgxResult.store(work.frame.result, std::memory_order_relaxed);
		if (!applied)
			throw std::runtime_error(DescribeEvaluationFailure(work.frame.result));

		// The pass wrote kMAIN behind the engine's back.
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

		appliedFrames.fetch_add(1, std::memory_order_relaxed);
		if (publishedState.load(std::memory_order_relaxed) != Status::State::kActive) {
			const auto runtime = work.runtime.Version();
			PublishStatus(Status::State::kActive, FormatActiveStatus(runtime));
			logger::info("[NeuralRendering] active: runtime {}, {}x{}", runtime, w, h);
		}
		work.lastFrame = frameCount;
	} catch (const winrt::hresult_error& error) {
		LatchFailure();
		PublishFailure(winrt::to_string(error.message()));
		logger::error("[NeuralRendering] D3D initialization/dispatch failed: 0x{:08X}", static_cast<uint32_t>(error.code().value));
	} catch (const std::exception& error) {
		LatchFailure();
		PublishFailure(error.what());
		logger::error("[NeuralRendering] {}", error.what());
	} catch (...) {
		LatchFailure();
		PublishFailure("unknown error");
		logger::error("[NeuralRendering] unknown error");
	}
}

void NeuralRendering::LatchFailure()
{
	if (NR::OnFailure(impl->interop.DeviceRemoved()) == NR::FailureAction::TeardownThenLatch) {
		// A removed device cannot be reused: the replacement starts latched, so only Retry rebuilds it.
		impl = std::make_unique<Impl>();
	}
	impl->failed = true;
}

#undef I18N_KEY_PREFIX
