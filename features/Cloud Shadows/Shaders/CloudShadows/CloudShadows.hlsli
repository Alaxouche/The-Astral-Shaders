#ifndef CLOUD_SHADOWS_HLSLI
#define CLOUD_SHADOWS_HLSLI

#include "Common/Game.hlsli"

namespace CloudShadows
{
	TextureCube<float> CloudShadowsTexture : register(t25);
	// Physical Sky's ray shadows sample the cloud occlusion cubemap through this slot. Nothing on
	// this base writes it, so the sample reads 0 and the self-shadow term contributes nothing
	// rather than failing to compile.
	TextureCube<float> CloudSelfShadowTexture : register(t26);

	const static float CloudHeight = (2e3f / GAME_UNIT_TO_M);
	const static float PlanetRadius = (6371e3f / GAME_UNIT_TO_M);
	const static float RcpHPlusR = (1.0 / (CloudHeight + PlanetRadius));

	// Distance from rel_pos along dir to the cloud shell, or -1 when the ray starts outside it.
	// Sky.hlsl uses it to place the cloud deck along a view ray.
	float IntersectCloudDist(float3 rel_pos, float3 dir)
	{
		float r = PlanetRadius;
		float3 p = (rel_pos + float3(0, 0, r)) * RcpHPlusR;
		float dotprod = dot(p, dir);
		float lengthsqr = dot(p, p);
		if (lengthsqr > 1.0)
			return -1.0;

		return (-dotprod + sqrt(dotprod * dotprod - lengthsqr + 1.0)) * (r + CloudHeight);
	}

	float3 GetCloudShadowSampleDir(float3 rel_pos, float3 eye_to_sun)
	{
		float r = PlanetRadius;
		float3 p = (rel_pos + float3(0, 0, r)) * RcpHPlusR;
		float dotprod = dot(p, eye_to_sun);
		float lengthsqr = dot(p, p);
		float t = -dotprod + sqrt(dotprod * dotprod - dot(p, p) + 1);
		float3 v = (p + eye_to_sun * t) * (r + CloudHeight) - float3(0, 0, r);
		return v;
	}

	float GetCloudShadowMult(float3 worldPosition, SamplerState textureSampler)
	{
		float3 cloudSampleDir = GetCloudShadowSampleDir(worldPosition, SharedData::DirLightDirection.xyz).xyz;
		float cloudCubeSample = CloudShadowsTexture.SampleLevel(textureSampler, cloudSampleDir, 0).x;
		return saturate(1.0 - cloudCubeSample * SharedData::cloudShadowsSettings.Opacity);
	}
}

#endif
