#include "Common/Color.hlsli"
#include "Common/FastMath.hlsli"
#include "Common/FlareOcclusion.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

struct VS_INPUT
{
	float4 Position: POSITION0;

#if defined(TEX) || defined(HORIZFADE)
	float2 TexCoord: TEXCOORD0;
#endif

	float4 Color: COLOR0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;

#if defined(DITHER) && defined(TEX)
	float4 TexCoord0: TEXCOORD0;
#elif defined(DITHER)
	float2 TexCoord0: TEXCOORD3;
#elif defined(TEX) || defined(HORIZFADE)
	float2 TexCoord0: TEXCOORD0;
#endif

#if defined(TEXLERP)
	float2 TexCoord1: TEXCOORD1;
#endif

#if defined(HORIZFADE)
	float TexCoord2: TEXCOORD2;
#endif

#if defined(TEX) || defined(DITHER) || defined(HORIZFADE)
	float4 Color: COLOR0;
#endif

#if !defined(OCCLUSION) && !defined(MOONMASK) && !defined(HORIZFADE)
	float4 SkyBlendColor0: TEXCOORD5;
	float4 SkyBlendColor2: TEXCOORD6;
#endif

	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
	float3 FogPosition: TEXCOORD4;
};

#ifdef VSHADER
cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj : packoffset(c0);
	row_major float4x4 World : packoffset(c4);
	row_major float4x4 PreviousWorld : packoffset(c8);
	float3 EyePosition : packoffset(c12);
	float VParams : packoffset(c12.w);
	float4 BlendColor[3] : packoffset(c13);
	float2 TexCoordOff : packoffset(c16);
};

#	if defined(DITHER) && defined(TEX)
/** @brief Fraction of the sun's neighbourhood showing sky, since the vanilla sun query quad sees gaps between distant ridges. */
float GetSunGlareVisibility()
{
	// Scene depth belongs to the main view, not the reflection camera
	if (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection)
		return 1.0;

	float4 sunPositionCS = mul(FrameBuffer::CameraViewProj, float4(SharedData::SunDirection.xyz, 0.0));
	if (sunPositionCS.w <= 0.0)
		return 1.0;

	float2 sunUV = sunPositionCS.xy / sunPositionCS.w * float2(0.5, -0.5) + 0.5;
	uint visibleSamples = 0;
	[unroll] for (uint i = 0; i < FlareOcclusion::SampleCount; i++)
	{
		float2 sampleUV = sunUV + FlareOcclusion::GetSampleOffset(i);
		float sampleDepth = SharedData::GetDepth(sampleUV);
#		ifdef REVERSE_Z
		bool isSky = FrameBuffer::IsReverseProjection() ? sampleDepth <= 0.0 : sampleDepth >= 1.0;
#		else
		bool isSky = sampleDepth >= 1.0;
#		endif
		visibleSamples += FrameBuffer::IsOutsideFrame(sampleUV) || isSky;
	}
	return FlareOcclusion::GetVisibility(visibleSamples);
}
#	endif

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float4 inputPosition = float4(input.Position.xyz, 1.0);

#	if defined(OCCLUSION)

	// Intentionally left blank

#	elif defined(MOONMASK)

	vsout.TexCoord0 = input.TexCoord;
	vsout.Color = float4(VParams.xxx, 1.0);

#	elif defined(HORIZFADE)

	float worldHeight = mul(World, inputPosition).z;
	float eyeHeightDelta = -EyePosition.z + worldHeight;

	vsout.TexCoord0.xy = input.TexCoord;
	vsout.TexCoord2.x = saturate((1.0 / 17.0) * eyeHeightDelta);
	vsout.Color.xyz = BlendColor[0].xyz * VParams;
	vsout.Color.w = BlendColor[0].w;

#	else  // MOONMASK HORIZFADE

#		if defined(DITHER)

#			if defined(TEX)
	vsout.TexCoord0.xyzw = input.TexCoord.xyxy * float4(1.0, 1.0, 501.0, 501.0);
#			else
	float3 inputDirection = normalize(input.Position.xyz);
	inputDirection.y += inputDirection.z;

	vsout.TexCoord0.x = 501 * acos(inputDirection.x);
	vsout.TexCoord0.y = 501 * asin(inputDirection.y);
#			endif  // TEX

#		elif defined(CLOUDS)
	vsout.TexCoord0.xy = TexCoordOff + input.TexCoord;
#		else
	vsout.TexCoord0.xy = input.TexCoord;
#		endif  // DITHER CLOUDS

#		ifdef TEXLERP
	vsout.TexCoord1.xy = TexCoordOff + input.TexCoord;
#		endif  // TEXLERP

	float3 skyColor = BlendColor[0].xyz * input.Color.xxx + BlendColor[1].xyz * input.Color.yyy +
	                  BlendColor[2].xyz * input.Color.zzz;

	vsout.Color.xyz = VParams * skyColor;
	vsout.Color.w = BlendColor[0].w * input.Color.w;
	vsout.SkyBlendColor0 = float4(BlendColor[0].xyz * VParams, 0);
	vsout.SkyBlendColor2 = float4(BlendColor[2].xyz * VParams, 0);
#		if defined(DITHER) && defined(TEX)
	vsout.Color.w *= GetSunGlareVisibility();
#		endif
#	endif  // OCCLUSION MOONMASK HORIZFADE

#	ifdef REVERSE_Z
	float4 skyPosition = mul(WorldViewProj, inputPosition);
	vsout.Position = float4(skyPosition.xy, FrameBuffer::FarPlaneClipZ(skyPosition.w), skyPosition.w);
#	else
	vsout.Position = mul(WorldViewProj, inputPosition).xyww;
#	endif
	vsout.WorldPosition = mul(World, inputPosition);
	vsout.FogPosition = vsout.WorldPosition.xyz - EyePosition.xyz;
	vsout.PreviousWorldPosition = mul(PreviousWorld, inputPosition);

	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
	float4 MotionVectors: SV_Target1;
	float4 Normal: SV_Target2;
#if defined(CLOUD_SHADOWS) && defined(CLOUDS) && !defined(DEFERRED)
	float4 CloudShadows: SV_Target3;
#endif
};

#ifdef PSHADER
SamplerState SampBaseSampler : register(s0);
SamplerState SampBlendSampler : register(s1);
SamplerState SampNoiseGradSampler : register(s2);

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexBlendSampler : register(t1);
Texture2D<float4> TexNoiseGradSampler : register(t2);

cbuffer PerGeometry : register(b2)
{
	float2 PParams : packoffset(c0);
};

cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}

#	include "Common/MotionBlur.hlsli"
#	include "Common/SharedData.hlsli"

#	if defined(CLOUD_SHADOWS)
#		if defined(CLOUDS)
// Cloud self shadowing is an extension of the fork's own Cloud Shadows -- its settings field, its
// GetCloudSelfShadow and the per-deck t26 binding all live there, and none of that is in this
// build. Never defining CLOUD_SELF_SHADOW compiles the path out; Physical Sky's own use of the
// occlusion cubemap sits under a different guard and is unaffected.
#		endif
#		include "CloudShadows/CloudShadows.hlsli"
#	endif

#	if defined(EFFECTS11) && (defined(HORIZFADE) || (defined(TEX) && !defined(DITHER) && !defined(CLOUDS)))
#		define EFFECTS11_CELESTIAL_EXTINCTION
#	endif

#	if defined(EFFECTS11) && defined(DITHER) && !defined(TEX)
#		define EFFECTS11_SKY_GRADIENT
#	endif

#	if defined(EFFECTS11) && (defined(CLOUDS) || defined(EFFECTS11_CELESTIAL_EXTINCTION) || defined(EFFECTS11_SKY_GRADIENT))
#		include "Effects11/SkyScattering.hlsli"
#	endif

#	if defined(PROCEDURAL_SUN)
#		include "ProceduralSun/ProceduralSun.hlsli"
#	endif

#	if defined(NIGHT_SKY)
#		include "NightSky/NightSky.hlsli"
#	endif

#	ifdef HDR_OUTPUT
#		include "HDRDisplay/HDRSun.hlsli"
#		include "Common/Random.hlsli"
#	endif

#	if defined(PHYSICAL_SKY)
#		define PS_SKY_SAMPLERS
#		include "PhysicalSky/Common.hlsli"
#		if defined(TEX) && defined(CLOUDS)
#			define PS_CLOUDS
#		endif
#	endif

#	if defined(EXP_HEIGHT_FOG)
#		define SampColorSampler SampBlendSampler
#		include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#	endif

Texture2D<float> TexDepthSampler : register(t17);

#	if defined(EFFECTS11) && (defined(HORIZFADE) || defined(MOONMASK))
float3 ShadeStars(float4 starTexel, float2 uv)
{
	float3 color = starTexel.xyz;
	[branch] if (SharedData::enbSettings.EnableAnimatedStars) {
		float2 textureSize;
		TexBaseSampler.GetDimensions(textureSize.x, textureSize.y);
		uint seed = Random::iqint3(uint2(floor(frac(uv) * textureSize)));
		float2 star = Random::f2(seed);
		float4 quad = TexBaseSampler.GatherAlpha(SampBaseSampler, uv);
		float isolation = max(max(quad.x, quad.y), max(quad.z, quad.w)) - dot(quad, 0.25);
		float mask = saturate(isolation * SharedData::enbSettings.StarsAnimationDensity - 0.5);
		float rate = SharedData::enbSettings.StarsAnimationTime * (2.0 + 6.0 * star.y);
		float wave = 0.5 + 0.5 * sin(Math::TAU * frac(SharedData::Timer * rate + star.x));
		color *= 1.0 + wave * wave * mask * SharedData::enbSettings.StarsAnimationIntensity * 0.3;
	}
	float3 squared = color * color;
	color = lerp(color, squared * squared, SharedData::enbSettings.StarsCurve);
	return max(color, 0.0) * SharedData::enbSettings.StarsIntensity;
}
#	endif

#	if defined(PHYSICAL_SKY)
float GetPhysSkyCloudShadow(float3 viewDir, uint2 pxCoord)
{
	if ((Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection) != 0) {
#		if defined(CLOUD_SHADOWS)
		float cloudCubeSample = CloudShadows::CloudShadowsTexture.SampleLevel(SampBaseSampler, viewDir, 0).x;
		return saturate(cloudCubeSample * SharedData::cloudShadowsSettings.Opacity);
#		else
		return 0.0;
#		endif
	}

	return PhysSky::GetApShadow(pxCoord);
}
#	endif

// The sun disc belongs to the Procedural Sun feature alone; Physical Sky no longer draws one, so the
// two can never stack.
#	if defined(PROCEDURAL_SUN)
bool ProceduralSunActive()
{
	return SharedData::proceduralSunSettings.enabled;
}
#	endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	// Color::Sky is float3->float3 (per-channel sky gamma). PParams.yyy broadcasts the packed
	// scalar in PParams.y to RGB; float3 matches output .xyz where skyScale is added.
	float3 skyScale = Color::Sky(PParams.yyy);

#	if defined(NIGHT_SKY) && defined(DITHER) && !defined(TEX)
	// Stars, Milky Way and aurora on the dome. The pixel's angular size, taken here in uniform
	// control flow, sizes each star to exactly the pixels it covers.
	const float3 nightSkyDir = normalize(input.WorldPosition.xyz);
	const float nightSkyPixelAngle = max(length(ddx(nightSkyDir)), length(ddy(nightSkyDir)));
	float3 nightSkyRadiance = 0.0;
	[branch] if (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld)
		nightSkyRadiance = NightSky::Radiance(nightSkyDir, nightSkyPixelAngle);
#	endif

#	if defined(PS_CLOUDS)
	float psCloudDist = 1e3f / 1.428e-2;
	float3 viewDir = normalize(input.WorldPosition.xyz);
#		if defined(CLOUD_SHADOWS)
	if (SharedData::physSkyData.enabled)
		psCloudDist = CloudShadows::IntersectCloudDist(float3(0, 0, 0), viewDir);
#		endif
#	endif

#	ifndef OCCLUSION
#		ifndef TEXLERP
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
#			if defined(EFFECTS11) && (defined(HORIZFADE) || defined(MOONMASK))
	[branch] if (SharedData::enbSettings.Enable)
		baseColor.xyz = ShadeStars(baseColor, input.TexCoord0.xy);
#			elif defined(EFFECTS11) && defined(TEX) && !defined(DITHER) && !defined(CLOUDS)
	[branch] if (SharedData::enbSettings.Enable && (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsMoon)) {
		float2 edge = abs(input.TexCoord0.xy * 2.0 - 1.0);
		baseColor.xyz = pow(max(baseColor.xyz, 0.0), SharedData::enbSettings.MoonCurve);
		if (max(edge.x, edge.y) > 0.985)
			baseColor = 0.0;
	}
#			endif
	baseColor.xyz = Color::Sky(baseColor.xyz);
#			ifdef TEXFADE
	baseColor.w *= PParams.x;
#			endif
#		else
	float4 blendColor = TexBlendSampler.Sample(SampBlendSampler, input.TexCoord1.xy);
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, input.TexCoord0.xy);
	blendColor.xyz = Color::Sky(blendColor.xyz);
	baseColor.xyz = Color::Sky(baseColor.xyz);
	baseColor = PParams.xxxx * (-baseColor + blendColor) + baseColor;
#		endif

#		if defined(PROCEDURAL_SUN) && defined(TEX) && defined(DEFERRED) && !defined(DITHER) && !defined(CLOUDS) && !defined(MOONMASK)
	bool proceduralSunActive = SharedData::proceduralSunSettings.enabled &&
	                           (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun) &&
	                           (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);
	[branch] if (proceduralSunActive) {
		float3 viewDirection = normalize(input.WorldPosition.xyz);
		float cosTheta = clamp(dot(viewDirection, SharedData::SunDirection.xyz), -1.0, 1.0);

		float influenceCos = ProceduralSun::GetInfluenceCos(
			SharedData::proceduralSunSettings.sunDiskCos,
			SharedData::proceduralSunSettings.haloEnabled,
			SharedData::proceduralSunSettings.sunHaloCos,
			SharedData::proceduralSunSettings.haloIntensity);

		float3 proceduralSunColor = 0.0;
		float sunCoverage = 0.0;

		[branch] if (cosTheta > influenceCos) {
			float3 limbDarkening;
			float discCoverage;
			ProceduralSun::EvaluateDisc(
				cosTheta,
				SharedData::proceduralSunSettings.sunDiskCos,
				SharedData::proceduralSunSettings.edgeSoftness,
				limbDarkening,
				discCoverage);

			float haloProfile = 0.0;
			if (SharedData::proceduralSunSettings.haloEnabled) {
				haloProfile = ProceduralSun::EvaluateHalo(
					cosTheta,
					SharedData::proceduralSunSettings.sunDiskCos,
					SharedData::proceduralSunSettings.sunHaloCos,
					SharedData::proceduralSunSettings.haloFalloff);
			}

			ProceduralSun::ComposeDiscAndHalo(
				limbDarkening,
				discCoverage,
				SharedData::proceduralSunSettings.diskIntensity,
				haloProfile,
				SharedData::proceduralSunSettings.haloIntensity,
				proceduralSunColor,
				sunCoverage);

#			if defined(CLOUD_SHADOWS)
			float cloudExtinction = SharedData::proceduralSunSettings.cloudExtinction * sunCoverage;
			[branch] if (cloudExtinction > 0.0) {
				float capturedCloudOcclusion = CloudShadows::CloudShadowsTexture.SampleLevel(SampBaseSampler, viewDirection, 0).x;
				proceduralSunColor *= ProceduralSun::GetCloudTransmission(capturedCloudOcclusion, cloudExtinction);
			}
#			endif
		}

		baseColor.xyz = proceduralSunColor;
		baseColor.w = sunCoverage * saturate(TexBaseSampler.SampleLevel(SampBaseSampler, 0.5, 0).w);

		skyScale = 0.0;
	}
#		endif

#		if defined(HDR_OUTPUT)
	float hdrSunGain = HDRSun::GetHdrSunGain(input.TexCoord0.xy, baseColor);
	baseColor.xyz *= hdrSunGain;
	if (HDRSun::IsHdrSunActive()) {
		// Dither bright output to reduce banding in high-boost sun path.
		// Same baseColor/skyScale treatment for DITHER and non-DITHER; DITHER adds noiseGrad later.
		baseColor.xyz += (Random::InterleavedGradientNoise(input.Position.xy) - 0.5f) *
		                 (saturate(hdrSunGain - 1.0f) / 255.0f);
		skyScale = 0.0f;
	}

#			if defined(CLOUD_SHADOWS)
	if (HDRSun::IsHdrSunActive()) {
		float cloudMult = CloudShadows::GetCloudShadowMult(input.WorldPosition.xyz, SampBaseSampler);
		baseColor.xyz *= cloudMult;
		baseColor.w *= cloudMult;
	}
#			endif
#		endif

#		if defined(PS_CLOUDS) && defined(CLOUD_SHADOWS)
	if (SharedData::physSkyData.enabled && SharedData::physSkyData.enableVanillaClouds)
		baseColor.rgb = PhysSky::RelightCloud(baseColor, viewDir, float3(0, 0, 0) + viewDir * psCloudDist, PhysSky::SampTr, SampBaseSampler);
	else if (SharedData::physSkyData.enabled && !SharedData::physSkyData.enableVanillaClouds)
		baseColor.a = 0;  // Hide vanilla clouds when disabled
#		elif defined(PS_CLOUDS)
	if (SharedData::physSkyData.enabled && !SharedData::physSkyData.enableVanillaClouds)
		baseColor.a = 0;  // Hide vanilla clouds when disabled (no cloud shadows)
#		endif

#		if defined(DITHER)
	float2 noiseGradUv = float2(0.125, 0.125) * input.Position.xy;
	float noiseGrad = TexNoiseGradSampler.Sample(SampNoiseGradSampler, noiseGradUv).x * 0.03125 - 0.0078125;
	noiseGrad *= 10.0;

#			ifdef TEX
	psout.Color.xyz = Color::Sky(input.Color.xyz) * baseColor.xyz + skyScale;
	psout.Color.xyz *= 1.0 + noiseGrad;
	psout.Color.w = baseColor.w * input.Color.w;

#				if defined(PROCEDURAL_SUN) && defined(CLOUD_SHADOWS) && defined(DEFERRED)
	if (ProceduralSunActive() && SharedData::proceduralSunSettings.cloudExtinction > 0.0) {
		float capturedCloudOcclusion = CloudShadows::CloudShadowsTexture.SampleLevel(SampBaseSampler, SharedData::SunDirection.xyz, 0).x;
		psout.Color.w *= ProceduralSun::GetGlareCloudTransmission(capturedCloudOcclusion, SharedData::proceduralSunSettings.cloudExtinction);
	}
#				endif
#			else
	float3 skyGradientColor = input.Color.xyz;

#				if defined(EFFECTS11)
	float3 viewDirection = normalize(input.WorldPosition.xyz);
	if (SharedData::enbSettings.UseProceduralGradientWeights) {
		float gradientPosition = pow(1.0 - saturate(viewDirection.z), SharedData::enbSettings.ProceduralGradientWeightCurve);
		skyGradientColor = lerp(input.SkyBlendColor2.xyz, input.SkyBlendColor0.xyz, gradientPosition);
	}
	[branch] if (SharedData::enbSettings.EnableCloudsScattering)
		skyGradientColor = SkyScattering::ApplySkyScattering(skyGradientColor, input.SkyBlendColor2.xyz, viewDirection) + SkyScattering::GetMoonGlow(viewDirection);
#				endif
	psout.Color.xyz = Color::Sky(skyGradientColor) + skyScale;

#				if defined(EFFECTS11)
	[branch] if (SharedData::enbSettings.Enable && !SharedData::enbSettings.FixBlackCrush) {
		float3 additiveDither = max(psout.Color.xyz + noiseGrad * 0.1, 0.0);
		psout.Color.xyz = lerp(additiveDither, psout.Color.xyz * (1.0 + noiseGrad), saturate(dot(psout.Color.xyz, 8.0)));
	} else {
		psout.Color.xyz *= 1.0 + noiseGrad;
	}
#				else
	psout.Color.xyz *= 1.0 + noiseGrad;
#				endif
	psout.Color.w = input.Color.w;
#				if defined(NIGHT_SKY)
	psout.Color.xyz += nightSkyRadiance;
#				endif
#			endif  // TEX

#		elif defined(MOONMASK)
	psout.Color.xyzw = baseColor;

	if (baseColor.w - AlphaTestRefRS.x < 0) {
		discard;
	}

#		elif defined(HORIZFADE)
#			if defined(EFFECTS11)
	if (SharedData::enbSettings.Enable)
		skyScale = 0.0;
#			endif
	psout.Color.xyz = float3(1.5, 1.5, 1.5) * (Color::Sky(input.Color.xyz) * baseColor.xyz + skyScale);
	psout.Color.w = input.TexCoord2.x * (baseColor.w * input.Color.w);
#			if defined(NIGHT_SKY)
	// The procedural star field replaces the game's star texture.
	if (SharedData::nightSkySettings.Enabled != 0 && SharedData::nightSkySettings.ReplaceStars != 0)
		psout.Color.w = 0.0;
#			endif
#		else

	psout.Color.w = input.Color.w * baseColor.w;
	psout.Color.xyz = Color::Sky(input.Color.xyz) * baseColor.xyz + skyScale;

#			if defined(CLOUD_SELF_SHADOW)
	bool selfShadowFromENB = false;
#				if defined(EFFECTS11)
	// ENB's cloud scattering already marches the same cubemap for its own shading.
	selfShadowFromENB = SharedData::enbSettings.Enable && SharedData::enbSettings.EnableCloudsScattering;
#				endif
	if (!selfShadowFromENB && SharedData::cloudShadowsSettings.SelfShadowStrength > 0.0) {
		psout.Color.xyz *= CloudShadows::GetCloudSelfShadow(
			normalize(input.WorldPosition.xyz),
			SharedData::DirLightDirection.xyz,
			SharedData::cloudShadowsSettings.SelfShadowStrength,
			SampBaseSampler);
	}
#			endif

	// Kept alongside the self shadow above rather than in place of it: that one needs
	// CLOUD_SELF_SHADOW and this one needs !CLOUDS, so no permutation compiles both.
#			if defined(PROCEDURAL_SUN) && defined(TEX) && defined(DEFERRED) && !defined(DITHER) && !defined(CLOUDS) && !defined(MOONMASK)
	[branch] if (proceduralSunActive)
		psout.Color = ProceduralSun::ToAdditiveBlend(psout.Color, SharedData::proceduralSunSettings.radianceLimit);
#			endif

#			if defined(CLOUDS) && defined(EFFECTS11)
	[branch] if (SharedData::enbSettings.Enable)
	{
		float3 viewDirection = normalize(input.WorldPosition.xyz);
		float cloudTextureAlpha = saturate(baseColor.w);
		float cloudTextureGray = pow(max(dot(baseColor.xyz, 1.0 / 3.0), 0.0), SharedData::enbSettings.CloudsCurve);

		float3 cloudColor = pow(max(Color::Sky(input.Color.xyz) * baseColor.xyz, 0.0), SharedData::enbSettings.CloudsCurve);
		cloudColor = lerp(cloudColor, dot(cloudColor, 1.0 / 3.0), SharedData::enbSettings.CloudsDesaturation) * SharedData::enbSettings.CloudsIntensity * SharedData::enbSettings.CloudsColorFilter;

		bool cloudScattering = SharedData::enbSettings.EnableCloudsScattering && !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::NoSkyScattering);
		psout.Color.xyz = SkyScattering::ShadeCloud(cloudColor, cloudTextureAlpha, cloudTextureGray, viewDirection, cloudScattering, SampBaseSampler) + skyScale * min(SharedData::enbSettings.CloudsIntensity, 1.0);
		psout.Color.w = saturate(input.Color.w * baseColor.w * (1.0 + baseColor.w * SharedData::enbSettings.CloudsVertexAlphaBoost));
	}
#			endif
#		endif

#		if defined(EFFECTS11_CELESTIAL_EXTINCTION)
	[branch] if (SharedData::enbSettings.Enable && SharedData::enbSettings.EnableCloudsScattering && !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun))
		psout.Color *= SkyScattering::GetCelestialExtinction(normalize(input.WorldPosition.xyz));
#		endif

#	else
	psout.Color = float4(0, 0, 0, 1.0);
#	endif  // OCCLUSION

	const bool inReflection = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection) != 0;
#	if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
#		if defined(DITHER) && !defined(TEX)
		// SKY
		float3 skyViewDir = normalize(input.WorldPosition.xyz);
		float skyShadow = GetPhysSkyCloudShadow(skyViewDir, input.Position.xy);
		float2 physSkyScreenUV = input.Position.xy * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;  // adjust for dynamic res
		float3 physSkyColor = PhysSky::SampleSky(skyViewDir, skyShadow, PhysSky::SampSv);
#			if defined(NIGHT_SKY)
		// Behind the volumetric clouds composited next, like the moons and clouds that cover it.
		physSkyColor += nightSkyRadiance;
#			endif
		float3 skyColor = physSkyColor;
		if (SharedData::physSkyData.enableVolumetricClouds && (inReflection || !SharedData::PostWaterComposite))
			skyColor = inReflection ? PhysSky::CompositeVolumetricCloudsCube(physSkyColor, skyViewDir, PhysSky::SampSv) : PhysSky::CompositeVolumetricCloudsUv(physSkyColor, physSkyScreenUV, PhysSky::SampSv);
		psout.Color = lerp(float4(skyColor, 1.0f), psout.Color, SharedData::physSkyData.vanillaMix);

#		elif defined(PS_CLOUDS)
		float apShadow = GetPhysSkyCloudShadow(viewDir, input.Position.xy);
		float4 apColor = PhysSky::SampleAp(viewDir, psCloudDist, apShadow, PhysSky::SampSv);
		psout.Color.xyz = psout.Color.xyz * apColor.a + apColor.rgb;
#		elif defined(TEX) && defined(DEFERRED)
		if (SharedData::physSkyData.enableVolumetricClouds && (inReflection || !SharedData::PostWaterComposite)) {
			float2 physSkyScreenUV = input.Position.xy * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;  // adjust for dynamic res
			psout.Color.xyz = PhysSky::ApplyVolumetricCloudTransmittanceUv(psout.Color.xyz, physSkyScreenUV, PhysSky::SampSv);
		}
#		else
		if (SharedData::physSkyData.enableVolumetricClouds && (inReflection || !SharedData::PostWaterComposite)) {
			float2 physSkyScreenUV = input.Position.xy * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;  // adjust for dynamic res
			float3 physSkyViewDir = normalize(input.WorldPosition.xyz);
			psout.Color.xyz = inReflection ? PhysSky::CompositeVolumetricCloudsCube(psout.Color.xyz, physSkyViewDir, PhysSky::SampSv) : PhysSky::CompositeVolumetricCloudsUv(psout.Color.xyz, physSkyScreenUV, PhysSky::SampSv);
		}
#		endif
	}
#	endif

#	if defined(EXP_HEIGHT_FOG)
	// Reflected skies follow the same weather-fog exclusion as the main-view composite.
	if (inReflection && SharedData::exponentialHeightFogSettings.enabled && SharedData::exponentialHeightFogSettings.useVanillaFogSettings == 0) {
		float skyFogDistance = SharedData::CameraData.x;
#		if defined(HORIZON_FIX)
		// Match the main view (ISSAOComposite.hlsl): fog the sky out to the HorizonFix far water's horizon
		skyFogDistance = max(skyFogDistance, SharedData::horizonFixSettings.farWaterDistance);
#		endif
		float3 skyFogPosition = normalize(input.FogPosition.xyz) * skyFogDistance;
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFogNoVolumetric(skyFogPosition, FrameBuffer::CameraPosAdjust.xyz, psout.Color.xyz, float4(input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy, input.Position.z, 1));
		psout.Color.xyz = lerp(psout.Color.xyz, exponentialHeightFog.xyz, exponentialHeightFog.w);
	}
#	endif

	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);

	psout.MotionVectors = float4(screenMotionVector, 0, psout.Color.w);
	psout.Normal = float4(0.5, 0.5, 0, psout.Color.w);

#	if defined(CLOUD_SHADOWS) && defined(CLOUDS) && !defined(DEFERRED)
	// A broadcast alpha would square coverage under SRC_ALPHA blending.
	psout.CloudShadows = float4(1, 1, 1, psout.Color.w);

	// Keep sun behind scene depth to prevent halo leaks through geometry.
	float depth = TexDepthSampler.Load(int3(input.Position.xy, 0));
#		ifdef REVERSE_Z
	if (depth > 0.0 && depth < 1.0 && SharedData::GetScreenDepth(depth) < SharedData::GetScreenDepth(input.Position.z) * 0.99)
#		else
	if (depth < input.Position.z)
#		endif
		psout.Color.w = 0;

#	elif !defined(DITHER) || !defined(TEX)
	// Even without cloud shadows enabled, sun disc should be occluded by scene depth (clouds, terrain, etc.)
	// The sun glare pass (DITHER + TEX) is skipped: it fades by depth coverage in the VS instead,
	// and the per-pixel reject made the glare disappear.
	[branch] if ((Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsSun) && !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection) && psout.Color.w > 0.0) {
		float depth = TexDepthSampler.Load(int3(input.Position.xy, 0));
#		ifdef REVERSE_Z
		if (depth > 0.0 && depth < 1.0 && SharedData::GetScreenDepth(depth) < SharedData::GetScreenDepth(input.Position.z) * 0.99)
#		else
		if (depth < input.Position.z)
#		endif
			psout.Color.w = 0;
	}
#	endif

	return psout;
}
#endif
