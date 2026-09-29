// Depth and motion guides of the DLSS Neural Rendering pass.
//
// Extracts the render-resolution region of the scene depth and motion vectors into textures of
// exactly that size, starting at (0, 0), which is what the guide metadata sent to Feature 18
// describes. Depth is written as standard (non-inverted) device depth, the contract Open Shaders
// validated; motion is copied undilated so it stays aligned with the centre-pixel depth.

cbuffer Guides : register(b0)
{
	uint Width;
	uint Height;
	uint2 pad0;
};

Texture2D<float2> MotionVectors : register(t0);
Texture2D<float> SceneDepth : register(t1);

RWTexture2D<float2> MotionOutput : register(u0);
RWTexture2D<float> DepthOutput : register(u1);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;

	MotionOutput[id.xy] = MotionVectors[id.xy];

	float depth = SceneDepth[id.xy];
#if defined(REVERSE_Z)
	depth = 1.0 - depth;
#endif
	DepthOutput[id.xy] = depth;
}
