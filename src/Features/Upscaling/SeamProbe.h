#pragma once

#include <d3d11.h>

/**
 * @brief Diagnostic that finds the pass responsible for a discontinuity along the edge of the
 * dynamic-resolution render rectangle.
 *
 * With an upscaler active the scene is rendered into the top-left sub-rectangle of full-size
 * targets and stretched to the display by the upscaler. Any later pass that still works on that
 * sub-rectangle, or samples a texture that only holds data there, leaves a straight seam at
 * (ratio * width, ratio * height) in the final image.
 *
 * The probe reads a thin strip of texels across that line at several stages of one frame. For
 * every texel boundary of the strip it measures the step between the two sides; image content
 * gives every boundary a similar, small step, while a seam gives the one boundary on the line a
 * step the others do not have. The score is how far that boundary stands out from the rest. The
 * first stage that shows one names the culprit. Selected stages are also written as images next
 * to the log, so the defect can be looked at rather than inferred.
 *
 * It stays idle until Rearm() requests a round, samples a handful of frames, then goes idle again;
 * the cost outside those frames is one branch per call.
 */
namespace SeamProbe
{
	/** @brief What a measurement reads from each texel. */
	enum class Channel : uint8_t
	{
		kLuminance,  ///< Luminance of the colour, or the value of a single-channel texture.
		kAlpha       ///< Alpha, for buffers that are composited over the scene.
	};

	/**
	 * @brief Starts a frame. Flushes the previous sampled frame to the log and decides whether
	 * this one is sampled.
	 * @param a_renderFraction Render size over display size, per axis.
	 * @param a_eligible False when no seam can exist (no upscaler, no world rendered).
	 */
	void BeginFrame(float2 a_renderFraction, bool a_eligible);

	/** @brief True while the current frame is being sampled. */
	bool IsSampling();

	/**
	 * @brief Measures the step across the line at a_fraction of the texture size.
	 * @param a_stage Label written to the log and used for the image file name.
	 * @param a_texture Any non-multisampled colour texture; depth-stencil targets are skipped.
	 * @param a_fraction Position of the line as a fraction of the texture size.
	 * @param a_dump Also write the region around the corner of the rectangle as an image.
	 * @param a_channel What to read from each texel.
	 */
	void Measure(const char* a_stage, ID3D11Resource* a_texture, float2 a_fraction, bool a_dump = false, Channel a_channel = Channel::kLuminance);

	/** @brief Measures at the render fraction latched by BeginFrame. */
	void Measure(const char* a_stage, ID3D11Resource* a_texture, bool a_dump = false, Channel a_channel = Channel::kLuminance);

	/** @brief Measures the resource behind a view at the latched render fraction. */
	void Measure(const char* a_stage, ID3D11View* a_view, bool a_dump = false, Channel a_channel = Channel::kLuminance);

	/** @brief Writes the sampled frame to the log. Safe to call when nothing was sampled. */
	void EndFrame();

	/** @brief Requests a round of samples (settings panel). Nothing is sampled without it. */
	void Rearm();
}
