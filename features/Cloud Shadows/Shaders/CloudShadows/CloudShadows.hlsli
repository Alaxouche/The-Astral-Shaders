#ifndef CLOUD_SHADOWS_HLSLI
#define CLOUD_SHADOWS_HLSLI

// Redefinable so a pass that already owns these slots can move them: Physical Sky's ShadowAccum
// remaps the shadow slot to t4, and its Volumetrics pass owns t26 for its own history texture.
#ifndef CLOUD_SHADOW_REGISTER
#	define CLOUD_SHADOW_REGISTER t25
#endif

#ifndef CLOUD_SELFSHADOW_REGISTER
#	define CLOUD_SELFSHADOW_REGISTER t26
#endif

#include "Common/Game.hlsli"

namespace CloudShadows
{
	TextureCube<float> CloudShadowsTexture : register(CLOUD_SHADOW_REGISTER);
	// Bound with the decks drawn above the one being drawn while a cloud deck is drawn, and with
	// the cloud occlusion copy for Physical Sky's ray shadows. Declared unconditionally rather than
	// under CLOUDS: Physical Sky samples it from its sky pass, which has PS_SKY_SAMPLERS but no
	// CLOUDS. Nothing else claims the slot, and a pass that does can move it with the macro above.
	TextureCube<float> CloudSelfShadowTexture : register(CLOUD_SELFSHADOW_REGISTER);

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
