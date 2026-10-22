// Caustic Light: water light thrown onto the scene, computed per pixel from the G-buffer.
//
// Based on CausticLight by qtuna (Iwillbewarlock, GPL-3.0, https://github.com/Iwillbewarlock/CausticLight): its
// pattern atlases, the mirror-image reflection model, the glossy spread, the pool floor focus and the fitted
// constants. The original is a standalone plugin that can only see the finished frame, so it estimated each surface's
// existing light and multiplied the scene by a share of it. Here the pass runs inside the deferred pipeline: every
// light comes from Light Limit Fix's clusters (no three-light cap, no CPU ranking), the sun from the shared data with
// its own shadow map at the water, the surface normal from the G-buffer, and the result is irradiance that the
// composite multiplies by the real albedo. Water comes from the engine's water objects (no Havok rays).
//
// Output: irradiance in the units of Lighting.hlsl's diffuse light, added by DeferredCompositeCS (t22).

#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"

#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> DepthTexture : register(t0);
#endif
Texture2D<unorm float3> NormalRoughnessTexture : register(t1);
Texture2D<float4> WaterMask : register(t2);  // one channel per water level: the share of the texel that is open water
Texture2D<float4> ReflCalm : register(t3);   // walls: RGB = the reflected pattern on screens 0.5 / 1.5 / 4 m above the water
Texture2D<float4> ReflNormal : register(t4);
Texture2D<float4> ReflRough : register(t5);
Texture2D<float4> FloorCalm : register(t6);  // pool floors: A = the refracted pattern
Texture2D<float4> FloorNormal : register(t7);
Texture2D<float4> FloorRough : register(t8);
Texture2DArray<float4> DirectionalShadowMap : register(t9);

struct DirectionalShadowLightData
{
	column_major float4x4 ShadowProj[2];
	column_major float4x4 InvShadowProj[2];
	float2 EndSplitDistances;
	float2 StartSplitDistances;
};
StructuredBuffer<DirectionalShadowLightData> DirectionalShadowLights : register(t98);

SamplerState LinearSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

RWTexture2D<float4> CausticRW : register(u0);

cbuffer CausticCB : register(b0)
{
	float4 Levels[4];       // x: water height (absolute), y: reach above it, z: deepest lit floor below it, w: 1 = used
	float4 LevelBounds[4];  // absolute xy bounds of the level's open water (min xy, max xy)
	float4 Mask;            // xy: absolute xy of the mask's corner, z: 1 / extent, w: texel size
	float4 Pattern;         // x: frame clock 0..64, y: reflection spread tan(2 sigma), z: ripple contrast, w: floor strength
	float4 Layers;          // xyz: calm / normal / rough weights (sum 1), w: strength
	float4 Ripple;          // xy: absolute position of the swimmer, z: amplitude 0..1, w: ring radius
	float4 Sun;             // x: share of the directional light that is a beam (0 = off), y: 1 = sun on pool floors
	float4 Params;          // x: camera distance where the effect is gone, y: lights per pixel, z: occlusion test, w: debug view
	float4 Params2;         // x: local lights, y: pool floors, z: floor absorption e-fold, w: frame index
};

#if defined(LIGHT_LIMIT_FIX)
#	include "LightLimitFix/LightLimitFix.hlsli"
#endif

namespace Caustic
{
	static const float Atlas = 2048, Cell = 256, Gutter = 4, Content = 248, Frames = 64;
	static const float Tile = 168;  // world units per pattern tile
	// Stored means of the reflection atlases' channels (tools/build-all.ps1 of the original): channel / mean = light.
	static const float3 ReflMeanCalm = float3(0.4184, 0.0765, 0.0713);
	static const float3 ReflMeanNormal = float3(0.0790, 0.0867, 0.1604);
	static const float3 ReflMeanRough = float3(0.2758, 0.5436, 0.7001);
	static const float3 ReflDist = float3(35, 105, 280);  // the three screens' distance from the water (world units)
	static const float LampRadius = 5, SunHalfAngle = 0.00465;
	static const float TexelWorld = 168.0 / 248.0;
	static const float PatternMean = 0.1401;  // the calm floor atlas's mean
	static const float SunReflectScale = 1.07;
	static const float FloorSpreadFactor = (1 - 1 / 1.33) / 2;
	static const float BottomContrast = 3.2, BottomMaxContrast = 0.5;
	static const int OcclusionSteps = 6;
	static const float OcclusionBias = 8, OcclusionThickness = 150, OcclusionNear = 20;

	float2 CellUV(float2 q, float frame)
	{
		float2 cell = float2(fmod(frame, 8), floor(frame / 8));
		return (cell * Cell + Gutter + frac(q) * Content) / Atlas;
	}

	// How the three reflection screens mix for a wall this far from the water: linear in log distance between the
	// screens; nearer than the first the pattern fades to flat (w).
	float4 ReflWeights(float d)
	{
		float4 wt = 0;
		[flatten] if (d <= ReflDist.x)
		{
			float t = saturate(d / ReflDist.x);
			wt = float4(t, 0, 0, 1 - t);
		}
		else if (d <= ReflDist.y)
		{
			float t = saturate(log2(d / ReflDist.x) / log2(ReflDist.y / ReflDist.x));
			wt = float4(1 - t, t, 0, 0);
		}
		else
		{
			float t = saturate(log2(d / ReflDist.y) / log2(ReflDist.z / ReflDist.y));
			wt = float4(0, 1 - t, t, 0);
		}
		return wt;
	}

	// One layer's deviation from its mean at this wall distance, variance-preserving across the screens.
	float ReflDeviation(Texture2D<float4> tex, float2 q, float frame, float lod, float4 wt, float3 mean)
	{
		float f0 = floor(frame);
		float4 a = tex.SampleLevel(LinearSampler, CellUV(q, f0), lod);
		float4 b = tex.SampleLevel(LinearSampler, CellUV(q, fmod(f0 + 1, Frames)), lod);
		float4 s = lerp(a, b, frame - f0);
		float norm = sqrt(max(dot(wt.xyz, wt.xyz), 1e-6));
		return dot(wt.xyz, s.xyz / mean - 1) / norm * (1 - wt.w);
	}

	// The walls' pattern as light (mean 1): the agitation's layers, each at the wall's distance.
	float ReflMix(float2 q, float frame, float lod, float dist, float contrast)
	{
		float4 wt = ReflWeights(dist);
		float dev = 0;
		[branch] if (Layers.x > 0.01) dev += Layers.x * ReflDeviation(ReflCalm, q, frame, lod, wt, ReflMeanCalm);
		[branch] if (Layers.y > 0.01) dev += Layers.y * ReflDeviation(ReflNormal, q, frame, lod, wt, ReflMeanNormal);
		[branch] if (Layers.z > 0.01) dev += Layers.z * ReflDeviation(ReflRough, q, frame, lod, wt, ReflMeanRough);
		return max(1 + contrast * dev / sqrt(max(dot(Layers.xyz, Layers.xyz), 1e-6)), 0);
	}

	float FloorPattern(Texture2D<float4> tex, float2 q, float frame, float lod)
	{
		float f0 = floor(frame);
		float a = tex.SampleLevel(LinearSampler, CellUV(q, f0), lod).w;
		float b = tex.SampleLevel(LinearSampler, CellUV(q, fmod(f0 + 1, Frames)), lod).w;
		return lerp(a, b, frame - f0);
	}

	// The floor atlases crossfaded, each rescaled to the calm atlas's mean.
	float FloorMix(float2 q, float frame, float lod)
	{
		float a = 0;
		[branch] if (Layers.x > 0.01) a += Layers.x * FloorPattern(FloorCalm, q, frame, lod);
		[branch] if (Layers.y > 0.01) a += Layers.y * FloorPattern(FloorNormal, q, frame, lod) * (0.1401 / 0.1562);
		[branch] if (Layers.z > 0.01) a += Layers.z * FloorPattern(FloorRough, q, frame, lod) * (0.1401 / 0.1695);
		return a;
	}

	// Share of open water of level `level` around absolute xy `w`, averaged over a disc of `radius`.
	float WaterCover(uint level, float2 w, float radius)
	{
		float2 uv = (w - Mask.xy) * Mask.z;
		float lod = log2(max(radius / Mask.w, 1));
		float cover = WaterMask.SampleLevel(LinearSampler, saturate(uv), lod)[level];
		// Past the mask the clamped sample is an edge average: fade it over the spread.
		float2 outside = max(max(-uv, uv - 1), 0) / Mask.z;
		return cover * saturate(1 - length(outside) / max(radius, Mask.w));
	}

	// Screen-space test of the straight path between two camera-relative points: a sample hidden behind the scene by
	// more than the bias and less than the thickness is blocked. Off-screen samples count as clear.
	float PathVisible(float3 fromRel, float3 toRel)
	{
		[loop] for (int k = 1; k <= OcclusionSteps; k++)
		{
			float f = 0.05 + 0.9 * k / (OcclusionSteps + 1);
			float3 x = lerp(toRel, fromRel, f);
			float4 clip = mul(FrameBuffer::CameraViewProj, float4(x, 1));
			[branch] if (clip.w <= OcclusionNear)
				continue;
			float2 ndc = clip.xy / clip.w;
			[branch] if (any(abs(ndc) >= 1))
				continue;
			float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
			int2 pixel = int2(uv / FrameBuffer::DynamicResolutionParams2.xy * SharedData::BufferDim.xy);
			float sceneDepth = SharedData::GetScreenDepth(DepthTexture.Load(int3(pixel, 0)));
			[branch] if (sceneDepth > OcclusionNear && sceneDepth < clip.w - OcclusionBias && sceneDepth > clip.w - OcclusionThickness)
				return 0;
		}
		return 1;
	}

	float SampleDirectionalShadow(float3 positionRel)
	{
		DirectionalShadowLightData data = DirectionalShadowLights[0];
		float shadowDepth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(positionRel));
		uint cascade = shadowDepth > data.EndSplitDistances.x ? 1 : 0;
		float3 positionLS = mul(data.ShadowProj[cascade], float4(positionRel + FrameBuffer::CameraPosAdjust.xyz, 1)).xyz;
		uint width, height, slices;
		DirectionalShadowMap.GetDimensions(width, height, slices);
		float shadow = 1;
		[branch] if (shadowDepth < data.EndSplitDistances.y && all(positionLS.xy >= 0) && all(positionLS.xy <= 1) && cascade < slices)
		{
			float2 texel = rcp(float2(max(width, 1), max(height, 1)));
			float compare = positionLS.z - 0.002;
			shadow = DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy, cascade), compare).x * 2;
			shadow += DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy + float2(texel.x, texel.y), cascade), compare).x;
			shadow += DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy - float2(texel.x, texel.y), cascade), compare).x;
			shadow *= 0.25;
		}
		return shadow;
	}

	// Rings around a swimmer: the pattern is pushed outward and brightens on the wave crests.
	float2 RippleOffset(float2 w, out float boost)
	{
		float2 rip = w - Ripple.xy;
		float rr = length(rip);
		float ramp = saturate(Ripple.z) * exp(-rr / max(Ripple.w, 1));
		float wave = sin(6.2831853 * (rr / 28 - Pattern.x / 8 * 1.6));
		boost = 1 + 0.6 * ramp * wave;
		return rip / max(rr, 1e-3) * (wave * ramp * 0.05);
	}

	struct Surface
	{
		float3 rel;     // camera-relative position
		float3 abs;     // absolute position
		float3 normal;  // world normal
		float camLod;   // atlas detail lost to distance
	};

	// Light from one source reflected off level `level` onto the surface. `toward` is the source's camera-relative
	// position, or for the sun the unit vector toward it (`sun`). Returns 0 when the reflection misses the water.
	float3 Reflect(Surface s, uint level, float waterRel, float heightFade, float3 toward, bool sun, float3 lightColor,
		inout float3 occlusionFrom, inout float occlusionWeight)
	{
		float3 dir;
		float travel;
		float2 w;
		float3 toP;
		[branch] if (sun)
		{
			dir = float3(-toward.xy, toward.z);  // parallel rays reflected off the plane, travelling upward
			w = s.rel.xy - dir.xy * ((s.rel.z - waterRel) / max(dir.z, 1e-3));
			travel = 1e6;
			toP = dir;
		}
		else
		{
			float3 mirror = float3(toward.xy, 2 * waterRel - toward.z);  // the source's image below the water
			toP = s.rel - mirror;
			travel = length(toP);
			dir = toP / max(travel, 1e-3);
			w = mirror.xy + toP.xy * ((waterRel - mirror.z) / max(s.rel.z - mirror.z, 1e-3));
		}
		float ndl = saturate(dot(s.normal, -dir));
		float3 result = 0;
		[branch] if (dir.z > 1e-3 && ndl > 0)
		{
			float3 water = float3(w, waterRel);
			float lwp = length(s.rel - water);
			float2 wAbs = w + FrameBuffer::CameraPosAdjust.xy;
			float cover = WaterCover(level, wAbs, Pattern.y * lwp);
			float m = saturate(1 - dir.z);
			float fresnel = 0.02 + 0.98 * (m * m) * (m * m) * m;  // Schlick, water F0 0.02
			float3 light = lightColor * (fresnel * ndl * cover * heightFade);
			float weight = max(light.r, max(light.g, light.b));
			[branch] if (cover > 0.002 && weight * Layers.w >= 1e-4)
			{
				float rippleBoost;
				float2 q = wAbs / Tile + RippleOffset(wAbs, rippleBoost);
				float dlw = max(travel - lwp, 1);                  // the source's image to the water point (the sun: infinite)
				float leff = sun ? lwp : lwp * dlw / (lwp + dlw);  // a diverging beam focuses farther away
				float blurR = lwp * (sun ? SunHalfAngle : LampRadius / dlw);
				float lod = clamp(s.camLod + log2(1 + blurR / TexelWorld), 0, 5);
				result = light * ReflMix(q, Pattern.x, lod, leff, Pattern.z) * rippleBoost * (sun ? SunReflectScale : 1);

				// The path from the water up to the surface is tested once per pixel, toward the strongest reflection.
				[flatten] if (weight > occlusionWeight)
				{
					occlusionWeight = weight;
					occlusionFrom = water;
				}
			}
		}
		return result;
	}

	// A source's light refracted through the rippled surface onto a floor below it, as the pattern's bright streaks
	// around the light the floor already gets (the darker gaps between them are left to the scene).
	float3 Refract(Surface s, uint level, float waterRel, float3 toward, bool sun, float3 lightColor)
	{
		float depth = waterRel - s.rel.z;
		static const float eta = 1 / 1.33;
		float2 w = s.rel.xy;
		float ci = 1;
		float3 toL;
		[branch] if (sun)
		{
			float3 d = -toward;
			ci = saturate(-d.z);
			float k2 = max(1 - eta * eta * (1 - ci * ci), 0);
			float tz = eta * d.z + (eta * ci - sqrt(k2));
			w = s.rel.xy - eta * d.xy * (depth / max(-tz, 1e-3));
			toL = toward;
		}
		else
		{
			[unroll] for (int it = 0; it < 2; it++)
			{
				float3 d = normalize(float3(w, waterRel) - toward);
				ci = saturate(-d.z);
				float k2 = max(1 - eta * eta * (1 - ci * ci), 0);
				float tz = eta * d.z + (eta * ci - sqrt(k2));
				w = s.rel.xy - eta * d.xy * (depth / max(-tz, 1e-3));
			}
			toL = normalize(toward - s.rel);
		}
		float ndl = saturate(dot(s.normal, toL));
		float m = saturate(1 - ci);
		float transmit = 1 - (0.02 + 0.98 * (m * m) * (m * m) * m);
		float2 wAbs = w + FrameBuffer::CameraPosAdjust.xy;
		float cover = ndl > 0 ? WaterCover(level, wAbs, max(depth * Pattern.y * FloorSpreadFactor, Mask.w)) : 0;

		float3 result = 0;
		[branch] if (cover > 0.002)
		{
			// The ripples as lenses: depth / focal length sets how sharp and how strong the pattern is. A lamp's
			// diverging light focuses as if the floor were d n D / (d + n D) deep (thin lens; the sun: d).
			float focus = 0.641 * Tile / max(tan(0.5 * atan(max(Pattern.y, 0))), 1e-3);
			float nD = sun ? 0 : 1.33 * length(float3(w, waterRel) - toward);
			float dEff = sun ? depth : depth * nD / max(depth + nD, 1e-3);
			float xl = saturate(dEff / focus);
			float contrast = min(BottomContrast * xl, BottomMaxContrast);
			float lod = clamp(s.camLod + 4 * (1 - xl), 0, 5);
			float pattern = FloorMix(wAbs / Tile, Pattern.x, lod) / PatternMean;
			float streak = max(1 + Pattern.z * (pattern - 1), 0) - 1;
			float fade = saturate((depth - 2) / 18) * exp(-depth / max(Params2.z, 1));
			result = lightColor * (transmit * ndl * cover * fade * contrast * max(streak, 0) * Pattern.w);
		}
		return result;
	}
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float depth = DepthTexture[dispatchID.xy];
	[branch] if (depth == FrameBuffer::FarPlaneDepth())
	{
		CausticRW[dispatchID.xy] = 0;
		return;
	}

	float2 uv = (dispatchID.xy + 0.5) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
	float4 h = mul(FrameBuffer::CameraViewProjInverse, float4(2 * float2(uv.x, 1 - uv.y) - 1, depth, 1));
	Caustic::Surface s;
	s.rel = h.xyz / h.w;
	s.abs = s.rel + FrameBuffer::CameraPosAdjust.xyz;
	float distance = length(s.rel);
	float fadeEnd = Params.x;
	[branch] if (!all(isfinite(s.rel)) || distance >= fadeEnd)
	{
		CausticRW[dispatchID.xy] = 0;
		return;
	}

	float3 normalVS = GBuffer::DecodeNormal(NormalRoughnessTexture[dispatchID.xy].xy);
	s.normal = normalize(mul(FrameBuffer::CameraViewInverse, float4(normalVS, 0)).xyz);
	s.camLod = log2(max(distance, 1) / 250);

	const uint debugView = (uint)Params.w;
	float3 caustic = 0;
	float3 occlusionFrom = 0;
	float occlusionWeight = 0;

#if defined(LIGHT_LIMIT_FIX)
	uint clusterIndex = 0;
	uint lightCount = 0;
	uint lightOffset = 0;
	[branch] if (Params2.x > 0.5 && LightLimitFix::GetClusterIndex(uv, mul(FrameBuffer::CameraView, float4(s.rel, 1)).z, clusterIndex))
	{
		lightCount = min(LightLimitFix::lightGrid[clusterIndex].lightCount, (uint)Params.y);
		lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
	}
	float shadowNoise = frac(52.9829189 * frac(dot(float2(dispatchID.xy) + Params2.w * 5.588238, float2(0.06711056, 0.00583715))));
#endif

	const bool sunUp = Sun.x > 0 && !SharedData::InInterior && !SharedData::HideSky && SharedData::DirLightDirection.z > 0.02;
	const float3 sunColor = Color::DirectionalLight(SharedData::DirLightColor.xyz) * Sun.x;

	[loop] for (uint level = 0; level < 4; level++)
	{
		float4 L = Levels[level];
		[branch] if (L.w < 0.5)
			continue;
		float height = s.abs.z - L.x;
		float reach = L.y;
		[branch] if (height > reach || height < -L.z)
			continue;
		float4 bounds = LevelBounds[level];
		float margin = max(height, 0) + 64;
		[branch] if (any(s.abs.xy < bounds.xy - margin) || any(s.abs.xy > bounds.zw + margin))
			continue;
		float waterRel = L.x - FrameBuffer::CameraPosAdjust.z;

		[branch] if (debugView == 2)
		{
			caustic += float3(0.05, 0.25, 0.6) * Caustic::WaterCover(level, s.abs.xy, Mask.w);
			continue;
		}

		[branch] if (height >= 2)
		{
			float rise = saturate((height - 2) / 28);
			float heightFade = rise * rise * (3 - 2 * rise) * saturate((reach - height) / (0.2 * reach));
			[branch] if (heightFade <= 0)
				continue;

			[branch] if (sunUp)
			{
				float3 occFrom = occlusionFrom;
				float occWeight = occlusionWeight;
				float3 light = Caustic::Reflect(s, level, waterRel, heightFade, SharedData::DirLightDirection.xyz, true, sunColor, occFrom, occWeight);
				[branch] if (any(light > 0))
				{
					// The sun reaches the water point only if nothing casts a shadow there (a bridge above the river).
					float3 dir = float3(-SharedData::DirLightDirection.xy, SharedData::DirLightDirection.z);
					float2 w = s.rel.xy - dir.xy * ((s.rel.z - waterRel) / max(dir.z, 1e-3));
					light *= Caustic::SampleDirectionalShadow(float3(w, waterRel));
					caustic += light;
					occlusionFrom = occFrom;
					occlusionWeight = occWeight;
				}
			}

#if defined(LIGHT_LIMIT_FIX)
			[loop] for (uint i = 0; i < lightCount; i++)
			{
				LightLimitFix::Light light = LightLimitFix::lights[LightLimitFix::lightList[lightOffset + i]];
				[branch] if (light.lightFlags & LightLimitFix::LightFlags::Disabled)
					continue;
				float3 source = light.positionWS.xyz;
				[branch] if (source.z <= waterRel + 1)
					continue;  // a light at or below the water reflects nothing upward
				float3 mirror = float3(source.xy, 2 * waterRel - source.z);
				float attenuation = LightLimitFix::GetAttenuation(length(s.rel - mirror), light);
				[branch] if (attenuation < 1e-4)
					continue;
				float3 color = Color::PointLight(light.color.xyz, light.lightFlags & LightLimitFix::LightFlags::Linear) * (attenuation * light.fade);
				float3 occFrom = occlusionFrom;
				float occWeight = occlusionWeight;
				float3 reflected = Caustic::Reflect(s, level, waterRel, heightFade, source, false, color, occFrom, occWeight);
				[branch] if (!any(reflected > 0))
					continue;
				[branch] if (light.lightFlags & LightLimitFix::LightFlags::LocalShadow)
				{
					// The lamp reaches the water point only where its own shadow map says so (a lamp behind a wall).
					float2 w = mirror.xy + (s.rel.xy - mirror.xy) * ((waterRel - mirror.z) / max(s.rel.z - mirror.z, 1e-3));
					float3 water = float3(w, waterRel);
					reflected *= LightLimitFix::GetLocalShadow(LinearSampler, light.localShadowIndex, water, FrameBuffer::CameraPosAdjust.xyz, normalize(source - water), false, shadowNoise);
				}
				caustic += reflected;
				occlusionFrom = occFrom;
				occlusionWeight = occWeight;
			}
#endif
		}
		else if (height <= -2 && Params2.y > 0.5)
		{
			[branch] if (sunUp && Sun.y > 0.5)
				caustic += Caustic::Refract(s, level, waterRel, SharedData::DirLightDirection.xyz, true, sunColor) * Caustic::SampleDirectionalShadow(s.rel);

#if defined(LIGHT_LIMIT_FIX)
			[loop] for (uint i = 0; i < lightCount; i++)
			{
				LightLimitFix::Light light = LightLimitFix::lights[LightLimitFix::lightList[lightOffset + i]];
				[branch] if (light.lightFlags & LightLimitFix::LightFlags::Disabled)
					continue;
				float3 source = light.positionWS.xyz;
				[branch] if (source.z <= waterRel + 1)
					continue;
				float attenuation = LightLimitFix::GetAttenuation(length(source - s.rel), light);
				[branch] if (attenuation < 1e-4)
					continue;
				float3 color = Color::PointLight(light.color.xyz, light.lightFlags & LightLimitFix::LightFlags::Linear) * (attenuation * light.fade);
				float3 refracted = Caustic::Refract(s, level, waterRel, source, false, color);
				[branch] if (any(refracted > 0) && (light.lightFlags & LightLimitFix::LightFlags::LocalShadow))
					refracted *= LightLimitFix::GetLocalShadow(LinearSampler, light.localShadowIndex, s.rel, FrameBuffer::CameraPosAdjust.xyz, normalize(source - s.rel), false, shadowNoise);
				caustic += refracted;
			}
#endif
		}
	}

	[branch] if (debugView != 2)
	{
		// Something between the water and this surface (a ledge, the walkway the water runs under) blocks it.
		[branch] if (Params.z > 0.5 && occlusionWeight > 0)
			caustic *= Caustic::PathVisible(occlusionFrom, s.rel);

		float fade = saturate((distance - 0.8 * fadeEnd) / (0.2 * fadeEnd));
		caustic *= Layers.w * (1 - fade * fade * (3 - 2 * fade));
		[branch] if (debugView == 1)
			caustic = float3(0.6, 0.35, 0.1) * saturate(dot(caustic, float3(0.2126, 0.7152, 0.0722)) * 4);
	}

	CausticRW[dispatchID.xy] = float4(all(isfinite(caustic)) ? max(caustic, 0) : 0, 0);
}
