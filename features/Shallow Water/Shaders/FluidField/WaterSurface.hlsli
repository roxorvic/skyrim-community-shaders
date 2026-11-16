#ifndef FLUIDFIELD_WATERSURFACE_HLSLI
#define FLUIDFIELD_WATERSURFACE_HLSLI

// Fluid Field's simulated surface for any water stage: the pixel shader (WaterNormal.hlsli) and PBR Water's
// displacement (vertex and domain shaders). Surface texel: slopes, water surface height (absolute once sampled), coverage.

namespace FluidField
{
	Texture2D<float4> WaterSurfaceTex : register(t66);
	Texture2D<float4> WaterFlowTex : register(t67);
	Texture2D<float4> FarSurfaceTex : register(t68);
	Texture2D<float4> FarFlowTex : register(t69);
	// Last frame's surfaces, vertex and domain stages only (motion vectors)
	Texture2D<float4> WaterSurfacePrevTex : register(t70);
	Texture2D<float4> FarSurfacePrevTex : register(t71);

	/** Share of the near level's width over which it hands over to the far level (matches WaterOutputCS). */
	static const float NEAR_FADE = 0.15;

	/** One water level's placement on the world. */
	struct Level
	{
		int2 GridMin;
		int GridRes;
		float CellSize;
		bool Enabled;
	};

	Level NearLevel()
	{
		Level level;
		level.GridMin = SharedData::fluidFieldSettings.GridMin;
		level.GridRes = int(SharedData::fluidFieldSettings.GridRes);
		level.CellSize = SharedData::fluidFieldSettings.CellSize;
		level.Enabled = SharedData::fluidFieldSettings.Enabled != 0;
		return level;
	}

	Level FarLevel()
	{
		Level level;
		level.GridMin = SharedData::fluidFieldSettings.FarGridMin;
		level.GridRes = int(SharedData::fluidFieldSettings.FarGridRes);
		level.CellSize = SharedData::fluidFieldSettings.FarCellSize;
		level.Enabled = SharedData::fluidFieldSettings.Enabled != 0 && SharedData::fluidFieldSettings.FarEnabled != 0;
		return level;
	}

	/** A world position in a level's cell coordinates (texel centres at integers); false outside it. */
	bool ToCell(Level a_level, float2 a_worldXY, out float2 a_cellPosition)
	{
		a_cellPosition = a_worldXY / max(a_level.CellSize, 1e-3) - 0.5;
		const int2 logical = int2(floor(a_cellPosition)) - a_level.GridMin;
		return a_level.Enabled && all(logical >= 0) && all(logical < a_level.GridRes - 1);
	}

	float4 BilinearSurface(Texture2D<float4> a_tex, Level a_level, float2 a_cellPosition)
	{
		const int mask = a_level.GridRes - 1;
		const int2 c = int2(floor(a_cellPosition));
		const float2 t = a_cellPosition - float2(c);
		return lerp(lerp(a_tex.Load(int3(c & mask, 0)), a_tex.Load(int3((c + int2(1, 0)) & mask, 0)), t.x),
			lerp(a_tex.Load(int3((c + int2(0, 1)) & mask, 0)), a_tex.Load(int3((c + int2(1, 1)) & mask, 0)), t.x), t.y);
	}

	/** World units from a drawn plane over which the sim's water fades to another pool's (SplineHeight) */
	static const float2 LEVEL_MATCH = float2(48.0, 80.0);

	/**
	 * A level's surface height (z) at a cell position with a uniform cubic B-spline over 4 x 4 cells, and its gradient per
	 * cell. Bilinear reading put a crease at every cell edge and turned a single high cell into a pyramid; the B-spline is
	 * smooth and averages over about two cells, the finest the sim resolves.
	 * Cells far from a_reference (the drawn plane, stored units) are another pool's water across a vanilla level step: read
	 * as the reference, or a plane straddling a falls was pulled up or down the step into a sheet.
	 */
	float3 SplineHeight(Texture2D<float4> a_tex, Level a_level, float2 a_cellPosition, float a_reference)
	{
		const int mask = a_level.GridRes - 1;
		const int2 c = int2(floor(a_cellPosition));
		const float2 t = a_cellPosition - float2(c);
		const float2 it = 1.0 - t;
		const float2 w0 = it * it * it / 6.0, w1 = (3.0 * t * t * t - 6.0 * t * t + 4.0) / 6.0;
		const float2 w2 = (-3.0 * t * t * t + 3.0 * t * t + 3.0 * t + 1.0) / 6.0, w3 = t * t * t / 6.0;
		const float2 d0 = -0.5 * it * it, d1 = 1.5 * t * t - 2.0 * t, d2 = -1.5 * t * t + t + 0.5, d3 = 0.5 * t * t;
		const float wx[4] = { w0.x, w1.x, w2.x, w3.x }, wy[4] = { w0.y, w1.y, w2.y, w3.y };
		const float dx[4] = { d0.x, d1.x, d2.x, d3.x }, dy[4] = { d0.y, d1.y, d2.y, d3.y };
		float3 r = 0.0;
		[unroll] for (int j = 0; j < 4; j++)
		{
			[unroll] for (int i = 0; i < 4; i++)
			{
				float h = a_tex.Load(int3((c + int2(i - 1, j - 1)) & mask, 0)).z;
				h = lerp(h, a_reference, smoothstep(LEVEL_MATCH.x, LEVEL_MATCH.y, abs(h - a_reference)));
				r += h * float3(wx[i] * wy[j], dx[i] * wy[j], wx[i] * dy[j]);
			}
		}
		return r;
	}

	/** How much the near level (rather than the far one) decides the water here: 1 inside, 0 past its edge band. */
	float NearWeight(Level a_near, float2 a_worldXY)
	{
		const float2 logical = a_worldXY / a_near.CellSize - float2(a_near.GridMin);
		const float fromEdge = min(min(logical.x, logical.y), min(a_near.GridRes - logical.x, a_near.GridRes - logical.y));
		return smoothstep(0.0, 1.0, fromEdge / (NEAR_FADE * a_near.GridRes));
	}

	/** The simulated surface at a world position, near level cross-faded into far; false outside both. */
	bool SampleSurface(float3 a_worldPosition, out float4 a_surface)
	{
		a_surface = 0;
		const Level nearLevel = NearLevel();
		const Level farLevel = FarLevel();
		float2 nearCell, farCell;
		const bool inNear = ToCell(nearLevel, a_worldPosition.xy, nearCell);
		const bool inFar = ToCell(farLevel, a_worldPosition.xy, farCell);
		if (!inNear && !inFar)
			return false;
		float4 nearSurface = inNear ? BilinearSurface(WaterSurfaceTex, nearLevel, nearCell) : 0;
		float4 farSurface = inFar ? BilinearSurface(FarSurfaceTex, farLevel, farCell) : 0;
		// Height smooth (B-spline), its slope from the same spline so the shading follows the shape
		const float reference = a_worldPosition.z - SharedData::fluidFieldSettings.RefZ;
		if (inNear) {
			const float3 h = SplineHeight(WaterSurfaceTex, nearLevel, nearCell, reference);
			nearSurface.xyz = float3(h.yz / nearLevel.CellSize, h.x);
		}
		if (inFar) {
			const float3 h = SplineHeight(FarSurfaceTex, farLevel, farCell, reference);
			farSurface.xyz = float3(h.yz / farLevel.CellSize, h.x);
		}
		const float weight = inNear ? (inFar ? NearWeight(nearLevel, a_worldPosition.xy) : 1.0) : 0.0;
		a_surface = lerp(farSurface, nearSurface, weight);
		// Eased in by Fade, except the height: absolute, it would sink toward RefZ (PBR Water fades it in by AreaWeight)
		a_surface.xyw *= SharedData::fluidFieldSettings.Fade;
		a_surface.z += SharedData::fluidFieldSettings.RefZ;
		return true;
	}

	/** How far into a level a world position lies, 0 at its edge to 1 past the band it hands over in (matches WaterOutputCS). */
	float LevelInside(Level a_level, float2 a_worldXY)
	{
		float2 cellPosition;
		if (!ToCell(a_level, a_worldXY, cellPosition))
			return 0.0;
		const float2 logical = cellPosition - float2(a_level.GridMin);
		const float fromEdge = min(min(logical.x, logical.y), min(a_level.GridRes - 1 - logical.x, a_level.GridRes - 1 - logical.y));
		return smoothstep(0.0, 1.0, fromEdge / (0.15 * a_level.GridRes));
	}

	/** Last frame's water surface height at a world position (absolute units), read with last frame's placement and RefZ. */
	float PreviousHeight(float3 a_worldPosition)
	{
		Level nearLevel = NearLevel();
		Level farLevel = FarLevel();
		nearLevel.GridMin = SharedData::fluidFieldSettings.PrevGridMin;
		farLevel.GridMin = SharedData::fluidFieldSettings.PrevFarGridMin;
		float2 nearCell, farCell;
		const bool inNear = ToCell(nearLevel, a_worldPosition.xy, nearCell);
		const bool inFar = ToCell(farLevel, a_worldPosition.xy, farCell);
		if (!inNear && !inFar)
			return 0.0;
		const float reference = a_worldPosition.z - SharedData::fluidFieldSettings.PrevRefZ;
		const float nearHeight = inNear ? SplineHeight(WaterSurfacePrevTex, nearLevel, nearCell, reference).x : 0.0;
		const float farHeight = inFar ? SplineHeight(FarSurfacePrevTex, farLevel, farCell, reference).x : 0.0;
		const float weight = inNear ? (inFar ? NearWeight(nearLevel, a_worldPosition.xy) : 1.0) : 0.0;
		return lerp(farHeight, nearHeight, weight) + SharedData::fluidFieldSettings.PrevRefZ;
	}

	/** The flow output at a world position (velocity xy in world units/s, foam z, depth w in units), near level cross-faded
	 * into far like the surface; 0 outside both and on dry cells. Not eased in: callers weigh it with AreaWeight. */
	float4 SampleFlowOutput(float2 a_worldXY)
	{
		const Level nearLevel = NearLevel();
		const Level farLevel = FarLevel();
		float2 nearCell, farCell;
		const bool inNear = ToCell(nearLevel, a_worldXY, nearCell);
		const bool inFar = ToCell(farLevel, a_worldXY, farCell);
		const float4 nearFlow = inNear ? BilinearSurface(WaterFlowTex, nearLevel, nearCell) : 0;
		const float4 farFlow = inFar ? BilinearSurface(FarFlowTex, farLevel, farCell) : 0;
		const float weight = inNear ? (inFar ? NearWeight(nearLevel, a_worldXY) : 1.0) : 0.0;
		return lerp(farFlow, nearFlow, weight);
	}

	/** The sim's share of the water here: 1 inside its area, easing to 0 where it hands over to vanilla. Not faded at
	 * shores or rocks, where the sim's surface is what the water must follow. */
	float AreaWeight(float2 a_worldXY)
	{
		return max(LevelInside(NearLevel(), a_worldXY), LevelInside(FarLevel(), a_worldXY)) * SharedData::fluidFieldSettings.Fade;
	}
}

#endif  // FLUIDFIELD_WATERSURFACE_HLSLI
