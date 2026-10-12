#ifndef PHYSICAL_SKY_CLOUD_BLUR_HLSLI
#define PHYSICAL_SKY_CLOUD_BLUR_HLSLI

Texture2D<float> TexCloudResultTr : register(t38);
Texture2D<float4> TexCloudResultLum : register(t39);
Texture2D<float4> TexCloudResultAux : register(t40);
RWTexture2D<float> RWTexFilteredTr : register(u3);
RWTexture2D<float4> RWTexFilteredLum : register(u4);
RWTexture2D<float4> RWTexFilteredAux : register(u5);

[numthreads(8, 8, 1)] void filterCloud(uint2 pixel : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(pixel >= uint2(info.activeFrameDim)))
		return;
	const float4 metadata = TexCloudResultAux[pixel];
	const float falloff = 1.0 - saturate(abs(metadata.x - 1.0) / 6.0);
	const float radius = pow(falloff, 4.0) * 0.67 + 0.33;
	const float2 center = float2(pixel) + 0.5;
	const float2 lower = max(center - radius, 0.5) * info.rcpFrameDim;
	const float2 upper = min(center + radius, info.activeFrameDim - 0.5) * info.rcpFrameDim;
	const float2 corners[4] = { float2(lower.x, upper.y), upper, lower, float2(upper.x, lower.y) };
	float4 color = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		color += TexCloudResultLum.SampleLevel(TransmittanceSampler, corners[i], 0);
	}
	color *= 0.25;

	// Keep clouds that lie behind this pixel's own surface off it. The trace takes the farthest
	// surface of each 4x4 block, so at a silhouette (a tree or ridge against the sky) the whole
	// block is traced as sky, and reconstruction and the blur above then lay that cloud over the
	// surface pixels too: a bright, soft rim around the mesh. Only the displayed result is cut
	// here; the temporal history keeps the full trace.
	const float deviceDepth = TexDepth[pixel];
	if (color.a > 0.0 && abs(deviceDepth - FrameBuffer::FarPlaneDepth()) > 1e-6) {
		const float3 ray = CloudScreenRay(pixel);
		const float sceneDistance = SharedData::GetScreenDepth(deviceDepth) / max(abs(mul(FrameBuffer::CameraView, float4(ray, 0)).z), 1e-6);
		const float cloudDistance = DecodeCloudDepth(metadata.x);
		// Fade over the near half of the cloud's weighted depth so a surface inside a cloud bank
		// keeps the part in front of it.
		color *= saturate((sceneDistance - 0.5 * cloudDistance) / max(0.5 * cloudDistance, 1.0));
	}

	RWTexFilteredTr[pixel] = 1.0 - color.a;
	RWTexFilteredLum[pixel] = color;
	RWTexFilteredAux[pixel] = metadata;
}

#endif
