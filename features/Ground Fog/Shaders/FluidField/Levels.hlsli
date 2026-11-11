#ifndef FLUIDFIELD_LEVELS_HLSLI
#define FLUIDFIELD_LEVELS_HLSLI

// Fluid Field fog (D193): nested levels round the player. A level reads its parent (the coarser level round it) at its
// edges and in cells it has no history for; the parent then absorbs it (AbsorbCS). Movers are drawn into fixed-point
// splat textures (SplatCS) with integer atomics. Needs Common.hlsli first.

cbuffer LevelCB : register(b2)
{
	int2 OtherGridMin;  // The parent's world cell of its logical texel (0, 0); in AbsorbCS the child's
	uint OtherGridRes;
	float OtherCellSize;
	int2 ChildGridMin;  // The finer level running this frame: it draws the movers it covers itself
	uint ChildGridRes;
	float ChildCellSize;
	uint HasOther;
	uint HasChild;
	uint MoverCount;
	uint EdgeBand;  // Cells along a level's edge that relax toward its parent
};

// D193: the fog's ground map (GroundHeightMap): x the ground, y the top of a rock, log or trunk standing on it
cbuffer WallCB : register(b3)
{
	int2 GroundMin;  // World cell (of GroundCellSize) of the ground map's texel (0, 0)
	uint GroundRes;
	float GroundCellSize;
	uint HasWalls;   // 0: this level has no walls (the far level, or none known)
	float WallRise;  // An object standing this far over the ground is a wall to the air
	float2 WallPad;
};

Texture2D<float4> GroundMap : register(t7);  // x ground, y object top, z floor under an overhang, w rise to a neighbour

namespace Levels
{
	/** Splat values are stored times this (world units per second for the wind and the shove). */
	static const float SPLAT_SCALE = 256.0;
	/** The carve's fixed-point full scale. */
	static const float CARVE_SCALE = 65535.0;

	/** World position of a logical cell's centre. */
	float2 WorldOf(int2 logical)
	{
		return (float2(GridMin + logical) + 0.5) * CellSize;
	}

	/** Cells from a logical cell to the nearest edge of its grid. */
	float FromEdge(float2 logical, float res)
	{
		return min(min(logical.x, logical.y), min(res - 1.0 - logical.x, res - 1.0 - logical.y));
	}

	/** 1 at the grid's edge, falling to 0 over EdgeBand cells: how far a cell follows its parent. */
	float EdgeWeight(int2 logical)
	{
		return saturate(1.0 - FromEdge(float2(logical), float(GridRes)) / float(EdgeBand));
	}

	int2 OtherStorage(int2 logical)
	{
		return (OtherGridMin + clamp(logical, 0, int(OtherGridRes) - 1)) & int(OtherGridRes - 1);
	}

	/** The other level's logical position (texel centres at +0.5) of a world position. */
	float2 OtherLogical(float2 worldXY)
	{
		return worldXY / OtherCellSize - float2(OtherGridMin);
	}

	float2 SampleOther(Texture2D<float2> tex, float2 worldXY)
	{
		const float2 p = OtherLogical(worldXY) - 0.5;
		const int2 i = int2(floor(p));
		const float2 t = p - float2(i);
		return lerp(lerp(tex[OtherStorage(i)], tex[OtherStorage(i + int2(1, 0))], t.x), lerp(tex[OtherStorage(i + int2(0, 1))], tex[OtherStorage(i + int2(1, 1))], t.x), t.y);
	}

	float SampleOther(Texture2D<float> tex, float2 worldXY)
	{
		const float2 p = OtherLogical(worldXY) - 0.5;
		const int2 i = int2(floor(p));
		const float2 t = p - float2(i);
		return lerp(lerp(tex[OtherStorage(i)], tex[OtherStorage(i + int2(1, 0))], t.x), lerp(tex[OtherStorage(i + int2(0, 1))], tex[OtherStorage(i + int2(1, 1))], t.x), t.y);
	}

	/** In AbsorbCS: 1 well inside the child (past two edge bands), 0 outside it and in its own edge band. */
	float OtherInterior(float2 worldXY)
	{
		const float fromEdge = FromEdge(OtherLogical(worldXY) - 0.5, float(OtherGridRes));
		return saturate((fromEdge - float(EdgeBand)) / float(EdgeBand));
	}

	/** The ground map's lowest real height (GroundHeightMap::NO_GROUND); below it: unknown land or water (no fog). */
	static const float NO_GROUND = -1.0e7;

	/** The ground map's texel under a world position (nearest); unknown (x below NO_GROUND) outside it. */
	float4 GroundAt(float2 worldXY)
	{
		const int2 texel = int2(floor(worldXY / GroundCellSize));
		if (HasWalls == 0 || any(texel < GroundMin) || any(texel >= GroundMin + int(GroundRes)))
			return float4(NO_GROUND * 2.0, NO_GROUND, NO_GROUND, 0.0);
		return GroundMap[texel & int(GroundRes - 1)];
	}

	/** True where a rock, log or trunk stands in the cell: no wind in it, and air and fog go round it. */
	bool Solid(int2 logical)
	{
		const float4 g = GroundAt(WorldOf(logical));
		return g.x > NO_GROUND && g.y > g.x + WallRise;
	}

	/** True where the running child draws this position's movers and is wholly absorbed back. */
	bool ChildCovers(float2 worldXY)
	{
		if (HasChild == 0)
			return false;
		const float2 logical = worldXY / ChildCellSize - float2(ChildGridMin) - 0.5;
		return FromEdge(logical, float(ChildGridRes)) >= 2.0 * float(EdgeBand);
	}
}

#endif
