#ifndef __ICE_HLSLI__
#define __ICE_HLSLI__

// Clear ice seen from above: the view ray refracts in, passes two depths of fracture planes and bubbles, and reaches the
// body of the ice below, everything along the way absorbed toward the ice's deep color. PBR surfaces rougher than
// FrostRoughness (frost, snow) hide the interior; vanilla specular masks are not roughness, so vanilla passes 0.
// Three texture fetches, only on draws an Ice rule matched and within the fade distance.
namespace Ice
{
#if defined(PSHADER)
	cbuffer IcePerDraw : register(b13)
	{
		float Strength;  // 0 on draws that take the permutation only for Detail Maps
		float Depth;     // Body depth in UV units
		float Cracks;
		float Bubbles;
		float Scale;           // Volume texture tiling per UV
		float FrostRoughness;  // Roughness around which the surface turns to frost and hides the interior
		float FadeEnd;
		float FadeInvRange;
		float3 Absorption;  // -ln(deep color): straight down to the body, light keeps exp(-Absorption)
		float Glow;         // Brightness of light scattered by planes and bubbles
	};

	Texture2D<float2> TexVolume : register(t59);

	static const float InverseIndex = 1.0 / 1.31;
	static const float MinDescent = 0.2;     // Bounds the slide at grazing angles
	static const float FrostBand = 0.15;     // Half-width of the roughness ramp around FrostRoughness
	static const float GrazingStart = 0.08;  // cos(view angle from the normal) where the interior is gone
	static const float GrazingEnd = 0.4;     // and where it is fully shown
	static const float NearBlur = 1.0;       // Mip biases: deeper layers are seen through more scattering
	static const float FarBlur = 2.0;
	static const float FarWeight = 0.6;
	static const float Shadowing = 0.4;  // How much a lit plane hides what lies beneath it
	static const float NearDepth = 0.3;  // Layer depths as fractions of Depth
	static const float FarDepth = 0.7;
	static const float BodyBlur = 2.5;  // Mip bias: the body is seen through the scattering above it
	static const float2x2 FarRotation = float2x2(0.799, -0.602, 0.602, 0.799);

	struct Volume
	{
		float Clear;
		float2 NearUV;
		float2 FarUV;
		float2 BodyUV;
		float Path;  // Path length per unit of depth, 1 straight down
		// Gradients taken before the per-pixel branch, where implicit derivatives would be undefined.
		float4 UvGrad;       // ddx(uv).xy, ddy(uv).xy, with the global mip bias applied
		float4 DiffuseGrad;  // the same for the diffuse UV
	};

	/**
	 * Call in uniform control flow: it takes the UV gradients the later samples use.
	 * @param uv Material UV; the volume layers slide from it.
	 * @param diffuseUv The UV the diffuse is sampled at; the body slides from it.
	 * @param tangentNormal Surface normal in tangent space, so the normal map bends the refraction.
	 * @param tangentView Direction to the eye in tangent space.
	 */
	Volume Trace(float2 uv, float2 diffuseUv, float3 tangentNormal, float3 tangentView, float roughness, float viewDepth)
	{
		Volume volume = (Volume)0;
		// Strength is 0 on draws that take the permutation only for Detail Maps: a uniform branch skips it all.
		[branch] if (Strength > 0.0)
		{
			const float gradScale = exp2(SharedData::MipBias);
			volume.UvGrad = float4(ddx(uv), ddy(uv)) * gradScale;
			volume.DiffuseGrad = float4(ddx(diffuseUv), ddy(diffuseUv)) * gradScale;

			const float fade = saturate((FadeEnd - viewDepth) * FadeInvRange);
			const float3 view = normalize(tangentView);
			// At grazing angles ice is mostly reflection, and a crisp interior there reads as a decal.
			const float facing = smoothstep(GrazingStart, GrazingEnd, view.z);
			volume.Clear = Strength * fade * facing * (1.0 - smoothstep(FrostRoughness - FrostBand, FrostRoughness + FrostBand, roughness));

			// A few ALU; the texture reads stay behind the caller's Clear > 0 branch.
			const float3 ray = refract(-view, normalize(tangentNormal), InverseIndex);
			const float descent = max(-ray.z, MinDescent);
			const float2 slide = ray.xy / descent * Depth;
			volume.Path = 1.0 / descent;
			volume.NearUV = (uv + slide * NearDepth) * Scale;
			volume.FarUV = mul(FarRotation, (uv + slide * FarDepth) * (Scale * 1.37)) + 0.37;
			volume.BodyUV = diffuseUv + slide;
		}
		return volume;
	}

	float3 Transmittance(float depthFraction, float path)
	{
		return exp(-Absorption * (depthFraction * path));
	}

	/** The material's diffuse under the ice: blurred, as if seen through the scattering above it. */
	float4 SampleBody(Texture2D<float4> tex, SamplerState samp, Volume volume)
	{
		const float scale = exp2(BodyBlur);
		return tex.SampleGrad(samp, volume.BodyUV, volume.DiffuseGrad.xy * scale, volume.DiffuseGrad.zw * scale);
	}

	/**
	 * Blends the surface albedo toward what lies under it. body is SampleBody's result in working space.
	 * The volume UVs wrap here, so any material sampler (even a clamping one) tiles the volume correctly.
	 */
	float3 Compose(float3 albedo, float3 body, Volume volume, SamplerState samp)
	{
		const float nearScale = Scale * exp2(NearBlur);
		const float farScale = Scale * 1.37 * exp2(FarBlur);
		const float2 nearLayer = TexVolume.SampleGrad(samp, frac(volume.NearUV), volume.UvGrad.xy * nearScale, volume.UvGrad.zw * nearScale);
		const float2 farLayer = TexVolume.SampleGrad(samp, frac(volume.FarUV), mul(FarRotation, volume.UvGrad.xy) * farScale, mul(FarRotation, volume.UvGrad.zw) * farScale);
		const float nearScatter = nearLayer.x * Cracks + nearLayer.y * Bubbles;
		const float farScatter = farLayer.x * Cracks + farLayer.y * Bubbles;

		// Planes and bubbles send light back up and shade what lies beneath them.
		const float3 nearT = Transmittance(NearDepth, volume.Path);
		const float3 farT = Transmittance(FarDepth, volume.Path);
		const float3 bodyT = Transmittance(1.0, volume.Path);
		const float nearShadow = saturate(nearScatter * Shadowing);
		const float occlusion = nearShadow + saturate(farScatter * Shadowing * 0.66) * (1.0 - nearShadow);
		const float3 scatter = Glow * (nearScatter * nearT + FarWeight * farScatter * farT);
		const float3 interior = body * bodyT * (1.0 - occlusion) + scatter;
		return lerp(albedo, interior, volume.Clear);
	}
#endif
}

#endif
