// Window light shafts: volumetric sun rays entering through Living Windows glass.
//
// renderShafts (CS, half dynamic res): reconstructs each pixel's world ray from the camera
// basis and frustum slopes, marches it up to the scene depth, and accumulates in-scattering
// inside per-window beam volumes -- a window's beam extends from its pane along the sun
// direction, with a lateral radius that spreads slowly with distance. Depth bounding is the
// occlusion: a beam behind a wall never accumulates because those steps sit past the depth.
//
// shaftVS/shaftPS: fullscreen triangle that upsamples the half-res beam buffer and adds it
// onto the scene (additive blend state set by the CPU side).
//
// Compiled at runtime by Util::CompileShader; self-contained cbuffer, no SharedData.

Texture2D<float> SceneDepth : register(t0);
RWTexture2D<float4> ShaftOut : register(u0);
Texture2D<float4> ShaftTex : register(t0);
SamplerState LinearSampler : register(s0);

#define MAX_SHAFT_WINDOWS 16

cbuffer ShaftCB : register(b0)
{
	float4 CamDir;       // xyz camera forward, w = reverse-Z flag
	float4 CamRight;     // xyz camera right
	float4 CamUp;        // xyz camera up
	float4 Slopes;       // (r-l)/2, (r+l)/2, (t-b)/2, (t+b)/2  (frustum slopes at unit distance)
	float4 DepthParams;  // F, NF (viewZ = NF / (F - z)), march far limit, step count
	float4 SunDir;       // xyz direction the light TRAVELS (from sun), w = phase anisotropy
	float4 SunColor;     // rgb tint * intensity, w = beam spread per unit distance
	float4 Dims;         // dynamic half-res W, H, dynamic full-res W, H
	float4 Dims2;        // allocated half-res texture W, H; unused zw
	float4 Params;       // beam length, density, window count, frame (jitter)
	float4 WindowCenter[MAX_SHAFT_WINDOWS];  // xyz camera-relative center, w = lateral radius
	float4 WindowNormal[MAX_SHAFT_WINDOWS];  // xyz pane normal (0 = unknown), w unused
};

float InterleavedGradientNoise(float2 pos)
{
	return frac(52.9829189 * frac(0.06711056 * pos.x + 0.00583715 * pos.y));
}

float PhaseHG(float cosTheta, float g)
{
	const float g2 = g * g;
	return (1.0 - g2) / max(pow(abs(1.0 + g2 - 2.0 * g * cosTheta), 1.5), 1e-4) * 0.0795774715;
}

[numthreads(8, 8, 1)] void renderShafts(uint2 tid : SV_DispatchThreadID)
{
	if (any(tid >= uint2(Dims.xy)))
		return;

	// Pixel -> NDC -> world ray at unit forward distance, matching the game projection's
	// slope convention (validated in game by the occlusion culler's Hi-Z tests).
	const float2 uv = (tid + 0.5) / Dims.xy;
	const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	const float sx = ndc.x * Slopes.x + Slopes.y;
	const float sy = ndc.y * Slopes.z + Slopes.w;
	const float3 rayPerZ = CamDir.xyz + CamRight.xyz * sx + CamUp.xyz * sy;

	// Scene view-depth bound from the depth buffer (sampled at the matching full-res texel).
	const uint2 fullPx = min(uint2(tid * 2u), uint2(Dims.zw) - 1u);
	float deviceZ = SceneDepth[fullPx];
	if (CamDir.w > 0.5)
		deviceZ = 1.0 - deviceZ;
	const float viewZ = DepthParams.y / max(DepthParams.x - deviceZ, 1e-6);
	const float marchEnd = min(viewZ, DepthParams.z);

	const float steps = DepthParams.w;
	const float stepZ = marchEnd / steps;
	const float jitter = InterleavedGradientNoise(tid + Params.w);
	const float3 rayNorm = normalize(rayPerZ);
	const float phase = PhaseHG(dot(rayNorm, SunDir.xyz), SunDir.w);
	const uint count = min(uint(Params.z), MAX_SHAFT_WINDOWS);

	float accum = 0.0;
	for (float s = 0.0; s < steps; s += 1.0) {
		const float z = (s + jitter) * stepZ;
		const float3 p = rayPerZ * z;  // camera-relative sample point
		for (uint w = 0; w < count; w++) {
			const float3 rel = p - WindowCenter[w].xyz;
			const float along = dot(rel, SunDir.xyz);
			if (along < 0.0 || along > Params.x)
				continue;
			const float3 lateral = rel - SunDir.xyz * along;
			const float radius = WindowCenter[w].w * (1.0 + SunColor.w * along);
			if (dot(lateral, lateral) > radius * radius)
				continue;
			// Beams from panes the sun cannot face contribute nothing.
			const float3 n = WindowNormal[w].xyz;
			const float facing = dot(n, n) > 0.1 ? abs(dot(n, SunDir.xyz)) : 1.0;
			if (facing < 0.25)
				continue;
			// Fade along the beam and toward its lateral edge.
			const float edge = 1.0 - saturate(dot(lateral, lateral) / max(radius * radius, 1e-4));
			accum += facing * edge * (1.0 - along / Params.x) * stepZ;
		}
	}

	const float3 lum = SunColor.rgb * (accum * Params.y * 0.001 * phase);
	ShaftOut[tid] = float4(lum, 0.0);
}

struct ShaftVSOut
{
	float4 Position : SV_Position;
	float2 Uv : TEXCOORD0;
};

ShaftVSOut shaftVS(uint id : SV_VertexID)
{
	ShaftVSOut vsout;
	vsout.Uv = float2((id << 1) & 2, id & 2);
	vsout.Position = float4(vsout.Uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return vsout;
}

float4 shaftPS(ShaftVSOut input) : SV_Target
{
	// Texture and render target both halve, so the screen uv IS the shaft uv; only the clamp
	// must stop at the edge of the dynamic render area the CS actually wrote.
	const float2 validMax = Dims.xy / Dims2.xy - 0.5 / Dims2.xy;
	const float2 uv = min(input.Uv, validMax);
	return float4(ShaftTex.SampleLevel(LinearSampler, uv, 0).rgb, 0.0);
}
