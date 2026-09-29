#pragma once

#include "NeuralRendering/Runtime.h"
#include "NeuralRendering/Tuning.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

/**
 * @brief Applies one render-resolution NGX Neural Rendering (DLSS 5, NGX Feature 18) pass to
 *        kMAIN before upscaling and frame-generation capture.
 *
 * Ported from Open Shaders (#723). The port keeps its runtime, interop and colour contract and
 * leaves out its VR path and its developer diagnostics suite. The NVIDIA runtime
 * (nvngx_dlssnr.dll) is never shipped with the mod: the player supplies it.
 */
struct NeuralRendering
{
	/** @brief What the pass is doing, as the settings panel reports it. */
	struct Status
	{
		enum class State : uint8_t
		{
			kOff,       ///< Switched off.
			kStarting,  ///< Enabled; the runtime initializes on the first world frame.
			kActive,    ///< Running; text names the runtime version.
			kFailed     ///< Latched; text is the failure reason.
		};
		State state = State::kOff;
		/** @brief Plain-language line for the settings panel; empty until the first publish. */
		std::string text;
		/** @brief Accepted nvngx_dlssnr.dll version; empty until the runtime initializes. */
		std::string runtimeVersion;
		/** @brief Render width the pass resources were last created for. */
		uint32_t width = 0;
		/** @brief Render height the pass resources were last created for. */
		uint32_t height = 0;
		/** @brief Frames the pass has applied since startup. */
		uint32_t appliedFrames = 0;
		/** @brief NGX result code of the last evaluated frame. */
		uint32_t ngxResult = 0;
	};

	NeuralRendering();
	~NeuralRendering();

	/** @brief Schedules recreation on the rendering thread. */
	void SetupResources();
	/** @brief Invalidates the temporal history on loading or setting changes. */
	void ResetHistory();
	/** @brief Drops the cached shaders so they recompile on the next frame. */
	void ClearShaderCache();
	/**
	 * @brief Draws the tuning controls, retry button and runtime status.
	 * @param enabled The persisted on/off switch.
	 * @param allowUnvalidatedBuild The persisted switch that accepts runtime builds outside the validated list.
	 * @param tuning The persisted appearance controls.
	 */
	void DrawSettings(bool& enabled, bool& allowUnvalidatedBuild, NR::Tuning& tuning);
	/**
	 * @brief Replaces the render-resolution region of kMAIN before upscaling.
	 * @param enabled The user has the pass switched on.
	 * @param allowUnvalidatedBuild Accept a runtime build outside the validated list.
	 * @param tuning Appearance controls for this frame.
	 * @param renderSize Size of the region the scene was rendered into.
	 * @param worldRendered The engine drew a world this frame.
	 */
	void DrawBeforeUpscaling(bool enabled, bool allowUnvalidatedBuild, const NR::Tuning& tuning, float2 renderSize, bool worldRendered);

	/** @brief Snapshot of the current status, safe from any thread. */
	Status GetStatus() const;
	/** @brief Queues one retry for the next world frame. */
	void RequestRetry() { retryRequested = resetHistory = true; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	std::atomic_bool resetHistory = true, recreate = false, clearShaders = false, retryRequested = false;
	Status status;
	/** @brief Mirror of status.state so the per-frame paths can skip a publish without the lock. */
	std::atomic<Status::State> publishedState{ Status::State::kOff };
	std::atomic<uint32_t> appliedFrames{ 0 };
	std::atomic<uint32_t> lastNgxResult{ 0 };
	mutable std::mutex statusMutex;

	/** @brief Publishes a status line plus the run state the panel reports. */
	void PublishStatus(Status::State state, std::string text);
	/** @brief Publishes a failure with the prefix the panel shows for a stopped pass. */
	void PublishFailure(const std::string& detail);
	/** @brief Latches a failure, tearing the runtime down first when the device was removed. */
	void LatchFailure();
	/** @brief Last member: it must be destroyed before impl's NGX teardown at process exit. */
	NR::TerminationSentinel terminationSentinel;
};
