// Ground fog v3 (D194): each cell's two beds from the ground tiles' 8-unit texels under it. The top: their highest surface
// (a step crossing the cell stays sharp), rock if that is a rock's top. D195, the lower level: the highest of their floors
// under overhangs and of their tops a level below the cell's top (a ledge's foot), closed where there is none. Each with
// the share of it fog may form on. Where no tile is resident, the loaded landscape (top only).

#include "FluidField/FogCommon.hlsli"
#include "FluidField/LandMap.hlsli"

Texture2D<float4> GroundTiles : register(t0);  // x top height, y flags, z lower height, w flags
Texture2D<float2> LandMap : register(t1);
RWTexture2DArray<float4> Bed : register(u0);        // Per level: x height, y fog source share, z kind
RWTexture2DArray<float2> DrawFloor : register(u1);  // Per level, for the draw: x the mean floor, y the share that carries fog

/** DrawFloor where a level has no floor. */
static const float NO_FLOOR = -1.0e7;

bool TileValid(int2 texel)
{
	const int2 tile = texel >> 7;
	const int4 slot = SlotTiles[(tile.y & (TILE_SLOTS - 1)) * TILE_SLOTS + (tile.x & (TILE_SLOTS - 1))];
	return all(tile >= TileMin) && all(tile < TileMin + TILE_SLOTS) && slot.z != 0 && all(slot.xy == tile);
}

/** A level's bed from its floors' summary: the highest, its flags, how many fog may form on and how many are water. */
float4 MakeBed(float height, uint flags, float count, float forming, float water)
{
	return float4(height, forming / count, (flags & TILE_ROCK) ? KIND_ROCK : water * 2.0 > count ? KIND_WATER :
																								   KIND_GROUND,
		0.0);
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= Resolution))
		return;
	const int2 cell = CellOf(id.xy);
	const float2 lo = float2(cell) * CellSize, hi = lo + CellSize - 0.01;
	const int2 t0 = int2(floor(lo / TILE_TEXEL)), t1 = int2(floor(hi / TILE_TEXEL));

	// The top first; then the floors a level under it
	float top = -1e30;
	uint topFlags = 0;
	float known = 0.0, forming = 0.0, water = 0.0;
	// The draw's top floor: the mean of its texels that are not rock or log tops (the draw ignores those: their own
	// surfaces hide the fog behind them, and the 8-unit map caught them in stripes), and their share that carries fog
	float groundSum = 0.0, groundCount = 0.0, groundCarries = 0.0;
	[loop] for (int y = t0.y; y <= t1.y; y++)
	{
		[loop] for (int x = t0.x; x <= t1.x; x++)
		{
			if (!TileValid(int2(x, y)))
				continue;
			const float4 g = GroundTiles.Load(int3(int2(x, y) & (TILE_SLOTS * 128 - 1), 0));
			if (g.x < -1e6)
				continue;
			known += 1.0;
			if (!(uint(g.y) & TILE_ROCK)) {
				groundSum += g.x;
				groundCount += 1.0;
				groundCarries += (uint(g.y) & (TILE_STEEP | TILE_WATER)) ? 0.0 : 1.0;
			}
			forming += (uint(g.y) & TILE_MAY_FORM) ? 1.0 : 0.0;
			water += (uint(g.y) & TILE_WATER) ? 1.0 : 0.0;
			if (g.x > top) {
				top = g.x;
				topFlags = uint(g.y);
			}
		}
	}
	float lower = -1e30;
	uint lowerFlags = 0;
	float lowerCount = 0.0, lowerForming = 0.0, lowerWater = 0.0;
	// The draw's lower floor: the mean over its texels, and their share that carries fog (not rock, steep or water)
	float lowerSum = 0.0, lowerCarries = 0.0;
	[loop] for (int y2 = t0.y; y2 <= t1.y; y2++)
	{
		[loop] for (int x2 = t0.x; x2 <= t1.x; x2++)
		{
			if (!TileValid(int2(x2, y2)))
				continue;
			const float4 g = GroundTiles.Load(int3(int2(x2, y2) & (TILE_SLOTS * 128 - 1), 0));
			if (g.x < -1e6)
				continue;
			// This texel's floor under the cell's top: its own lower level, or its top a level down
			const bool own = g.z > -1e6;
			const float floorHeight = own ? g.z : g.x;
			const uint floorFlags = own ? uint(g.w) : uint(g.y);
			if (floorHeight > top - LEVEL_STEP)
				continue;
			lowerCount += 1.0;
			lowerSum += floorHeight;
			lowerCarries += (floorFlags & (TILE_ROCK | TILE_STEEP | TILE_WATER)) ? 0.0 : 1.0;
			lowerForming += (floorFlags & TILE_MAY_FORM) ? 1.0 : 0.0;
			lowerWater += (floorFlags & TILE_WATER) ? 1.0 : 0.0;
			if (floorHeight > lower) {
				lower = floorHeight;
				lowerFlags = floorFlags;
			}
		}
	}

	float4 topBed;
	float2 topFloor;
	if (known > 0.0) {
		topBed = MakeBed(top, topFlags, known, forming, water);
		topFloor = groundCount > 0.0 ? float2(groundSum / groundCount, groundCarries / groundCount) : float2(NO_FLOOR, 0.0);
	} else {
		float landForming, landWater;
		const float land = HasLand != 0 ? LandHeight(LandMap, LandCell, (float2(cell) + 0.5) * CellSize, landForming, landWater) : LAND_NONE;
		topBed = land > LAND_NONE ? float4(land, landForming, landWater > 0.5 ? KIND_WATER : KIND_GROUND, 0.0) : float4(0.0, 0.0, KIND_CLOSED, 0.0);
		topFloor = float2(land > LAND_NONE ? land : NO_FLOOR, landForming);
	}
	Bed[uint3(id.xy, 0)] = topBed;
	Bed[uint3(id.xy, 1)] = lowerCount > 0.0 ? MakeBed(lower, lowerFlags, lowerCount, lowerForming, lowerWater) : float4(0.0, 0.0, KIND_CLOSED, 0.0);
	DrawFloor[uint3(id.xy, 0)] = topFloor;
	DrawFloor[uint3(id.xy, 1)] = lowerCount > 0.0 ? float2(lowerSum / lowerCount, lowerCarries / lowerCount) : float2(NO_FLOOR, 0.0);
}
