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
