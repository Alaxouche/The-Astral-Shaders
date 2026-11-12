// Interior eye adaptation, run in place of the preset's enbadaptation.fx Draw pass while the Effects11 "UseInteriorValues"
// setting is on and the player is indoors. Same formula as ENB's reference shader, with the interior values, so the
// interior can be tuned in the Effects11 editor whatever the preset's own shader does (NAT ignores enbseries.ini there).

Texture2D<float> TextureCurrent : register(t0);   // 16x16, the preset's Downsample pass
Texture2D<float> TexturePrevious : register(t1);  // 1x1, last frame's adaptation

cbuffer AdaptationParams : register(b0)
{
	float4 Parameters;  // x = min, y = max, z = sensitivity, w = time factor (elapsed / adaptation time, saturated)
	float4 Options;     // x = 1 to clamp to min/max
};

struct PS_INPUT
{
	float4 pos : SV_POSITION;
	float2 txcoord0 : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target
{
	float previous = TexturePrevious.Load(int3(0, 0, 0));

	float average = 0.0;
	float brightest = 0.0;
	[unroll] for (int y = 0; y < 16; y++)
	{
		[unroll] for (int x = 0; x < 16; x++)
		{
			float value = TextureCurrent.Load(int3(x, y, 0));
			average += value;
			brightest = max(brightest, value);
		}
	}
	average *= 1.0 / 256.0;

	// Sensitivity weighs small bright areas (a fire, a lamp) against the average of the frame
	float target = lerp(average, brightest, Parameters.z);
	float adapted = lerp(previous, target, Parameters.w);
	adapted = clamp(adapted, 0.001, 16384.0);
	if (Options.x > 0.5)
		adapted = clamp(adapted, Parameters.x, Parameters.y);

	return float4(adapted, 0.0, 0.0, 1.0);
}
