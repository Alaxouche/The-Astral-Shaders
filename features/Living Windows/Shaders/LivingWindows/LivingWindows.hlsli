#ifndef __LIVING_WINDOWS_HLSLI__
#define __LIVING_WINDOWS_HLSLI__

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/SharedData.hlsli"

// Living Windows: parallax interiors, curtains, occupants and old-glass optics behind recognized
// window panes. The CPU classifier publishes the draw's window tier and room family in
// ExtraFeatureDescriptor bits 22-28; the artwork atlases are bound once per frame at t104-t109.
//
// Interiors use interior mapping against a single one-point-perspective room image: the view ray
// is traced through a box behind the pane and the hit point is projected back into the image, so
// walls, floor and ceiling parallax correctly and stay seamless across the box edges.
namespace LivingWindows
{
	Texture2D<float4> RoomAtlas : register(t104);
	Texture2D<float4> CurtainAtlas : register(t105);
	Texture2D<float4> OccupantAtlas : register(t106);
	Texture2D<float4> OutdoorDayAtlas : register(t107);
	Texture2D<float4> OutdoorNightAtlas : register(t108);
	Texture2D<float4> GlassGrime : register(t109);

	// The windows of the glass shape being drawn, found in its mesh on the CPU (LivingWindows.cpp):
	// world-space boxes around panes that sit closer together than a mullion, nearest first.
	static const uint kMaxWindowsPerDraw = 16;
	cbuffer LivingWindowList : register(b13)
	{
		uint4 WindowCount;
		float4 WindowCenter[kMaxWindowsPerDraw];
		float4 WindowHalfExtent[kMaxWindowsPerDraw];
	};

	// A room is framed on its window, but never narrower or flatter than this, so a long clerestory
	// strip still shows a room rather than a sliver of floor.
	static const float kMaxRoomAspect = 1.8;

	static const uint kAtlasRoom = 1;
	static const uint kAtlasCurtain = 2;
	static const uint kAtlasOccupant = 4;
	static const uint kAtlasOutdoorDay = 8;
	static const uint kAtlasOutdoorNight = 16;
	static const uint kAtlasGrime = 32;

	// Share of the room image covered by the back wall in the authored rooms.
	static const float kBackWallScale = 0.5;
	// Curtain plane depth, as a fraction of the room depth.
	static const float kCurtainDepth = 0.04;
	// Glow-map aperture search: guide texels per texture edge and steps per direction. Used only
	// on facade shapes, to pick the room cell from a pane's centre rather than from each pixel.
	// About twelve texels across the tile, the resolution PIXL WindowLife settled on: a painted
	// mullion is under one guide texel and is averaged into the glass around it.
	static const float kGuideTexels = 12.0;
	static const int kGuideSteps = 12;

	// Where a material marks its glass.
	static const uint kPaneNone = 0;
	static const uint kPaneGlow = 1;        // bright glow map texels (vanilla glow maps, TruePBR emissive maps)
	static const uint kPaneLayerAlpha = 2;  // low diffuse alpha, where multilayer parallax shows its inner layer

	struct Surface
	{
		float pane;          // glass coverage at this pixel
		float glassWeight;   // pane coverage with distance fade, drives the glass material
		float roomWeight;    // strength of the interior on this pixel
		float roughness;
		float f0;
		float albedoScale;   // diffuse kept on the pane once an interior shows through
		float footprint;     // pixel footprint in room-local units, for atlas mip selection
		float seed;
		uint tier;
		uint family;
		float2 local;        // position on the room's front face, [0,1]^2, x right and y up
		float2 size;         // room front face in world units
		float framedWindow;  // 1 when the room is framed on one window, 0 on a facade grid
		float3 right;
		float3 up;
		float3 normal;       // faces the viewer
		float3 glassNormal;
		float frost;         // ice coverage on the pane, 0-1
		float condensation;  // misted glass seen from inside, 0-1
	};

	struct Interior
	{
		float3 color;   // linear
		float weight;
	};

	bool HasAtlas(uint flag)
	{
		return (SharedData::livingWindowsSettings.AtlasFlags & flag) != 0;
	}

	bool IsCandidate()
	{
		return SharedData::livingWindowsSettings.Enabled != 0 && (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::LivingWindowCandidate) != 0;
	}

	uint DrawTier()
	{
		return (Permutation::ExtraFeatureDescriptor >> Permutation::ExtraFeatureFlags::LivingWindowTierShift) & 3u;
	}

	uint DrawFamily()
	{
		return (Permutation::ExtraFeatureDescriptor >> Permutation::ExtraFeatureFlags::LivingWindowFamilyShift) & 7u;
	}

	float Hash11(float p)
	{
		p = frac(p * 0.1031);
		p *= p + 33.33;
		p *= p + p;
		return frac(p);
	}

	float Hash21(float2 p)
	{
		float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
		p3 += dot(p3, p3.yzx + 33.33);
		return frac((p3.x + p3.y) * p3.z);
	}

	float ValueNoise(float2 p)
	{
		float2 i = floor(p);
		float2 f = frac(p);
		float2 u = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(Hash21(i), Hash21(i + float2(1, 0)), u.x), lerp(Hash21(i + float2(0, 1)), Hash21(i + float2(1, 1)), u.x), u.y);
	}

	float Luma(float3 color)
	{
		return dot(max(color, 0.0), float3(0.2126, 0.7152, 0.0722));
	}

	/**
	 * @brief Rain beading on the outside of a pane, as a height field.
	 *
	 * Drops sit still until they grow enough to break loose, then run down and leave a thinning
	 * trail, so the glass reads as wet rather than as a moving texture. Returns coverage in x and
	 * the surface gradient in yz, which bends what the glass reflects and refracts.
	 */
	float3 RainDroplets(float2 plane, float amount)
	{
		if (amount <= 1e-3)
			return 0.0;

		static const float kCellSize = 9.0;
		float2 grid = plane / kCellSize;
		float3 field = 0.0;

		[unroll] for (int i = 0; i < 2; ++i)
		{
			float2 offsetGrid = grid + float2(i * 0.37, i * 0.71);
			float2 cell = floor(offsetGrid);
			float2 local = frac(offsetGrid);

			[unroll] for (int y = -1; y <= 1; ++y)
			{
				[unroll] for (int x = -1; x <= 1; ++x)
				{
					float2 neighbour = cell + float2(x, y);
					float seed = Hash21(neighbour + i * 37.0);
					// Only a share of the cells carry a drop, and only once it is raining hard enough.
					if (seed > amount * 0.85)
						continue;

					float speed = 0.25 + 0.75 * Hash11(seed * 31.7);
					float fall = frac(SharedData::Timer * speed * 0.35 + seed * 7.3);
					// Drops hang, then release: most of the cycle is spent nearly still.
					float travel = fall * fall * (3.0 - 2.0 * fall);
					float2 centre = float2(x + Hash11(seed * 17.1), y + 1.0 - travel * 2.0);
					float2 delta = local - centre;

					float radius = lerp(0.10, 0.22, Hash11(seed * 53.9));
					float drop = saturate(1.0 - length(delta / float2(radius, radius * 1.25)));
					// The trail left behind a falling drop, fading with distance and speed.
					float trailWidth = radius * 0.45;
					float trail = saturate(1.0 - abs(delta.x) / trailWidth) *
					              saturate(1.0 - saturate(delta.y * 1.4)) * travel * 0.35;

					float height = max(drop * drop, trail);
					if (height <= field.x)
						continue;
					field.x = height;
					field.yz = -delta * height * 6.0;
				}
			}
		}

		return field * amount;
	}

	/**
	 * @brief Ice crystal coverage, growing inward from the edges of the pane.
	 *
	 * Frost nucleates where the glass meets the cold frame, so coverage is strongest at the
	 * border and thins toward the middle of the pane.
	 */
	float Frost(float2 plane, float2 paneLocal, float amount)
	{
		if (amount <= 1e-3)
			return 0.0;

		// Feathery crystals: two noise octaves sharpened into branches.
		float2 p = plane * 0.09;
		float branches = ValueNoise(p * 2.3) * 0.6 + ValueNoise(p * 7.1) * 0.4;
		branches = pow(saturate(branches * 1.35), 2.2);

		// Distance to the nearest pane edge, in pane-local units.
		float edge = min(min(paneLocal.x, 1.0 - paneLocal.x), min(paneLocal.y, 1.0 - paneLocal.y)) * 2.0;
		float reach = lerp(0.05, 1.25, amount);
		float fromEdge = 1.0 - smoothstep(0.0, reach, edge);

		return saturate((fromEdge * 0.75 + amount * 0.45) * (0.45 + branches));
	}

	/**
	 * @brief Misted glass, seen from a warm room through a cold pane.
	 *
	 * The mirror image of Frost: vapour condenses on the middle of the pane, where the glass is
	 * furthest from the frame that conducts the cold away, and the edges stay comparatively clear.
	 * Beads that have run leave narrow vertical tracks behind them.
	 */
	float Condensation(float2 plane, float2 paneLocal, float amount)
	{
		if (amount <= 1e-3)
			return 0.0;

		const float edge = min(min(paneLocal.x, 1.0 - paneLocal.x), min(paneLocal.y, 1.0 - paneLocal.y)) * 2.0;
		const float middle = smoothstep(0.02, 0.5, edge);

		// Far lower frequency than the frost crystals: mist has no structure, only unevenness.
		const float2 p = plane * 0.03;
		const float blotch = ValueNoise(p) * 0.65 + ValueNoise(p * 2.7) * 0.35;
		float mist = saturate(middle * (0.5 + 0.8 * blotch) * amount);

		// A drop that runs clears a line and does not grow back while the glass stays wet.
		const float runnel = ValueNoise(float2(plane.x * 0.75, plane.y * 0.015));
		mist *= 1.0 - 0.6 * smoothstep(0.66, 0.97, runnel) * amount;

		return saturate(mist);
	}

	// Front face of a typical room per family, in world units. From PIXL WindowLife (GPL-3.0); the
	// CPU uses the same table to tell a single window from a facade (LivingWindows.cpp).
	float2 FamilyRoomSize(uint family)
	{
		if (family == 1)
			return float2(132.0, 174.0);  // noble
		if (family == 2)
			return float2(118.0, 138.0);  // Riften
		if (family == 3)
			return float2(104.0, 154.0);  // Windhelm
		if (family == 4)
			return float2(128.0, 128.0);  // Dwemer
		if (family == 5)
			return float2(126.0, 146.0);  // trade
		return float2(110.0, 140.0);      // Nordic, farmhouse
	}

	// One seed per window: the glass shape's centre, rounded to a unit so float noise in the
	// recomputed bounds never flips it, plus the room cell on a facade.
	float WindowSeed(float3 boundCenter, float2 cell)
	{
		float3 anchor = fmod(floor(boundCenter + 0.5), 8192.0);
		return Hash21(anchor.xy * float2(0.7131, 0.5173) + anchor.z * 0.3917 + cell * float2(17.0, 31.0));
	}

	float PickTile(float selector, float a, float b, float c, float d, float e)
	{
		float choice = floor(saturate(selector) * 4.999);
		return choice < 0.5 ? a : (choice < 1.5 ? b : (choice < 2.5 ? c : (choice < 3.5 ? d : e)));
	}

	// Regional room moods follow PIXL's curation of the shared artwork atlas.
	float SelectRoomTile(float seed, uint family)
	{
		float s = Hash11(seed * 173.31 + 7.13);
		if (family == 1)
			return PickTile(s, 1, 2, 7, 10, 15);  // Solitude, castles, nobility
		if (family == 2)
			return PickTile(s, 0, 3, 6, 11, 14);  // Riften timber
		if (family == 3)
			return PickTile(s, 2, 5, 7, 9, 12);  // Windhelm stone
		if (family == 4)
			return PickTile(s, 12, 7, 2, 12, 9);  // Markarth, Dwemer
		if (family == 5)
			return PickTile(s, 1, 3, 4, 5, 14);  // inns, shops
		return PickTile(s, 0, 6, 8, 11, 13);     // Nordic homes
	}

	float SelectCurtainTile(float seed, uint family)
	{
		float s = Hash11(seed * 47.91 + 19.7);
		if (family == 1)
			return PickTile(s, 0, 4, 6, 10, 15);
		if (family == 2)
			return PickTile(s, 3, 7, 9, 13, 14);
		if (family == 3)
			return PickTile(s, 2, 5, 8, 11, 12);
		if (family == 4)
			return PickTile(s, 1, 5, 8, 12, 14);
		return PickTile(s, 7, 9, 11, 13, 15);
	}

	/**
	 * Samples one cell of a 4x4 atlas without neighbours bleeding in at coarse mips.
	 * texelsPerLocal: atlas-cell texels covered by one unit of cellUV per screen pixel.
	 */
	float4 SampleCell(Texture2D<float4> atlas, SamplerState samp, float tile, float2 cellUV, float texelsPerPixel)
	{
		uint width, height, mipCount;
		atlas.GetDimensions(0, width, height, mipCount);
		float cellTexels = width * 0.25;
		float mip = clamp(log2(max(texelsPerPixel * cellTexels, 1.0)), 0.0, min((float)mipCount - 1.0, 6.0));
		float inset = (exp2(mip) + 1.0) / max(cellTexels, 1.0);
		float2 cell = float2(fmod(tile, 4.0), floor(tile / 4.0));
		return atlas.SampleLevel(samp, (cell + clamp(cellUV, inset, 1.0 - inset)) * 0.25, mip);
	}

	// Architectural glass is locally flat even where the surrounding frame is carved or bumpy.
	// Tangent slope says whether this texel is flat, local continuity says whether it sits in a
	// smooth region rather than on a moulding. Adapted from PIXL WindowLife (GPL-3.0). The
	// derivatives come in from the caller: this runs under dynamic branches, where ddx is not
	// defined.
	float PaneNormalEvidence(float4 normalSample, float2 normalDx, float2 normalDy)
	{
		float2 tangentSlope = normalSample.xy * 2.0 - 1.0;
		float flatness = 1.0 - smoothstep(0.20, 0.62, length(tangentSlope));
		float continuity = 1.0 - smoothstep(0.035, 0.24, length(normalDx) + length(normalDy));
		return saturate(flatness * lerp(0.72, 1.0, continuity));
	}

	float MaskValue(float4 texel, uint source)
	{
		return source == kPaneLayerAlpha ? 1.0 - texel.a : Luma(texel.rgb);
	}

	// Soft glass evidence for one texel: bright glow, or low diffuse alpha where multilayer
	// parallax shows its inner layer. Kept inside the authored tile so a wrapping sampler never
	// borrows glass from the opposite edge of a facade atlas.
	float PaneEvidence(Texture2D<float4> glowTex, SamplerState samp, float2 uv, float mip, float2 tile, uint source)
	{
		if (any(abs(floor(uv) - tile) > 0.5))
			return 0.0;
		float value = MaskValue(glowTex.SampleLevel(samp, uv, mip), source);
		return source == kPaneLayerAlpha ? smoothstep(0.35, 0.80, value) : smoothstep(0.028, 0.145, value);
	}

	// Where this pixel is glass. After PIXL WindowLife's pane mask (GPL-3.0), which fixes the two
	// ways the previous one broke up at the rim of every pane:
	//   - it read the full-resolution texel at every distance, so once the pane was minified the
	//     threshold aliased into a speckled, crawling border. Sampling at the pixel's own footprint
	//     mip hands the threshold a pre-filtered value instead;
	//   - flatness only dimmed the result by up to 55 percent and never excluded anything, so
	//     grime painted into the alpha of old-glass textures became a lace of holes around the
	//     edge. Here the normal map is a real gate: frames, mullions and trim are bumpy, glass is
	//     flat. The two-texel erosion still rejects the halo some atlases paint onto frames.
	// This is full resolution on purpose: it is what keeps the room hidden behind the mullions,
	// while the coarse aperture guide below is what lets one room span all of the panes.
	float PaneMask(Texture2D<float4> glowTex, SamplerState samp, float2 uv, uint source,
		float4 normalSample, float2 normalDx, float2 normalDy, float2 duvdx, float2 duvdy)
	{
		uint width, height, mipCount;
		glowTex.GetDimensions(0, width, height, mipCount);
		float2 size = max(float2(width, height), 1.0);
		float footprint = max(length(duvdx * size), length(duvdy * size));
		float mip = clamp(log2(max(footprint, 1.0)), 0.0, max((float)mipCount - 1.0, 0.0));
		float2 tile = floor(uv);
		float2 texel = exp2(mip) / size;

		float center = PaneEvidence(glowTex, samp, uv, mip, tile, source);
		[branch] if (center <= 1e-4)
			return 0.0;
		float core = min(
			min(PaneEvidence(glowTex, samp, uv - float2(texel.x * 2.0, 0.0), mip, tile, source),
				PaneEvidence(glowTex, samp, uv + float2(texel.x * 2.0, 0.0), mip, tile, source)),
			min(PaneEvidence(glowTex, samp, uv - float2(0.0, texel.y * 2.0), mip, tile, source),
				PaneEvidence(glowTex, samp, uv + float2(0.0, texel.y * 2.0), mip, tile, source)));
		float glass = smoothstep(0.22, 0.80, PaneNormalEvidence(normalSample, normalDx, normalDy));
		return saturate(center * smoothstep(0.10, 0.68, core) * glass);
	}

	float Guide(Texture2D<float4> glowTex, SamplerState samp, float2 uv, float mip, float2 tile, uint source)
	{
		// Never let a wrapping sampler join opposite edges of an atlas.
		if (any(abs(floor(uv) - tile) > 0.5))
			return 0.0;
		float value = MaskValue(glowTex.SampleLevel(samp, uv, mip), source);
		return source == kPaneLayerAlpha ? smoothstep(0.3, 0.7, value) : smoothstep(0.004, 0.06, value);
	}

	// Distance from seed to the aperture edge along stepUV. A single dark guide texel is a mullion;
	// two in a row are the frame, so leaded sub-panes resolve to one window.
	float FindEdge(Texture2D<float4> glowTex, SamplerState samp, float2 seed, float2 stepUV, float mip, float2 tile, uint source, out float found)
	{
		float inside = 0.0;
		float outside = kGuideSteps;
		float gap = 0.0;
		found = 0.0;
		[loop] for (int i = 1; i <= kGuideSteps; ++i)
		{
			if (Guide(glowTex, samp, seed + stepUV * i, mip, tile, source) > 0.5) {
				inside = i;
				gap = 0.0;
			} else {
				gap += 1.0;
				if (gap >= 2.0) {
					outside = max(i - 1.0, inside + 0.25);
					found = 1.0;
					break;
				}
			}
		}
		[branch] if (found > 0.5)
		{
			for (int refine = 0; refine < 2; ++refine) {
				float middle = (inside + outside) * 0.5;
				if (Guide(glowTex, samp, seed + stepUV * middle, mip, tile, source) > 0.5)
					inside = middle;
				else
					outside = middle;
			}
		}
		return (inside + outside) * 0.5;
	}

	struct Aperture
	{
		float2 centerUV;
		float2 spanUV;
		float confidence;
	};

	Aperture FindAperture(Texture2D<float4> glowTex, SamplerState samp, float2 uv, uint source)
	{
		Aperture aperture = (Aperture)0;
		uint width, height, mipCount;
		glowTex.GetDimensions(0, width, height, mipCount);
		if (width < 8 || height < 8 || mipCount == 0)
			return aperture;

		float2 size = float2(width, height);
		float mip = clamp(log2(max(max(size.x, size.y) / kGuideTexels, 1.0)), 0.0, (float)mipCount - 1.0);
		float2 stepUV = exp2(mip) / size;
		float2 tile = floor(uv);

		// Start the search from the centre of the guide texel this pixel falls in, never from the
		// pixel itself: walking out from each pixel's own UV reached the edge after a varying whole
		// number of steps, so neighbouring pixels measured slightly different panes. On the guide
		// grid every pixel of a row searches the same absolute texels and bisects the same interval.
		float2 seed = (floor(uv / stepUV) + 0.5) * stepUV;
		float best = Guide(glowTex, samp, seed, mip, tile, source);
		[branch] if (best <= 0.25)
		{
			// Narrow panes vanish at the guide mip; look at the adjacent guide texels only. The
			// candidates stay on the same grid, so the agreement above still holds.
			const float2 gridSeed = seed;
			for (int y = -1; y <= 1; ++y) {
				for (int x = -1; x <= 1; ++x) {
					float2 candidate = gridSeed + float2(x, y) * stepUV;
					float guide = Guide(glowTex, samp, candidate, mip, tile, source) - (abs(x) + abs(y)) * 0.006;
					if (guide > best) {
						best = guide;
						seed = candidate;
					}
				}
			}
		}
		if (best <= 0.25)
			return aperture;

		float foundLeft, foundRight, foundDown, foundUp;
		float left = FindEdge(glowTex, samp, seed, float2(-stepUV.x, 0), mip, tile, source, foundLeft);
		float right = FindEdge(glowTex, samp, seed, float2(stepUV.x, 0), mip, tile, source, foundRight);
		float down = FindEdge(glowTex, samp, seed, float2(0, -stepUV.y), mip, tile, source, foundDown);
		float up = FindEdge(glowTex, samp, seed, float2(0, stepUV.y), mip, tile, source, foundUp);

		aperture.spanUV = float2(left + right, down + up) * stepUV;
		aperture.centerUV = seed + float2(right - left, up - down) * 0.5 * stepUV;
		aperture.confidence = foundLeft * foundRight * foundDown * foundUp;
		return aperture;
	}

	// Fills s in place: early exits return nothing, which keeps fxc from reporting the partially
	// written struct as uninitialized.
	void EvaluateSurfaceInto(inout Surface s, float3 positionCS, float3 viewDirection, float3 vertexNormal, float4 normalSample, float2 uv, float viewDepth,
		Texture2D<float4> glowTex, SamplerState glowSampler, uint paneSource, float2 duvdx, float2 duvdy, float3 dpdx, float3 dpdy,
		float2 normalDx, float2 normalDy)
	{
		if (!IsCandidate() || SharedData::InMapMenu || (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection) != 0)
			return;

		float fade = 1.0 - smoothstep(SharedData::livingWindowsSettings.FadeStart, SharedData::livingWindowsSettings.FadeEnd, abs(viewDepth));
		if (fade <= 1e-4 || paneSource == kPaneNone)
			return;

		float pane = PaneMask(glowTex, glowSampler, uv, paneSource, normalSample, normalDx, normalDy, duvdx, duvdy);
		if (pane <= 1e-4)
			return;

		float3 N = normalize(vertexNormal);
		// Window glass is often double sided or seen from indoors: work with the side facing the viewer.
		N = dot(N, viewDirection) < 0.0 ? -N : N;
		float3 up = float3(0, 0, 1) - N * N.z;
		up = dot(up, up) > 1e-4 ? normalize(up) : float3(0, 1, 0);

		s.pane = pane;
		s.tier = DrawTier();
		s.family = DrawFamily();
		s.normal = N;
		s.up = up;
		s.right = normalize(cross(up, N));
		// Roof lights and awnings keep glass optics only.
		if (abs(N.z) > 0.55)
			s.tier = min(s.tier, 1u);

		// Old glass: grime, rain runs and a gentle waviness that bends reflections.
		float3 absolutePosition = positionCS + FrameBuffer::CameraPosAdjust.xyz;
		static const float2 kGrimeScale = float2(0.0065, 0.0045);
		float2 plane = float2(dot(absolutePosition, s.right), absolutePosition.z);
		float2 grimeUV = plane * kGrimeScale;
		float grime = 0.4;
		[branch] if (HasAtlas(kAtlasGrime))
		{
			float2 gradX = float2(dot(dpdx, s.right), dpdx.z) * kGrimeScale;
			float2 gradY = float2(dot(dpdy, s.right), dpdy.z) * kGrimeScale;
			grime = Luma(GlassGrime.SampleGrad(glowSampler, grimeUV, gradX, gradY).rgb);
		}
		float rainFlow = frac(grimeUV.y * 1.73 - SharedData::Timer * 0.085 + grime * 0.31);
		float rainRun = pow(saturate(1.0 - rainFlow), 12.0) * SharedData::livingWindowsSettings.Raining;
		float grimeStrength = SharedData::livingWindowsSettings.GrimeStrength;
		s.roughness = saturate(SharedData::livingWindowsSettings.GlassRoughness + (grime - 0.35) * grimeStrength * 0.5 + rainRun * 0.12);
		s.f0 = 0.04 * SharedData::livingWindowsSettings.GlassReflectance;
		float2 wave = float2(ValueNoise(plane * 0.031 + 17.3), ValueNoise(plane * 0.027 + 41.9)) - 0.5;

		// Weather on the outside face: beading rain, then ice once it turns to snow.
		float dropletAmount = SharedData::livingWindowsSettings.Raining * SharedData::livingWindowsSettings.DropletStrength;
		float3 droplets = RainDroplets(plane, dropletAmount);
		wave += droplets.yz;
		s.roughness = saturate(s.roughness + droplets.x * 0.18);

		s.glassNormal = normalize(N + (s.right * wave.x + s.up * wave.y) * SharedData::livingWindowsSettings.Waviness * 0.12);
		s.glassWeight = pane * fade * (SharedData::livingWindowsSettings.GlassOptics != 0 ? 1.0 : 0.0);

		if (s.tier < 2 || SharedData::livingWindowsSettings.Interiors == 0)
		{
			// Without an aperture there is no pane-local frame; frost still forms, using the
			// texture coordinates as a stand-in for the pane.
			s.frost = Frost(plane, frac(uv), SharedData::livingWindowsSettings.FrostAmount * SharedData::livingWindowsSettings.FrostStrength) * pane * fade;
			// Not multiplied by FrostAmount the way frost is: that signal is snow falling, and the
			// coldest nights are the clear ones. The plugin folds the cold into CondensationStrength.
			s.condensation = Condensation(plane, frac(uv), SharedData::livingWindowsSettings.CondensationStrength) * pane * fade;
			s.roughness = saturate(s.roughness + s.frost * 0.55 + s.condensation * 0.4);
			return;
		}

		// The room is framed in world space, on the bounds of the glass shape being drawn, never on
		// the texture. Window glass is nearly always an atlas: frame and mullions are geometry, and
		// each pane samples its own rectangle of glass somewhere in the texture, in whatever order
		// and orientation the modeller chose. A frame rebuilt from those UVs described the atlas and
		// not the window, so every pane received a different, displaced and sometimes flipped piece
		// of room, and the coarse search merged neighbouring atlas rectangles into one "window" that
		// existed nowhere on the mesh. The bounding sphere is one fact shared by every pixel of every
		// pane: one room, one seed, and axes that are upright in the world whatever the UV layout.
		// After PIXL WindowLife's geometry framing (GPL-3.0).
		float4 bound = Permutation::LivingWindowBound;
		if (bound.w == 0.0) {
			s.tier = 1;  // no shape bounds: glass optics only
			return;
		}

		// The window this pixel belongs to, when the mesh was readable. A house carries the glass of
		// all its windows in one shape; each window here is its own box, so each gets its own room.
		int window = -1;
		uint windowCount = min(WindowCount.x, kMaxWindowsPerDraw);
		[loop] for (uint w = 0; w < windowCount; ++w)
		{
			float3 outside = abs(absolutePosition - WindowCenter[w].xyz) - WindowHalfExtent[w].xyz;
			if (all(outside <= 4.0)) {
				window = (int)w;
				break;
			}
		}

		float3 anchor = window >= 0 ? WindowCenter[window].xyz : bound.xyz;
		float3 fromCenter = positionCS - (anchor - FrameBuffer::CameraPosAdjust.xyz);
		float2 planeOffset = float2(dot(fromCenter, s.right), dot(fromCenter, s.up));
		float2 reference = FamilyRoomSize(s.family);
		float2 grid;
		float2 cell = 0.0;

		[branch] if (window >= 0)
		{
			// The box's reach along the wall's own axes is the window's width and height, whatever the
			// house's rotation. The room's front face is the window, so the whole room image shows
			// through it at the artwork's full resolution.
			float3 halfExtent = WindowHalfExtent[window].xyz;
			float2 size = max(2.0 * float2(dot(halfExtent, abs(s.right)), dot(halfExtent, abs(s.up))), 16.0);
			size = max(size, size.yx / kMaxRoomAspect);
			s.size = size;
			grid = planeOffset / s.size + 0.5;
			s.framedWindow = 1.0;
		}
		else if (bound.w > 0.0)
		{
			// One window: a room of the family's proportions whose diagonal follows the shape, so the
			// front face spans the glass and the room image is framed on the window.
			s.size = reference * clamp(bound.w / (0.5 * length(reference)), 0.55, 2.85);
			grid = planeOffset / s.size + 0.5;
			s.framedWindow = 1.0;
		}
		else
		{
			// A facade carrying many windows: a grid of typical rooms anchored on the shape. The cell is
			// chosen from the pane's own centre where the glow map outlines it, so a room boundary can
			// only ever fall on a mullion or a frame, never across the glass of one pane. Curtains and
			// people need the window's own edges, which a facade shape does not give, so they stay off.
			s.size = reference;
			grid = planeOffset / s.size + 0.5;
			cell = floor(grid);
			Aperture aperture = FindAperture(glowTex, glowSampler, uv, paneSource);
			float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
			[branch] if (aperture.confidence > 0.5 && abs(det) > 1e-14)
			{
				float3 dpdu = (dpdx * duvdy.y - dpdy * duvdx.y) / det;
				float3 dpdv = (dpdy * duvdx.x - dpdx * duvdy.x) / det;
				float3 paneCenter = fromCenter + dpdu * (aperture.centerUV.x - uv.x) + dpdv * (aperture.centerUV.y - uv.y);
				cell = floor(float2(dot(paneCenter, s.right), dot(paneCenter, s.up)) / s.size + 0.5);
			}
			grid -= cell;
			s.tier = min(s.tier, 2u);
			s.framedWindow = 0.0;
		}

		s.seed = WindowSeed(anchor, cell);
		s.local = saturate(grid);
		s.footprint = max(length(dpdx), length(dpdy)) / min(s.size.x, s.size.y);
		s.roomWeight = pane * fade;
		s.albedoScale = lerp(1.0, 0.15, s.roomWeight);
		return;
	}

	/** An empty surface: no pane, material untouched. Fill it with EvaluateSurfaceInto. */
	Surface InitSurface()
	{
		Surface s = (Surface)0;
		s.albedoScale = 1.0;
		return s;
	}

	float TraceBox(float3 origin, float3 ray)
	{
		float tx = ray.x > 1e-5 ? (1.0 - origin.x) / ray.x : (ray.x < -1e-5 ? -origin.x / ray.x : 1e5);
		float ty = ray.y > 1e-5 ? (1.0 - origin.y) / ray.y : (ray.y < -1e-5 ? -origin.y / ray.y : 1e5);
		return min(min(tx, ty), 1.0 / ray.z);
	}

	// Projects a hit point in the room box onto the one-point-perspective room image (cell UV, y down).
	float2 ProjectToImage(float3 hit, float2 size)
	{
		float2 c = hit.xy * 2.0 - 1.0;
		c /= 1.0 + saturate(hit.z) * (1.0 / kBackWallScale - 1.0);
		// Crop the square image to the aperture aspect, keeping the floor anchored at the sill.
		float aspect = size.x / max(size.y, 1.0);
		float2 crop = (aspect < 1.0 ? float2(aspect, 1.0) : float2(1.0, 1.0 / aspect)) * 0.92;
		c.x *= crop.x;
		c.y = -1.0 + (c.y + 1.0) * crop.y;
		return float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
	}

	Interior EvaluateInterior(Surface s, float3 positionCS, SamplerState samp)
	{
		Interior result = (Interior)0;
		if (s.roomWeight <= 1e-4 || s.tier < 2)
			return result;

		float3 ray = normalize(positionCS);
		float facing = saturate(dot(-ray, s.normal));
		float grazing = smoothstep(0.05, 0.25, facing);
		if (grazing <= 1e-4)
			return result;

		float roomDepth = SharedData::livingWindowsSettings.RoomDepth * s.size.x * 0.5;
		float3 boxRay = float3(dot(ray, s.right) / s.size.x, dot(ray, s.up) / s.size.y, max(dot(ray, -s.normal) / max(roomDepth, 1.0), 1e-3));
		float3 origin = float3(s.local, 0.0);
		float night = SharedData::livingWindowsSettings.NightBlend;
		uint family = s.family;

		[branch] if (SharedData::InInterior)
		{
			// From indoors the pane looks out onto a distant landscape.
			if (SharedData::livingWindowsSettings.OutdoorViews == 0 || !HasAtlas(kAtlasOutdoorDay))
				return result;
			float3 far = origin + boxRay * (6.0 / boxRay.z);
			float2 cellUV = float2((far.x - 0.5) * 0.36 + 0.5, 0.5 - (far.y - 0.5) * 0.36);
			float row = family == 2 ? 1.0 : (family == 3 ? 2.0 : (family == 4 ? 3.0 : 0.0));
			float tile = row * 4.0 + floor(Hash11(s.seed * 193.7) * 3.999);
			float3 day = SampleCell(OutdoorDayAtlas, samp, tile, cellUV, s.footprint * 0.36).rgb;
			float3 nightView = HasAtlas(kAtlasOutdoorNight) ? SampleCell(OutdoorNightAtlas, samp, tile, cellUV, s.footprint * 0.36).rgb * 0.35 : day * 0.06;
			result.color = lerp(day, nightView, night) * SharedData::livingWindowsSettings.OutdoorBrightness;
			result.weight = s.roomWeight * grazing;
			return result;
		}

		float tWall = TraceBox(origin, boxRay);
		float3 hit = origin + boxRay * tWall;
		float2 imageUV = ProjectToImage(hit, s.size);

		// How fast the room image is actually sampled, which is what the mip has to follow. The atlas
		// is read through a coordinate projected into the room box, so its rate differs from the
		// pane's: slower on the back wall, which the image shrinks to half size, faster down a
		// grazing side wall. Re-projecting one pixel step measures it without derivatives, which
		// are not available here (this runs under dynamic branches). Neighbouring pixels' rays also
		// fan apart behind the glass, by the distance travelled over the distance to the camera.
		// The mip used to be clamped to the pane's rate as well, which on the back wall - most of
		// what a window shows - read the room one mip level blurrier than the screen could resolve.
		float cameraDistance = max(length(positionCS), 1.0);
		float2 imageFootprint;
		{
			float spread = s.footprint * (1.0 + tWall / cameraDistance);
			float3 originStep = origin + float3(spread, spread, 0.0);
			float2 imageStepUV = ProjectToImage(originStep + boxRay * TraceBox(originStep, boxRay), s.size);
			imageFootprint = abs(imageStepUV - imageUV);
		}
		const float roomFootprint = max(max(imageFootprint.x, imageFootprint.y), 1e-5);

		float3 room = SampleCell(RoomAtlas, samp, SelectRoomTile(s.seed, family), imageUV, roomFootprint).rgb;

		// Each window has its own evening: some never light up, the rest switch off at different hours.
		float lit = Hash11(s.seed * 91.7 + 3.3) < SharedData::livingWindowsSettings.NightLitShare ? 1.0 : 0.0;
		float hour = SharedData::livingWindowsSettings.GameHour;
		hour = hour < 6.0 ? hour + 24.0 : hour;
		float lightsOut = lerp(21.5, 26.5, Hash11(s.seed * 53.1 + 1.7));
		lit *= 1.0 - smoothstep(lightsOut, lightsOut + 0.5, hour);
		float flicker = 1.0 + 0.04 * sin(SharedData::Timer * (5.0 + s.seed * 3.0)) * sin(SharedData::Timer * 1.7 + s.seed * 40.0);
		float3 daylight = float3(0.43, 0.45, 0.47);
		float3 lamplight = float3(1.25, 0.92, 0.62) * flicker;
		float3 darkness = float3(0.03, 0.035, 0.05);
		float3 light = lerp(daylight, lerp(darkness, lamplight, lit), night) * SharedData::livingWindowsSettings.InteriorBrightness;

		// Walls meeting the window reveal receive less light.
		float edge = max(abs(hit.x - 0.5), abs(hit.y - 0.5)) * 2.0;
		float3 color = room * light * lerp(1.0, 0.78, smoothstep(0.8, 1.0, edge) * (1.0 - hit.z));

		[branch] if (s.tier >= 3 && SharedData::livingWindowsSettings.Occupants != 0 && HasAtlas(kAtlasOccupant))
		{
			float presence = lerp(1.0, lit, night);  // dark rooms stay empty
			float duration = lerp(18.0, 42.0, Hash11(s.seed * 79.13 + 11.7));
			float clock = (SharedData::Timer + s.seed * 977.0) / duration;
			float phase = frac(clock);
			float eventSeed = Hash11(s.seed * 113.0 + floor(clock) * 17.17 + 0.31);
			float envelope = (eventSeed < SharedData::livingWindowsSettings.OccupantActivity * presence ? 1.0 : 0.0) *
			                 smoothstep(0.0, 0.12, phase) * (1.0 - smoothstep(0.88, 1.0, phase));
			float depth = lerp(0.3, 0.75, Hash11(eventSeed * 91.7 + 3.1));
			float tPerson = depth / boxRay.z;
			[branch] if (envelope > 1e-3 && tPerson < tWall)
			{
				float3 p = origin + boxRay * tPerson;
				float direction = Hash11(eventSeed * 53.1) > 0.5 ? 1.0 : -1.0;
				float eased = phase * phase * (3.0 - 2.0 * phase);
				float x = Hash11(eventSeed * 31.9) < 0.6 ? lerp(-0.35, 1.35, direction > 0.0 ? eased : 1.0 - eased) : 0.5 + direction * 0.18 * sin(phase * 6.2831853);
				// A standing adult spans about one and a half window heights, feet below the sill.
				static const float kPersonHeight = 1.45;
				static const float kFeet = -0.55;
				float widthBox = kPersonHeight * s.size.y / max(s.size.x, 1.0);
				float2 q = float2((p.x - x) / widthBox + 0.5, (p.y - kFeet) / kPersonHeight);
				[branch] if (all(q >= 0.0) && all(q <= 1.0))
				{
					float tile = floor(Hash11(eventSeed * 157.7 + s.seed * 23.9) * 15.999);
					float2 cellUV = float2(direction > 0.0 ? q.x : 1.0 - q.x, 1.0 - q.y);
					float personFootprint = s.footprint * (1.0 + tPerson / cameraDistance) / min(kPersonHeight, widthBox);
					float4 person = SampleCell(OccupantAtlas, samp, tile, cellUV, personFootprint);
					float alpha = smoothstep(0.3, 0.7, person.a) * envelope;
					float3 personLight = lerp(daylight * 0.9, lerp(darkness, lamplight * 0.8, lit), night) * SharedData::livingWindowsSettings.InteriorBrightness;
					color = lerp(color, person.rgb * personLight, alpha);
				}
			}
		}

		[branch] if (s.framedWindow > 0.5 && SharedData::livingWindowsSettings.Curtains != 0 && HasAtlas(kAtlasCurtain) && Hash11(s.seed * 67.13 + 4.7) < SharedData::livingWindowsSettings.CurtainShare)
		{
			float3 p = origin + boxRay * (kCurtainDepth / boxRay.z);
			[branch] if (all(p.xy >= 0.0) && all(p.xy <= 1.0))
			{
				float4 cloth = SampleCell(CurtainAtlas, samp, SelectCurtainTile(s.seed, family), float2(p.x, 1.0 - p.y), s.footprint);
				// Keep the middle of the window open whatever the drape artwork does.
				float alpha = smoothstep(0.35, 0.65, cloth.a) * smoothstep(0.16, 0.30, abs(p.x - 0.5));
				float3 clothLight = lerp(daylight * 1.1, lerp(darkness * 2.0, lamplight * 0.7, lit), night) * SharedData::livingWindowsSettings.InteriorBrightness;
				color = lerp(color, cloth.rgb * clothLight, alpha);
			}
		}

		result.color = max(color, 0.0);
		result.weight = s.roomWeight * grazing * (1.0 - s.frost * 0.85);
		return result;
	}

	float3 DebugColor(uint tier)
	{
		return tier >= 3 ? float3(0.1, 0.9, 0.32) : (tier == 2 ? float3(1.0, 0.63, 0.08) : float3(0.1, 0.38, 1.0));
	}
}

#endif  // __LIVING_WINDOWS_HLSLI__
