#include "SeamProbe.h"

#include "../../Globals.h"
#include "../../State.h"

#include <DirectXPackedVector.h>
#include <filesystem>
#include <fstream>

namespace SeamProbe
{
	namespace
	{
		// Frames between entering an eligible state and the first sample, so temporal history and
		// exposure have settled.
		constexpr uint32_t kWarmupFrames = 600;
		constexpr uint32_t kIntervalFrames = 240;
		constexpr uint32_t kSamplesPerRound = 6;
		// A request from the panel, or a render fraction that changes during its round, samples after this delay.
		constexpr uint32_t kRearmFrames = 120;

		// Strip layout, in texels relative to the strip origin: the seam line sits kLine texels
		// in. Every boundary between kSide and kStripSize - kSide is measured; the ones within
		// kGuard of the line are left out of the baseline the line is compared with.
		constexpr uint32_t kStripSize = 72;
		constexpr uint32_t kLine = 64;
		constexpr uint32_t kSide = 2;
		constexpr uint32_t kGuard = 3;
		// Length, along the line, of the segments a strip is cut into. Each yields one step per
		// boundary, so a single bright object cannot dominate the median.
		constexpr uint32_t kSegment = 8;

		// A boundary counts as a seam when its step stands this many robust deviations clear of
		// the other boundaries of the strip, and is more than rounding.
		constexpr float kSeamMinScore = 8.0f;
		constexpr float kSeamMinStep = 0.0015f;

		// Region written as an image: the corner of the render rectangle, with more of the inside
		// than of the outside.
		constexpr uint32_t kDumpInside = 448;
		constexpr uint32_t kDumpOutside = 192;
		// Samples of a round whose stages are written as images.
		constexpr uint32_t kDumpSampleA = 1;
		constexpr uint32_t kDumpSampleB = 4;

		struct LineResult
		{
			float step = 0.0f;       // step on the line, minus the median step of the other boundaries
			float score = 0.0f;      // that difference in robust deviations of the other boundaries
			float agreement = 0.0f;  // share of segments whose step on the line has the sign of its median
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
		// Only the panel sets this. Sampling reads textures back and writes images on the render thread.
		bool requested = false;
		float2 renderFraction{ 1.0f, 1.0f };
		float2 lastEligibleFraction{ 1.0f, 1.0f };
		uint32_t samplesTaken = 0;
		uint32_t nextSampleFrame = 0;
		uint32_t sampledFrame = 0;
		std::vector<StageResult> results;

		/** @brief Depth-stencil layouts the probe reads through a whole-resource copy. */
		bool IsDepthFormat(DXGI_FORMAT a_format)
		{
			return a_format == DXGI_FORMAT_R32G8X24_TYPELESS || a_format == DXGI_FORMAT_R24G8_TYPELESS || a_format == DXGI_FORMAT_R32_TYPELESS;
		}

		/** @brief Bytes per texel for the formats the probe can decode, 0 otherwise. */
		uint32_t BytesPerTexel(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R32G8X24_TYPELESS:
				return 8;
			case DXGI_FORMAT_R24G8_TYPELESS:
				return 4;
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

		/** @brief True for formats whose values are not bounded to [0, 1]. */
		bool IsFloatFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R11G11B10_FLOAT:
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_R16_FLOAT:
				return true;
			default:
				return false;
			}
		}

		/** @brief Decodes one texel to RGBA; single-channel formats repeat their value in RGB. */
		float4 DecodeTexel(const uint8_t* a_texel, DXGI_FORMAT a_format)
		{
			using namespace DirectX::PackedVector;

			switch (a_format) {
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				{
					const auto* half = reinterpret_cast<const HALF*>(a_texel);
					return { XMConvertHalfToFloat(half[0]), XMConvertHalfToFloat(half[1]), XMConvertHalfToFloat(half[2]), XMConvertHalfToFloat(half[3]) };
				}
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return { a_texel[0] / 255.0f, a_texel[1] / 255.0f, a_texel[2] / 255.0f, a_texel[3] / 255.0f };
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return { a_texel[2] / 255.0f, a_texel[1] / 255.0f, a_texel[0] / 255.0f, a_texel[3] / 255.0f };
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				{
					const uint32_t packed = *reinterpret_cast<const uint32_t*>(a_texel);
					return { (packed & 0x3FF) / 1023.0f, ((packed >> 10) & 0x3FF) / 1023.0f, ((packed >> 20) & 0x3FF) / 1023.0f, (packed >> 30) / 3.0f };
				}
			case DXGI_FORMAT_R11G11B10_FLOAT:
				{
					const XMFLOAT3PK packed(*reinterpret_cast<const uint32_t*>(a_texel));
					DirectX::XMFLOAT3 colour{};
					DirectX::XMStoreFloat3(&colour, XMLoadFloat3PK(&packed));
					return { colour.x, colour.y, colour.z, 1.0f };
				}
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_R32_FLOAT:
			case DXGI_FORMAT_R32G8X24_TYPELESS:  // depth float in the first four bytes
				{
					const float value = *reinterpret_cast<const float*>(a_texel);
					return { value, value, value, 1.0f };
				}
			case DXGI_FORMAT_R24G8_TYPELESS:  // 24-bit unorm depth in the low bits
				{
					const float value = static_cast<float>(*reinterpret_cast<const uint32_t*>(a_texel) & 0xFFFFFFu) / 16777215.0f;
					return { value, value, value, 1.0f };
				}
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_R16_FLOAT:
				{
					const float value = XMConvertHalfToFloat(*reinterpret_cast<const HALF*>(a_texel));
					return { value, value, value, 1.0f };
				}
			case DXGI_FORMAT_R16_UNORM:
				{
					const float value = *reinterpret_cast<const uint16_t*>(a_texel) / 65535.0f;
					return { value, value, value, 1.0f };
				}
			case DXGI_FORMAT_R8_TYPELESS:
			case DXGI_FORMAT_R8_UNORM:
				{
					const float value = a_texel[0] / 255.0f;
					return { value, value, value, 1.0f };
				}
			default:
				return {};
			}
		}

		/** @brief Scalar a step is measured on. */
		float Scalar(const float4& a_texel, Channel a_channel)
		{
			if (a_channel == Channel::kAlpha)
				return a_texel.w;
			return 0.2126f * a_texel.x + 0.7152f * a_texel.y + 0.0722f * a_texel.z;
		}

		float Median(std::vector<float>& a_values)
		{
			if (a_values.empty())
				return 0.0f;
			const auto middle = a_values.begin() + a_values.size() / 2;
			std::nth_element(a_values.begin(), middle, a_values.end());
			return *middle;
		}

		/** @brief Copies a box of a_source to the CPU as RGBA floats, row by row. */
		bool ReadBox(ID3D11Texture2D* a_source, const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_BOX& a_box, std::vector<float4>& a_texels)
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

			// Depth-stencil resources can only be copied whole.
			const bool depth = IsDepthFormat(a_desc.Format);
			const uint32_t regionWidth = stagingDesc.Width;
			const uint32_t regionHeight = stagingDesc.Height;
			uint32_t originX = 0, originY = 0;
			if (depth) {
				stagingDesc.Width = a_desc.Width;
				stagingDesc.Height = a_desc.Height;
				originX = a_box.left;
				originY = a_box.top;
			}

			winrt::com_ptr<ID3D11Texture2D> staging;
			if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.put())))
				return false;

			if (depth)
				context->CopyResource(staging.get(), a_source);
			else
				context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, a_source, 0, &a_box);

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return false;

			const uint32_t texelSize = BytesPerTexel(a_desc.Format);
			const auto* base = static_cast<const uint8_t*>(mapped.pData);
			a_texels.resize(static_cast<size_t>(regionWidth) * regionHeight);
			for (uint32_t y = 0; y < regionHeight; y++) {
				const auto* row = base + static_cast<size_t>(y + originY) * mapped.RowPitch;
				for (uint32_t x = 0; x < regionWidth; x++)
					a_texels[static_cast<size_t>(y) * regionWidth + x] = DecodeTexel(row + static_cast<size_t>(x + originX) * texelSize, a_desc.Format);
			}

			context->Unmap(staging.get(), 0);
			return true;
		}

		/**
		 * @brief Reduces a strip to how far the boundary on the seam line stands out from the
		 * other boundaries of the strip.
		 * @param a_acrossX True when the line is vertical (the strip is kStripSize wide).
		 */
		bool MeasureStrip(ID3D11Texture2D* a_source, const D3D11_TEXTURE2D_DESC& a_desc, const D3D11_BOX& a_box, bool a_acrossX, Channel a_channel, LineResult& a_result)
		{
			std::vector<float4> texels;
			if (!ReadBox(a_source, a_desc, a_box, texels))
				return false;

			const uint32_t width = a_box.right - a_box.left;
			const uint32_t height = a_box.bottom - a_box.top;
			const uint32_t along = a_acrossX ? height : width;
			const uint32_t segments = along / kSegment;
			if (segments < 4)
				return false;

			// value(across, along): the strip seen with the line always vertical.
			const auto value = [&](uint32_t a_across, uint32_t a_along) {
				const uint32_t x = a_acrossX ? a_across : a_along;
				const uint32_t y = a_acrossX ? a_along : a_across;
				return Scalar(texels[static_cast<size_t>(y) * width + x], a_channel);
			};

			// Mean of each segment at each position across the strip.
			std::vector<float> means(static_cast<size_t>(segments) * kStripSize);
			for (uint32_t segment = 0; segment < segments; segment++) {
				for (uint32_t across = 0; across < kStripSize; across++) {
					float sum = 0.0f;
					for (uint32_t i = 0; i < kSegment; i++)
						sum += value(across, segment * kSegment + i);
					means[static_cast<size_t>(segment) * kStripSize + across] = sum / static_cast<float>(kSegment);
				}
			}

			// Relative step of one segment across the boundary before position a_boundary.
			const auto stepOf = [&](uint32_t a_segment, uint32_t a_boundary) {
				const float* row = &means[static_cast<size_t>(a_segment) * kStripSize];
				float before = 0.0f;
				float after = 0.0f;
				for (uint32_t i = 0; i < kSide; i++) {
					before += row[a_boundary - 1 - i];
					after += row[a_boundary + i];
				}
				const float scale = std::max(std::max(std::abs(before), std::abs(after)), 1e-6f);
				return (before - after) / scale;
			};

			std::vector<float> steps(segments);
			std::vector<float> others;
			others.reserve(kStripSize);
			float lineStep = 0.0f;
			for (uint32_t boundary = kSide; boundary <= kStripSize - kSide; boundary++) {
				for (uint32_t segment = 0; segment < segments; segment++)
					steps[segment] = stepOf(segment, boundary);

				if (boundary == kLine) {
					uint32_t agreeing = 0;
					std::vector<float> sorted = steps;
					lineStep = Median(sorted);
					for (const float step : steps) {
						if (step != 0.0f && (step > 0.0f) == (lineStep > 0.0f))
							agreeing++;
					}
					a_result.agreement = static_cast<float>(agreeing) / static_cast<float>(segments);
				} else if (boundary + kGuard < kLine || boundary > kLine + kGuard) {
					others.push_back(Median(steps));
				}
			}

			std::vector<float> sorted = others;
			const float baseline = Median(sorted);
			std::vector<float> deviations;
			deviations.reserve(others.size());
			for (const float other : others)
				deviations.push_back(std::abs(other - baseline));
			// 1.4826 turns a median absolute deviation into a standard deviation for normal data.
			const float spread = std::max(1.4826f * Median(deviations), 1e-5f);

			a_result.step = lineStep - baseline;
			a_result.score = a_result.step / spread;
			a_result.count = segments;
			return true;
		}

		bool IsSeam(const LineResult& a_line)
		{
			return a_line.count != 0 && std::abs(a_line.score) >= kSeamMinScore && std::abs(a_line.step) >= kSeamMinStep;
		}

		/** @brief Folder the images are written to, next to the log; empty when unavailable. */
		std::filesystem::path DumpDirectory()
		{
			auto directory = logger::log_directory();
			if (!directory)
				return {};
			std::error_code error;
			const auto path = *directory / "SeamProbe";
			std::filesystem::create_directories(path, error);
			return error ? std::filesystem::path{} : path;
		}

		/** @brief Writes a_texels as a 24-bit BMP; float formats are tone mapped first. */
		void WriteImage(const std::filesystem::path& a_path, const std::vector<float4>& a_texels, uint32_t a_width, uint32_t a_height, bool a_floatFormat, Channel a_channel)
		{
			const uint32_t rowSize = (a_width * 3 + 3) & ~3u;
			const uint32_t dataSize = rowSize * a_height;

#pragma pack(push, 1)
			struct Header
			{
				uint16_t type = 0x4D42;
				uint32_t fileSize = 0;
				uint32_t reserved = 0;
				uint32_t dataOffset = 54;
				uint32_t infoSize = 40;
				int32_t width = 0;
				int32_t height = 0;
				uint16_t planes = 1;
				uint16_t bitsPerPixel = 24;
				uint32_t compression = 0;
				uint32_t imageSize = 0;
				int32_t pixelsPerMeterX = 2835;
				int32_t pixelsPerMeterY = 2835;
				uint32_t coloursUsed = 0;
				uint32_t coloursImportant = 0;
			};
#pragma pack(pop)
			static_assert(sizeof(Header) == 54);

			Header header;
			header.fileSize = 54 + dataSize;
			header.width = static_cast<int32_t>(a_width);
			header.height = -static_cast<int32_t>(a_height);  // top-down
			header.imageSize = dataSize;

			const auto encode = [a_floatFormat](float a_value) {
				a_value = std::isfinite(a_value) ? std::max(a_value, 0.0f) : 0.0f;
				if (a_floatFormat)
					a_value = std::pow(a_value / (1.0f + a_value), 1.0f / 2.2f);
				return static_cast<uint8_t>(std::clamp(a_value, 0.0f, 1.0f) * 255.0f + 0.5f);
			};

			std::vector<uint8_t> data(dataSize, 0);
			for (uint32_t y = 0; y < a_height; y++) {
				uint8_t* row = &data[static_cast<size_t>(y) * rowSize];
				for (uint32_t x = 0; x < a_width; x++) {
					const auto& texel = a_texels[static_cast<size_t>(y) * a_width + x];
					if (a_channel == Channel::kAlpha) {
						row[x * 3 + 0] = row[x * 3 + 1] = row[x * 3 + 2] = encode(texel.w);
					} else {
						row[x * 3 + 0] = encode(texel.z);
						row[x * 3 + 1] = encode(texel.y);
						row[x * 3 + 2] = encode(texel.x);
					}
				}
			}

			std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
			if (!file)
				return;
			file.write(reinterpret_cast<const char*>(&header), sizeof(header));
			file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
		}

		/** @brief Writes the region around the corner of the render rectangle as an image. */
		void DumpCorner(const StageResult& a_result, ID3D11Texture2D* a_source, const D3D11_TEXTURE2D_DESC& a_desc, Channel a_channel)
		{
			const auto directory = DumpDirectory();
			if (directory.empty())
				return;

			D3D11_BOX box{};
			box.left = a_result.lineX > kDumpInside ? a_result.lineX - kDumpInside : 0;
			box.top = a_result.lineY > kDumpInside ? a_result.lineY - kDumpInside : 0;
			box.right = std::min(a_result.lineX + kDumpOutside, a_desc.Width);
			box.bottom = std::min(a_result.lineY + kDumpOutside, a_desc.Height);
			box.front = 0;
			box.back = 1;

			std::vector<float4> texels;
			if (!ReadBox(a_source, a_desc, box, texels))
				return;

			std::string name = a_result.stage;
			for (auto& character : name) {
				if (!std::isalnum(static_cast<unsigned char>(character)))
					character = '_';
			}
			const auto file = directory / std::format("sample{}_{}_x{}_y{}.bmp", samplesTaken + 1, name, a_result.lineX - box.left, a_result.lineY - box.top);
			WriteImage(file, texels, box.right - box.left, box.bottom - box.top, IsFloatFormat(a_desc.Format), a_channel);
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

		// A new quality preset moves the line, so it is a new question.
		if (std::abs(a_renderFraction.x - lastEligibleFraction.x) > 0.01f || std::abs(a_renderFraction.y - lastEligibleFraction.y) > 0.01f) {
			lastEligibleFraction = a_renderFraction;
			samplesTaken = 0;
			nextSampleFrame = std::max(nextSampleFrame, frame + kRearmFrames);
		}

		if (!requested || samplesTaken >= kSamplesPerRound || frame < nextSampleFrame)
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

	void Measure(const char* a_stage, ID3D11Resource* a_texture, float2 a_fraction, bool a_dump, Channel a_channel)
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

		if (desc.SampleDesc.Count > 1 || ((desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) && !IsDepthFormat(desc.Format)) || BytesPerTexel(desc.Format) == 0) {
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

		const bool verticalOk = MeasureStrip(texture.get(), desc, verticalBox, true, a_channel, result.vertical);
		const bool horizontalOk = MeasureStrip(texture.get(), desc, horizontalBox, false, a_channel, result.horizontal);
		result.measured = verticalOk && horizontalOk;
		if (!result.measured)
			result.note = "readback failed";

		if (a_dump && result.measured && (samplesTaken + 1 == kDumpSampleA || samplesTaken + 1 == kDumpSampleB))
			DumpCorner(result, texture.get(), desc, a_channel);

		results.push_back(std::move(result));
	}

	void Measure(const char* a_stage, ID3D11Resource* a_texture, bool a_dump, Channel a_channel)
	{
		Measure(a_stage, a_texture, renderFraction, a_dump, a_channel);
	}

	void Measure(const char* a_stage, ID3D11View* a_view, bool a_dump, Channel a_channel)
	{
		if (!sampling || !a_view)
			return;

		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		Measure(a_stage, resource.get(), renderFraction, a_dump, a_channel);
	}

	void EndFrame()
	{
		if (!sampling)
			return;
		sampling = false;

		samplesTaken++;
		if (samplesTaken >= kSamplesPerRound)
			requested = false;
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

			logger::info("[SeamProbe]   {:<34} {}x{} fmt {} line ({},{}) | V step {:+.4f} score {:+.1f} agree {:.2f} | H step {:+.4f} score {:+.1f} agree {:.2f}{}",
				result.stage, result.width, result.height, static_cast<int>(result.format), result.lineX, result.lineY,
				result.vertical.step, result.vertical.score, result.vertical.agreement,
				result.horizontal.step, result.horizontal.score, result.horizontal.agreement,
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
		requested = true;
		samplesTaken = 0;
		nextSampleFrame = globals::state->frameCount + kRearmFrames;
	}
}
