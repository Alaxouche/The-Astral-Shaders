#ifndef __FUR_SHELLS_HLSLI__
#define __FUR_SHELLS_HLSLI__

namespace FurShells
{
	cbuffer FurShellData : register(b13)
	{
		float Length;
		float ShellCount;
		float Droop;
		float RootThreshold;
		float TipThreshold;
		float RootDarkening;
		float ShellColor;
		float UVScale;
	};

#if defined(PSHADER)
	Texture2D<float4> TexShell : register(t122);
#elif defined(VSHADER) && defined(MODELSPACENORMALS)
	Texture2D<float4> TexModelSpaceNormal : register(t122);

	/**
	 * Model-space normal (xzy-packed, as Lighting.hlsl decodes it) at a mip near 256 texels, so
	 * neighbouring vertices agree on a smooth direction instead of the map's fine detail.
	 */
	float3 GetModelSpaceNormal(float2 uv)
	{
		uint width, height, levels;
		TexModelSpaceNormal.GetDimensions(0, width, height, levels);
		if (width == 0 || levels == 0)
			return 0.0.xxx;
		uint mip = min(levels - 1, (uint)max(0.0, log2(float(max(width, height)) / 256.0)));
		uint mipWidth = max(width >> mip, 1u);
		uint mipHeight = max(height >> mip, 1u);
		int2 texel = int2(frac(uv) * float2(mipWidth, mipHeight));
		float3 packed = TexModelSpaceNormal.Load(int3(texel, mip)).xzy;
		float3 normal = packed * 2.0 - 1.0;
		float len = length(normal);
		return len > 1e-3 ? normal / len : 0.0.xxx;
	}
#endif

	float GetShell(uint instanceID)
	{
		return saturate((ShellCount - float(instanceID)) / max(ShellCount, 1.0));
	}
}

#endif  // __FUR_SHELLS_HLSLI__
