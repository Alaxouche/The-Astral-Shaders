#include "SeamProbe.h"

#include "../../Globals.h"
#include "../../State.h"

#include <DirectXPackedVector.h>

namespace SeamProbe
{
	namespace
	{
		// Frames between entering an eligible state and the first sample, so temporal history and
		// exposure have settled.
		constexpr uint32_t kWarmupFrames = 600;
		constexpr uint32_t kIntervalFrames = 240;
		constexpr uint32_t kSamplesPerRound = 6;

		// Strip layout, in texels relative to the strip origin: the seam line sits kLine texels
		// in, and a control line where no seam can exist sits kControl texels in. Both compare the
		// kSide texels on either side of their line.
		constexpr uint32_t kStripSize = 72;
		constexpr uint32_t kLine = 64;
		constexpr uint32_t kControl = 8;
		constexpr uint32_t kSide = 4;

		// A step counts as a seam when it is consistent along the line and stands clear of what
		// ordinary image content produces on the control line.
		constexpr float kSeamMinStep = 0.004f;
		constexpr float kSeamMinAgreement = 0.75f;
		constexpr float kSeamControlFactor = 4.0f;

		struct LineResult
		{
			float step = 0.0f;       // median relative step across the seam line, first side minus second
			float agreement = 0.0f;  // share of segments whose step has the sign of the median
			float control = 0.0f;    // same median, on the control line
			uint32_t count = 0;
		};

		struct StageResult
		{
			std::string stage;
			std::string note;
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t lineX = 0;
			uint32_t lineY = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			LineResult vertical;
			LineResult horizontal;
			bool measured = false;
		};

		bool sampling = false;
		bool armed = false;
		float2 renderFraction{ 1.0f, 1.0f };
		uint32_t samplesTaken = 0;
		uint32_t nextSampleFrame = 0;
		uint32_t sampledFrame = 0;
		std::vector<StageResult> results;

		/** @brief Bytes per texel for the formats the probe can decode, 0 otherwise. */
		uint32_t BytesPerTexel(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				return 8;
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R11G11B10_FLOAT:
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_R32_FLOAT:
				return 4;
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_R16_UNORM:
				return 2;
			case DXGI_FORMAT_R8_TYPELESS:
			case DXGI_FORMAT_R8_UNORM:
				return 1;
			default:
				return 0;
			}
		}

		/** @brief Scalar the step is measured on: luminance for colour, the value itself otherwise. */
		float DecodeTexel(const uint8_t* a_texel, DXGI_FORMAT a_format)
		{
			using namespace DirectX::PackedVector;

			const auto luminance = [](float r, float g, float b) {
				return 0.2126f * r + 0.7152f * g + 0.0722f * b;
			};

			switch (a_format) {
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				{
					const auto* half = reinterpret_cast<const HALF*>(a_texel);
					return luminance(XMConvertHalfToFloat(half[0]), XMConvertHalfToFloat(half[1]), XMConvertHalfToFloat(half[2]));
				}
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return luminance(a_texel[0] / 255.0f, a_texel[1] / 255.0f, a_texel[2] / 255.0f);
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return luminance(a_texel[2] / 255.0f, a_texel[1] / 255.0f, a_texel[0] / 255.0f);
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				{
					const uint32_t packed = *reinterpret_cast<const uint32_t*>(a_texel);
					return luminance((packed & 0x3FF) / 1023.0f, ((packed >> 10) & 0x3FF) / 1023.0f, ((packed >> 20) & 0x3FF) / 1023.0f);
				}
			case DXGI_FORMAT_R11G11B10_FLOAT:
				{
					const XMFLOAT3PK packed(*reinterpret_cast<const uint32_t*>(a_texel));
					DirectX::XMFLOAT3 colour{};
					DirectX::XMStoreFloat3(&colour, XMLoadFloat3PK(&packed));
					return luminance(colour.x, colour.y, colour.z);
				}
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_R32_FLOAT:
				return *reinterpret_cast<const float*>(a_texel);
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_R16_FLOAT:
				return XMConvertHalfToFloat(*reinterpret_cast<const HALF*>(a_texel));
			case DXGI_FORMAT_R16_UNORM:
				return *reinterpret_cast<const uint16_t*>(a_texel) / 65535.0f;
			case DXGI_FORMAT_R8_TYPELESS:
			case DXGI_FORMAT_R8_UNORM:
				return a_texel[0] / 255.0f;
			default:
				return 0.0f;
			}
		}

		float Median(std::vector<float>& a_values)
		{
			if (a_values.empty())
				return 0.0f;
			const auto middle = a_values.begin() + a_values.size() / 2;
			std::nth_element(a_values.begin(), middle, a_values.end());
			return *middle;
		}

		/**
		 * @brief Copies a box of a_source into a staging texture and reduces it to the step across
		 * the seam line and across the control line.
		 * @param a_acrossX True when the line is vertical (the strip is kStripSize wide).
		 */
		bool MeasureStrip(ID3D11Texture2D* a_source, const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_BOX& a_box, bool a_acrossX, LineResult& a_result)
		{
			auto device = globals::d3d::device;
			auto context = globals::d3d::context;

			D3D11_TEXTURE2D_DESC stagingDesc{};
			stagingDesc.Width = a_box.right - a_box.left;
			stagingDesc.Height = a_box.bottom - a_box.top;
			stagingDesc.MipLevels = 1;
			stagingDesc.ArraySize = 1;
			stagingDesc.Format = a_desc.Format;
			stagingDesc.SampleDesc = { 1, 0 };
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

			winrt::com_ptr<ID3D11Texture2D> staging;
			if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.put())))
				return false;

			context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, a_source, 0, &a_box);

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return false;

			const uint32_t texelSize = BytesPerTexel(a_desc.Format);
			const auto* base = static_cast<const uint8_t*>(mapped.pData);
			const auto texel = [&](uint32_t x, uint32_t y) {
				return DecodeTexel(base + static_cast<size_t>(y) * mapped.RowPitch + static_cast<size_t>(x) * texelSize, a_desc.Format);
			};

			// Along the line the strip is cut into short segments; each yields one relative step,
			// so a single bright object cannot dominate the median.
			constexpr uint32_t kSegment = 8;
			const uint32_t along = a_acrossX ? stagingDesc.Height : stagingDesc.Width;

			std::vector<float> seamSteps;
			std::vector<float> controlSteps;
			seamSteps.reserve(along / kSegment + 1);
			controlSteps.reserve(along / kSegment + 1);

			const auto stepAt = [&](uint32_t a_line, uint32_t a_start) {
				float first = 0.0f;
				float second = 0.0f;
				for (uint32_t s = 0; s < kSegment; s++) {
					for (uint32_t k = 0; k < kSide; k++) {
						const uint32_t before = a_line - 1 - k;
						const uint32_t after = a_line + k;
						if (a_acrossX) {
							first += texel(before, a_start + s);
							second += texel(after, a_start + s);
						} else {
							first += texel(a_start + s, before);
							second += texel(a_start + s, after);
						}
					}
				}
				const float scale = std::max(std::max(std::abs(first), std::abs(second)), 1e-6f);
				return (first - second) / scale;
			};

			for (uint32_t start = 0; start + kSegment <= along; start += kSegment) {
				seamSteps.push_back(stepAt(kLine, start));
				controlSteps.push_back(stepAt(kControl, start));
			}

			context->Unmap(staging.get(), 0);

			a_result.count = static_cast<uint32_t>(seamSteps.size());
			if (seamSteps.empty())
				return false;

			std::vector<float> sorted = seamSteps;
			a_result.step = Median(sorted);
			a_result.control = Median(controlSteps);

			uint32_t agreeing = 0;
			for (const float step : seamSteps) {
				if ((step > 0.0f) == (a_result.step > 0.0f) && step != 0.0f)
					agreeing++;
			}
			a_result.agreement = static_cast<float>(agreeing) / static_cast<float>(seamSteps.size());
			return true;
		}

		bool IsSeam(const LineResult& a_line)
		{
			return a_line.count != 0 &&
			       std::abs(a_line.step) >= kSeamMinStep &&
			       a_line.agreement >= kSeamMinAgreement &&
			       std::abs(a_line.step) >= kSeamControlFactor * std::abs(a_line.control);
		}
	}

	void BeginFrame(float2 a_renderFraction, bool a_eligible)
	{
		EndFrame();

		const uint32_t frame = globals::state->frameCount;

		if (!a_eligible || a_renderFraction.x >= 0.99f || a_renderFraction.y >= 0.99f) {
			armed = false;
			return;
		}

		if (!armed) {
			armed = true;
			nextSampleFrame = frame + kWarmupFrames;
		}

		if (samplesTaken >= kSamplesPerRound || frame < nextSampleFrame)
			return;

		sampling = true;
		sampledFrame = frame;
		renderFraction = a_renderFraction;
		results.clear();
	}

	bool IsSampling()
	{
		return sampling;
	}

	void Measure(const char* a_stage, ID3D11Resource* a_texture, float2 a_fraction)
	{
		if (!sampling || !a_texture)
			return;

		StageResult result;
		result.stage = a_stage;

		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(a_texture->QueryInterface(texture.put())) || !texture) {
			result.note = "not a 2D texture";
			results.push_back(std::move(result));
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		result.width = desc.Width;
		result.height = desc.Height;
		result.format = desc.Format;

		if (desc.SampleDesc.Count > 1 || (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) || BytesPerTexel(desc.Format) == 0) {
			result.note = "format not probed";
			results.push_back(std::move(result));
			return;
		}

		const uint32_t lineX = static_cast<uint32_t>(static_cast<float>(desc.Width) * a_fraction.x);
		const uint32_t lineY = static_cast<uint32_t>(static_cast<float>(desc.Height) * a_fraction.y);
		result.lineX = lineX;
		result.lineY = lineY;

		const uint32_t tail = kStripSize - kLine;
		if (lineX < kLine || lineY < kLine || lineX + tail > desc.Width || lineY + tail > desc.Height) {
			result.note = "line too close to the edge";
			results.push_back(std::move(result));
			return;
		}

		// Both strips stay inside the rectangle along the line, so they only ever compare
		// texels that the render rectangle and its surroundings actually disagree on.
		const D3D11_BOX verticalBox{ lineX - kLine, 0, 0, lineX + tail, lineY, 1 };
		const D3D11_BOX horizontalBox{ 0, lineY - kLine, 0, lineX, lineY + tail, 1 };

		const bool verticalOk = MeasureStrip(texture.get(), desc, verticalBox, true, result.vertical);
		const bool horizontalOk = MeasureStrip(texture.get(), desc, horizontalBox, false, result.horizontal);
		result.measured = verticalOk && horizontalOk;
		if (!result.measured)
			result.note = "readback failed";

		results.push_back(std::move(result));
	}

	void Measure(const char* a_stage, ID3D11Resource* a_texture)
	{
		Measure(a_stage, a_texture, renderFraction);
	}

	void Measure(const char* a_stage, ID3D11View* a_view)
	{
		if (!sampling || !a_view)
			return;

		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		Measure(a_stage, resource.get(), renderFraction);
	}

	void EndFrame()
	{
		if (!sampling)
			return;
		sampling = false;

		samplesTaken++;
		nextSampleFrame = sampledFrame + kIntervalFrames;

		logger::info("[SeamProbe] sample {}/{} frame {} render fraction {:.4f}x{:.4f}",
			samplesTaken, kSamplesPerRound, sampledFrame, renderFraction.x, renderFraction.y);

		const char* firstSeam = nullptr;
		for (const auto& result : results) {
			if (!result.measured) {
				logger::info("[SeamProbe]   {:<34} {}x{} fmt {}: {}", result.stage, result.width, result.height, static_cast<int>(result.format), result.note);
				continue;
			}

			const bool seam = IsSeam(result.vertical) || IsSeam(result.horizontal);
			if (seam && !firstSeam)
				firstSeam = result.stage.c_str();

			logger::info("[SeamProbe]   {:<34} {}x{} fmt {} line ({},{}) | V step {:+.4f} agree {:.2f} ctrl {:+.4f} | H step {:+.4f} agree {:.2f} ctrl {:+.4f}{}",
				result.stage, result.width, result.height, static_cast<int>(result.format), result.lineX, result.lineY,
				result.vertical.step, result.vertical.agreement, result.vertical.control,
				result.horizontal.step, result.horizontal.agreement, result.horizontal.control,
				seam ? "  <-- SEAM" : "");
		}

		if (firstSeam)
			logger::info("[SeamProbe]   first stage with a seam: {}", firstSeam);
		else
			logger::info("[SeamProbe]   no seam at any stage");

		results.clear();
	}

	void Rearm()
	{
		samplesTaken = 0;
		nextSampleFrame = globals::state->frameCount + 60;
	}
}
