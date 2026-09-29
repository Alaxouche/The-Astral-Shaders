// Colour boundary of the DLSS Neural Rendering pass, ported from Open Shaders (#723).
//
// Feature 18 is handed a bounded, display-referred proxy of the scene, never the scene-linear
// frame itself. The original frame stays untouched in Original; after the model has run, the
// tone difference between the proxy it received and the proxy it returned is applied to the
// original as a bounded gain, so the model can neither replace the frame's hue or alpha nor
// push a bad frame past kMaxToneStops.
//
// This is the production path only. Open Shaders also carries a set of developer diagnostic
// modes (alternative conversions, split views, masks) that this fork does not port.

#include "Common/Color.hlsli"

cbuffer ColorTransfer : register(b0)
{
	uint Width;
	uint Height;
	float ToneLowStrength;   // Local Tone Strength
	float ToneHighStrength;  // Local Structure Strength
	float ToneRadius;
	uint HasToneData;
	float2 pad0;
};

Texture2D<float4> Original : register(t0);
Texture2D<float4> NeuralInput : register(t1);
Texture2D<float4> NeuralOutput : register(t2);
Texture2D<float2> ToneData : register(t4);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float2> ToneDataOutput : register(u2);

static const float3 Luma = float3(0.2125, 0.7154, 0.0721);
static const float kProxyEpsilon = 1e-8;
static const float kPeakEpsilon = 1e-6;
static const float kLumaEpsilon = 1e-5;
static const float kWeightEpsilon = 1e-5;
static const float kSpatialEpsilon = 1e-4;
// Tone clamp in stops: bounded so a bad model frame cannot blow out the frame.
static const float kMaxToneStops = 2.0;

float3 ProxyLinearToSrgb(float3 value)
{
	value = saturate(value);
	return lerp(value * 12.92, 1.055 * pow(max(value, kProxyEpsilon), 1.0 / 2.4) - 0.055, step(0.0031308, value));
}

float3 ProxySrgbToLinear(float3 value)
{
	value = saturate(value);
	return lerp(value / 12.92, pow((value + 0.055) / 1.055, 2.4), step(0.04045, value));
}

// Hue-preserving soft knee: the peak channel follows x / sqrt(x^2 + 1), the others scale with it.
float3 NeutwoEncode(float3 value)
{
	value = max(value, 0.0);
	float peak = max(value.r, max(value.g, value.b));
	if (peak <= kPeakEpsilon)
		return value;
	return value * ((peak * rsqrt(peak * peak + 1.0)) / peak);
}

float3 SceneToLinear(float3 nativeColor)
{
	return ENABLE_LL ? nativeColor : Color::SkyrimGammaToLinear(nativeColor);
}

// exp2 of the tone in the source's own domain: tone 0 stays an exact gain-1 passthrough.
float CompositeGain(float tone)
{
	tone = isfinite(tone) ? tone : 0.0;
	float gain = exp2(clamp(tone, -kMaxToneStops, kMaxToneStops));
	return ENABLE_LL ? gain : Color::LinearToSkyrimGamma(gain);
}

[numthreads(8, 8, 1)] void Prepare(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	float3 source = Original[id.xy].rgb;
	// The saturate inside ProxyLinearToSrgb bounds this to [0, 1] whatever the input holds.
	float3 proxy = ProxyLinearToSrgb(NeutwoEncode(max(SceneToLinear(source), 0.0)));
	Output[id.xy] = float4(proxy, 1.0);
}

	[numthreads(8, 8, 1)] void PrepareToneData(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height)
		return;
	float3 input = ProxySrgbToLinear(NeuralInput[id.xy].rgb);
	float3 output = ProxySrgbToLinear(NeuralOutput[id.xy].rgb);
	float inputLuma = max(dot(input, Luma), kLumaEpsilon);
	float outputLuma = max(dot(output, Luma), kLumaEpsilon);
	float logInput = log2(inputLuma);
	ToneDataOutput[id.xy] = float2(logInput, log2(outputLuma) - logInput);
}

// Edge-stopping 5x5 average of the tone difference: the low spatial band.
float ToneLowAt(int2 pixel, float centerDelta)
{
	float radius = ToneRadius;
	if (radius <= 0.01)
		return centerDelta;
	int2 limit = int2(max(Width, 1u) - 1, max(Height, 1u) - 1);
	float centerLogLuma = ToneData[pixel].x;
	float weighted = 0.0;
	float weightSum = 0.0;
	for (int y = -2; y <= 2; ++y) {
		for (int x = -2; x <= 2; ++x) {
			float distance = float(x * x + y * y);
			float spatial = exp(-distance / max(2.0 * radius * radius, kSpatialEpsilon));
			int2 samplePixel = clamp(pixel + int2(x, y), int2(0, 0), limit);
			float2 sample = ToneData[samplePixel];
			float edge = exp(-abs(sample.x - centerLogLuma) * 2.0);
			float weight = spatial * edge;
			weighted += sample.y * weight;
			weightSum += weight;
		}
	}
	return weightSum > kWeightEpsilon ? weighted / weightSum : centerDelta;
}

[numthreads(8, 8, 1)] void Composite(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Width || id.y >= Height)
		return;
	float4 original = Original[id.xy];
	float3 rawNeural = NeuralOutput[id.xy].rgb;
	if (!all(isfinite(original))) {
		Output[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	Output[id.xy] = original;
	if (!all(isfinite(rawNeural)))
		return;

	float3 inputProxy = ProxySrgbToLinear(NeuralInput[id.xy].rgb);
	float3 neuralProxy = ProxySrgbToLinear(rawNeural);
	const float ratioFloor = 1.0 / 512.0;
	float inputLuminance = dot(inputProxy, Luma);
	float neuralLuminance = dot(neuralProxy, Luma);

	float toneDelta = log2(max(neuralLuminance, ratioFloor)) - log2(max(inputLuminance, ratioFloor));
	float toneLow = toneDelta;
	[branch] if (HasToneData != 0)
		toneLow = ToneLowAt(int2(id.xy), toneDelta);
	float toneHigh = toneDelta - toneLow;
	float tone = ToneLowStrength == ToneHighStrength ? toneDelta * ToneHighStrength :
	                                                   toneLow * ToneLowStrength + toneHigh * ToneHighStrength;

	float3 result = original.rgb * CompositeGain(tone);
	if (!all(isfinite(result)))
		return;
	Output[id.xy] = float4(result, original.a);
}
