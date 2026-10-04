namespace TerrainShadows
{
	Texture2D<float2> ShadowHeightTexture : register(t60);
	// t61 through t64 belong to Physical Sky's atmosphere LUTs in this fork, and Lighting,
	// DistantTree and Effect all pull in both headers, so the upstream slots overlap there.
	// t67-t68 are free; LODShadowMap.cpp binds the same numbers (kLODShadowSlot).
	Texture2DArray<float> LODShadowTexture : register(t67);
	Texture2DArray<float> LODShadowPreviousTexture : register(t68);

	// Lowers shadow heights to hide self-shadowing from the coarse heightmap.
	static const float SelfShadowBias = 256.0;

	float2 GetTerrainShadowUV(float2 xy)
	{
		return xy * SharedData::terraOccSettings.Scale.xy + SharedData::terraOccSettings.Offset.xy;
	}

	float GetTerrainZ(float norm_z)
	{
		return lerp(SharedData::terraOccSettings.ZRange.x, SharedData::terraOccSettings.ZRange.y, norm_z) - SelfShadowBias;
	}

	float2 GetTerrainZ(float2 norm_z)
	{
		return float2(GetTerrainZ(norm_z.x), GetTerrainZ(norm_z.y));
	}

	float GetTerrainShadow(const float3 worldPos, SamplerState samp)
	{
		if (!SharedData::terraOccSettings.EnableTerrainShadow)
			return 1.0;
		float2 uv = GetTerrainShadowUV(worldPos.xy);
		if (any(uv < 0.0) || any(uv > 1.0))
			return 1.0;
		float2 shadowHeight = GetTerrainZ(ShadowHeightTexture.SampleLevel(samp, uv, 0));
		// Blurring in z hides the heightmap's xy resolution; capped by the bias so lit flat terrain stays lit.
		float zBlur = min(SharedData::terraOccSettings.ZBlur, SelfShadowBias);
		float lowerHeight = shadowHeight.y - zBlur;
		float transitionHeight = shadowHeight.x + zBlur - lowerHeight;
		if (transitionHeight <= 0.0)
			return worldPos.z >= shadowHeight.x ? 1.0 : 0.0;
		return saturate((worldPos.z - lowerHeight) / transitionHeight);
	}

	float GetLODShadowVisibility(Texture2DArray<float> shadowTexture, const uint capture, const float3 worldPos, SamplerState samp)
	{
		const float4 axisZ = SharedData::terraOccSettings.LODShadowCaptures[capture].AxisZ;
		const float3 lightPos = float3(
			dot(SharedData::terraOccSettings.LODShadowCaptures[capture].AxisX.xyz, worldPos),
			dot(SharedData::terraOccSettings.LODShadowCaptures[capture].AxisY.xyz, worldPos),
			dot(axisZ.xyz, worldPos) + axisZ.w);

		const float resolution = SharedData::terraOccSettings.LODShadowResolution;
		const float innerEdge = 1.0 - 2.0 / resolution;

		[loop] for (uint cascade = 0; cascade < 3; cascade++)
		{
			const float4 mapping = SharedData::terraOccSettings.LODShadowCaptures[capture].Cascades[cascade];
			const float2 uv = float2(lightPos.x * mapping.x + mapping.y, lightPos.y * mapping.z + mapping.w);
			const float edge = max(abs(uv.x * 2.0 - 1.0), abs(uv.y * 2.0 - 1.0));
			if (edge >= innerEdge)
				continue;

			const float2 texel = uv * resolution - 0.5;
			const float2 weight = frac(texel);
			const float4 depths = shadowTexture.GatherRed(samp, float3((floor(texel) + 1.0) / resolution, cascade));
			const float texelDepth = SharedData::terraOccSettings.LODShadowDepthBias[cascade];
			const float slope = max(abs(depths.y - depths.x), abs(depths.z - depths.w)) + max(abs(depths.x - depths.w), abs(depths.y - depths.z));
			const float bias = 0.5 * texelDepth + SharedData::terraOccSettings.LODShadowDepthBias.w + min(slope, 6.0 * texelDepth);
			const float4 lit = step(lightPos.z - bias, depths);
			const float visibility = lerp(lerp(lit.w, lit.z, weight.x), lerp(lit.x, lit.y, weight.x), weight.y);
			const float fade = cascade == 2 ? saturate((innerEdge - edge) * 8.0) : 1.0;
			return lerp(1.0, visibility, fade);
		}
		return 1.0;
	}

	float GetLODShadow(const float3 worldPos, SamplerState samp)
	{
		float strength = SharedData::terraOccSettings.LODShadowStrength;
		if (strength <= 0.0)
			return 1.0;

		// The capture is LOD geometry, coarser than the ground around the camera; it only
		// shadows receivers beyond the start distance, fading in over 4096 units.
		const float4 near = SharedData::terraOccSettings.LODShadowNear;
		strength *= saturate((length(worldPos - near.xyz) - near.w) / 4096.0);
		if (strength <= 0.0)
			return 1.0;

		float visibility = GetLODShadowVisibility(LODShadowTexture, 0, worldPos, samp);
		const float blend = SharedData::terraOccSettings.LODShadowBlend;
		[branch] if (blend < 1.0)
			visibility = lerp(GetLODShadowVisibility(LODShadowPreviousTexture, 1, worldPos, samp), visibility, blend);
		return lerp(1.0, visibility, strength);
	}
}
