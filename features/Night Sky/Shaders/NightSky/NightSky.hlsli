#ifndef __NIGHT_SKY_HLSLI__
#define __NIGHT_SKY_HLSLI__

#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

// Night Sky: a physically inspired star field, the Milky Way, airglow and auroras, all added to the
// sky dome. Radiance is in the same space as the vanilla sky gradient it is added to. World axes are
// Skyrim's: +x east, +y north, +z up.
namespace NightSky
{
	float Hash31(float3 p)
	{
		p = frac(p * 0.1031);
		p += dot(p, p.zyx + 31.32);
		return frac((p.x + p.y) * p.z);
	}

	float4 Hash44(float4 p)
	{
		p = frac(p * float4(0.1031, 0.1030, 0.0973, 0.1099));
		p += dot(p, p.wzxy + 33.33);
		return frac((p.xxyz + p.yzzw) * p.zywx);
	}

	float ValueNoise3(float3 p)
	{
		float3 i = floor(p);
		float3 f = frac(p);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(
			lerp(lerp(Hash31(i), Hash31(i + float3(1, 0, 0)), f.x), lerp(Hash31(i + float3(0, 1, 0)), Hash31(i + float3(1, 1, 0)), f.x), f.y),
			lerp(lerp(Hash31(i + float3(0, 0, 1)), Hash31(i + float3(1, 0, 1)), f.x), lerp(Hash31(i + float3(0, 1, 1)), Hash31(i + float3(1, 1, 1)), f.x), f.y),
			f.z);
	}

	float Fbm3(float3 p, int octaves)
	{
		float sum = 0.0;
		float amplitude = 0.5;
		float norm = 0.0;
		[loop] for (int i = 0; i < octaves; ++i)
		{
			sum += amplitude * ValueNoise3(p);
			norm += amplitude;
			p = p * 2.03 + 17.1;
			amplitude *= 0.5;
		}
		return sum / norm;
	}

	float ValueNoise1(float x)
	{
		float i = floor(x);
		float f = frac(x);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(Hash31(float3(i, 3.7, 1.3)), Hash31(float3(i + 1.0, 3.7, 1.3)), f);
	}

	float3 Rotate(float3 v, float3 axis, float angle)
	{
		float s, c;
		sincos(angle, s, c);
		return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
	}

	/**
	 * World direction to celestial coordinates: z is the celestial pole, which stands at the
	 * observer's latitude above the northern horizon, and the sky turns around it with sidereal time.
	 */
	float3 ToCelestial(float3 worldDir)
	{
		const float latitude = SharedData::nightSkySettings.Latitude;
		float sinLat, cosLat;
		sincos(latitude, sinLat, cosLat);
		const float3 pole = float3(0.0, cosLat, sinLat);
		const float3 east = float3(1.0, 0.0, 0.0);
		const float3 meridian = cross(pole, east);
		float3 turned = Rotate(worldDir, pole, SharedData::nightSkySettings.SiderealAngle);
		return float3(dot(turned, east), dot(turned, meridian), dot(turned, pole));
	}

	/** Blackbody-like tint for a star temperature parameter: 0 cool red dwarf, 1 hot blue giant. */
	float3 StarTint(float t)
	{
		float3 cool = float3(1.0, 0.60, 0.36);
		float3 warm = float3(1.0, 0.86, 0.68);
		float3 white = float3(0.96, 0.97, 1.0);
		float3 hot = float3(0.64, 0.76, 1.0);
		return t < 0.35 ? lerp(cool, warm, t / 0.35) : (t < 0.7 ? lerp(warm, white, (t - 0.35) / 0.35) : lerp(white, hot, (t - 0.7) / 0.3));
	}

	/**
	 * One layer of stars on a cube-mapped grid of cells, each holding at most one star.
	 * Each layer covers its own magnitude range, and within it the counts follow the real sky:
	 * about three times more stars per fainter magnitude (N(<m) grows as 10^(m/2)).
	 * Each star is a point spread over at least half a pixel, keeping its total light constant,
	 * so stars neither shimmer nor change brightness with resolution.
	 */
	float3 StarLayer(float3 c, float pixelAngle, float cellsPerFace, float occupancy, float brightestMagnitude, float faintestMagnitude, float layerSeed, float twinkle)
	{
		float3 a = abs(c);
		float face;
		float2 uv;
		if (a.x >= a.y && a.x >= a.z) {
			face = c.x > 0.0 ? 0.0 : 1.0;
			uv = c.yz / a.x;
		} else if (a.y >= a.z) {
			face = c.y > 0.0 ? 2.0 : 3.0;
			uv = c.xz / a.y;
		} else {
			face = c.z > 0.0 ? 4.0 : 5.0;
			uv = c.xy / a.z;
		}

		const float2 grid = (uv * 0.5 + 0.5) * cellsPerFace;
		const float2 cell = floor(grid);
		// A little wider than a pixel: narrower points are erased by the temporal upscaler's jitter.
		const float sigma = max(pixelAngle * 0.85, 2e-5);
		const float invTwoSigma2 = 1.0 / (2.0 * sigma * sigma);
		const float normalisation = 1.0 / (Math::TAU * sigma * sigma);
		float3 radiance = 0.0;

		[unroll] for (int y = -1; y <= 1; ++y)
		{
			[unroll] for (int x = -1; x <= 1; ++x)
			{
				const float2 neighbour = cell + float2(x, y);
				const float4 h = Hash44(float4(neighbour, face, layerSeed));
				if (h.x > occupancy)
					continue;

				// Rebuild the star's direction from its position on the cube face.
				const float2 starUv = ((neighbour + 0.1 + 0.8 * h.yz) / cellsPerFace) * 2.0 - 1.0;
				float3 starDir;
				if (face < 1.5)
					starDir = float3(face < 0.5 ? 1.0 : -1.0, starUv.x, starUv.y);
				else if (face < 3.5)
					starDir = float3(starUv.x, face < 2.5 ? 1.0 : -1.0, starUv.y);
				else
					starDir = float3(starUv.x, starUv.y, face < 4.5 ? 1.0 : -1.0);
				starDir = normalize(starDir);

				const float3 chord = c - starDir;
				const float distance2 = dot(chord, chord);
				if (distance2 > 36.0 * sigma * sigma)
					continue;

				// Inverse of N(<m) = 10^(m/2), restricted to this layer's range.
				const float4 h2 = Hash44(float4(neighbour.yx, face + 11.0, layerSeed * 1.7));
				const float brightestShare = pow(10.0, 0.5 * (brightestMagnitude - faintestMagnitude));
				const float magnitude = faintestMagnitude + 2.0 * log10(lerp(brightestShare, 1.0, h2.x));
				// The eye compresses the magnitude scale; the raw 2.5x per magnitude hid every faint star.
				const float flux = pow(10.0, -0.28 * magnitude);

				// Scintillation: a random-phase flicker per star, stronger through more air.
				const float flicker = ValueNoise1(SharedData::Timer * (6.0 + 9.0 * h2.y) + h2.z * 97.0);
				const float scintillation = 1.0 + twinkle * (flicker - 0.5) * 1.6;

				radiance += StarTint(pow(h2.w, 0.8)) * (flux * scintillation * exp(-distance2 * invTwoSigma2) * normalisation);
			}
		}
		return radiance;
	}

	float3 Stars(float3 c, float altitudeSin, float pixelAngle)
	{
		const float density = SharedData::nightSkySettings.StarDensity;
		// Starlight crosses more air toward the horizon: dimmer, and twinkling harder.
		const float airmass = 1.0 / max(altitudeSin + 0.025, 0.06);
		const float extinction = exp(-0.22 * (airmass - 1.0));
		const float twinkle = SharedData::nightSkySettings.Twinkle * saturate(0.25 + 0.12 * airmass);

		// Occupancies reproduce the real whole-sky counts: about 370 stars brighter than 3.2,
		// 14 000 more down to the naked-eye limit, and a faint grain of 43 000 below it.
		float3 stars = StarLayer(c, pixelAngle, 40.0, 0.038, -1.5, 3.2, 1.0, twinkle);
		stars += StarLayer(c, pixelAngle, 150.0, 0.105 * density, 3.2, 6.4, 2.0, twinkle);
		stars += StarLayer(c, pixelAngle, 420.0, 0.040 * density, 6.4, 7.6, 3.0, twinkle * 0.5);
		return stars * (SharedData::nightSkySettings.StarBrightness * 9e-6 * extinction);
	}

	/**
	 * The Milky Way: a band along the galactic plane, a bulging bright core toward the galactic
	 * centre, patchy star clouds and dark dust lanes along its middle. The galactic pole and centre
	 * are the real ones in equatorial coordinates.
	 */
	float3 MilkyWay(float3 c, float altitudeSin)
	{
		const float3 galacticPole = float3(-0.8676, -0.1976, 0.4560);
		const float3 galacticCentre = float3(-0.0549, -0.8734, -0.4838);
		const float3 galacticSide = cross(galacticPole, galacticCentre);

		const float b = asin(clamp(dot(c, galacticPole), -1.0, 1.0));
		const float l = atan2(dot(c, galacticSide), dot(c, galacticCentre));

		const float thin = exp(-b * b / (0.13 * 0.13));
		const float thick = exp(-b * b / (0.38 * 0.38));
		const float bulge = exp(-l * l / (0.45 * 0.45) - b * b / (0.22 * 0.22));

		const float clouds = Fbm3(c * 5.0, 4);
		const float grain = Fbm3(c * 23.0 + 5.3, 3);
		const float dust = Fbm3(c * 8.0 + 11.7, 4);
		const float lanes = smoothstep(0.42, 0.72, dust) * exp(-b * b / (0.07 * 0.07)) * (0.45 + 0.55 * saturate(bulge * 2.0 + 0.3));

		float brightness = (thin * (0.35 + 1.1 * clouds) + 0.3 * thick) * (1.0 + 2.8 * bulge) * (0.75 + 0.5 * grain);
		brightness *= 1.0 - 0.88 * lanes;

		const float3 tint = lerp(float3(0.78, 0.85, 1.0), float3(1.0, 0.9, 0.76), saturate(bulge * 1.6 + thin * 0.25));
		const float extinction = exp(-0.3 * (1.0 / max(altitudeSin + 0.03, 0.08) - 1.0));
		return tint * (brightness * extinction * 0.03 * SharedData::nightSkySettings.MilkyWay);
	}

	/** Green oxygen airglow: a faint band some ten degrees above the horizon on dark nights. */
	float3 Airglow(float altitudeSin)
	{
		const float altitude = asin(saturate(altitudeSin));
		const float band = exp(-pow((altitude - 0.16) / 0.13, 2.0));
		return float3(0.42, 1.0, 0.55) * (band * 0.0035 * SharedData::nightSkySettings.Airglow);
	}

	float AuroraFold(float x, float time, float seed)
	{
		// Large meanders, then folds and curls on ever smaller scales, all drifting along the arc, km.
		return 60.0 * sin(x * 0.0019 + time * 0.010 + seed) +
		       44.0 * (ValueNoise1(x * 0.011 - time * 0.035 + seed) - 0.5) +
		       14.0 * (ValueNoise1(x * 0.06 + time * 0.12 + seed * 1.7) - 0.5);
	}

	/**
	 * Auroral curtains: thin vertical sheets of emission running east-west, low over the northern
	 * horizon. Each is found where the view ray crosses it, solved directly rather than ray-marched:
	 * a sheet a few km thick is thinner than any affordable march step, and sampling it produced
	 * moire. The light gathered is the emission at the crossing times the path through the sheet,
	 * its thickness over the angle of incidence, so folds seen edge-on stand out bright as in real
	 * displays. A curtain is brightest along a sharp, rippling lower edge about 100 km up and fades
	 * within some 70 km above it (oxygen green), with a faint red top and a thin magenta hem where
	 * nitrogen emits. Segments of each arc wax and wane independently.
	 */
	float3 Aurora(float3 v)
	{
		const float strength = SharedData::nightSkySettings.AuroraStrength;
		if (strength <= 1e-3 || v.z <= 0.015 || v.y <= 0.02)
			return 0.0;

		static const float kSpan = 220.0;
		const float time = SharedData::Timer;
		const float3 low = SharedData::nightSkySettings.AuroraColorLow;
		const float3 high = SharedData::nightSkySettings.AuroraColorHigh;

		float3 emission = 0.0;
		[unroll] for (int band = 0; band < 3; ++band)
		{
			const float seed = band * 17.3;
			const float north = 260.0 + band * 180.0;

			// The crossing of y = north + fold(x) along the ray, by fixed-point iteration.
			float t = north / v.y;
			[unroll] for (int i = 0; i < 3; ++i)
				t = (north + AuroraFold(v.x * t, time, seed)) / v.y;
			const float x = v.x * t;

			const float base = 100.0 + 7.0 * (ValueNoise1(x * 0.03 + time * 0.05 + seed) - 0.5) * 2.0;
			const float height = (v.z * t - base) / kSpan;
			if (height < 0.0 || height >= 1.0)
				continue;
			// A curtain has no top edge: it thins out into the atmosphere. Ending it at the top of the
			// modelled span drew a straight line across the sky, because that boundary is a plane
			// through the eye and a plane through the eye projects to a line. The last third of the
			// span fades out instead, which is also where the red emission is on its way out.
			const float topFade = smoothstep(1.0, 0.62, height);

			// Path through the sheet: thickness over the cosine to its normal.
			const float foldSlope = (AuroraFold(x + 1.0, time, seed) - AuroraFold(x - 1.0, time, seed)) * 0.5;
			const float3 sheetNormal = normalize(float3(-foldSlope, 1.0, 0.0));
			const float thickness = 5.0 + 4.0 * ValueNoise1(x * 0.01 + seed * 3.1);
			const float path = thickness / max(abs(dot(v, sheetNormal)), 0.12);

			const float activity = 0.2 + 0.8 * smoothstep(0.3, 0.75, ValueNoise1(x * 0.0035 + time * 0.015 + seed * 2.3));
			const float rays = 0.45 + 0.55 * pow(ValueNoise1(x * 0.25 + time * (0.4 + band * 0.1) + seed), 1.5);
			const float curtain = path * activity * rays * topFade * (1.0 - band * 0.25);

			const float lowerEdge = smoothstep(0.0, 0.012, height);
			const float green = lowerEdge * exp(-height / 0.16) * (1.0 + 0.6 * exp(-height / 0.03));
			const float red = 0.3 * exp(-pow((height - 0.45) / 0.25, 2.0));
			const float hem = 0.3 * exp(-height * height / (0.012 * 0.012));
			emission += curtain * (low * green + high * red + float3(0.75, 0.25, 0.85) * hem);
		}

		// Far curtains near the horizon fade into the haze.
		const float haze = saturate(v.z * 8.0);
		return emission * (strength * SharedData::nightSkySettings.AuroraIntensity * haze * 0.005);
	}

	/**
	 * Everything the night sky adds to the dome in this direction.
	 * @param worldDir     normalized view direction, world axes
	 * @param pixelAngle   angular size of a pixel, for star anti-aliasing
	 */
	float3 Radiance(float3 worldDir, float pixelAngle)
	{
		if (SharedData::nightSkySettings.Enabled == 0 || worldDir.z <= -0.02)
			return 0.0;

		const float visibility = SharedData::nightSkySettings.StarVisibility;
		const float altitudeSin = max(worldDir.z, 0.0);
		float3 radiance = Aurora(worldDir);
		[branch] if (visibility > 1e-3)
		{
			const float3 c = ToCelestial(worldDir);
			const float horizon = smoothstep(-0.02, 0.03, worldDir.z);
			radiance += (Stars(c, altitudeSin, pixelAngle) + MilkyWay(c, altitudeSin) + Airglow(altitudeSin) * SharedData::nightSkySettings.Night) * (visibility * horizon);
		}
		return radiance;
	}
}

#endif  // __NIGHT_SKY_HLSLI__
