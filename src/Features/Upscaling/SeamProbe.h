#pragma once

#include <d3d11.h>

/**
 * @brief Diagnostic that finds the pass responsible for a discontinuity along the edge of the
 * dynamic-resolution render rectangle.
 *
 * With an upscaler active the scene is rendered into the top-left sub-rectangle of full-size
 * targets and stretched to the display by the upscaler. Any later pass that still works on that
 * sub-rectangle, or samples a texture that only holds data there, leaves a straight seam at
 * (ratio * width, ratio * height) in the final image. The probe reads a thin strip of texels
 * across that line at several stages of one frame and logs the step it measures at each, so the
 * first stage that shows a step names the culprit. It samples a handful of frames and then stays
 * idle; the cost outside those frames is one branch per call.
 */
namespace SeamProbe
{
	/**
	 * @brief Starts a frame. Flushes the previous sampled frame to the log and decides whether
	 * this one is sampled.
	 * @param a_renderFraction Render size over display size, per axis.
	 * @param a_eligible False when no seam can exist (no upscaler, not in the world).
	 */
	void BeginFrame(float2 a_renderFraction, bool a_eligible);

	/** @brief True while the current frame is being sampled. */
	bool IsSampling();

	/**
	 * @brief Measures the step across the line at a_fraction of the texture size.
	 * @param a_stage Label written to the log.
	 * @param a_texture Any non-multisampled colour texture; depth-stencil targets are skipped.
	 * @param a_fraction Position of the line as a fraction of the texture size.
	 */
	void Measure(const char* a_stage, ID3D11Resource* a_texture, float2 a_fraction);

	/** @brief Measures at the render fraction latched by BeginFrame. */
	void Measure(const char* a_stage, ID3D11Resource* a_texture);

	/** @brief Measures the resource behind a view at the latched render fraction. */
	void Measure(const char* a_stage, ID3D11View* a_view);

	/** @brief Writes the sampled frame to the log. Safe to call when nothing was sampled. */
	void EndFrame();

	/** @brief Schedules another round of samples (debug UI). */
	void Rearm();
}
