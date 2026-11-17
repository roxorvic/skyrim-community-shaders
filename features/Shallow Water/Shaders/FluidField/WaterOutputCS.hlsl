// Fluid Field water, pass 5: outputs for other shaders. Surface: (slope x, slope y, water surface height - RefZ, coverage);
// Flow: (cell-centred velocity, foam, depth). Coverage is 0 when dry and fades out toward the edge and shores.
// The height is the sim's water surface, absolute (against RefZ, for half-float precision): PBR Water's surface sits on
// it inside the sim's area, each draw against its own plane (DisplaceSurface), so overlapping vanilla planes cannot offset
// it. Dry cells beside water carry the water's level, so the surface never drains into a wall.

#include "FluidField/WaterCommon.hlsli"

Texture2D<float> Depth : register(t0);
Texture2D<float2> Velocity : register(t1);
Texture2D<float2> CellMap : register(t2);
Texture2D<float> Foam : register(t3);

RWTexture2D<float4> SurfaceOut : register(u0);
RWTexture2D<float4> FlowOut : register(u1);

/** @brief Steepest surface slope passed to the water shader (rise over run): guards against spikes at jagged edges. */
static const float MAX_SLOPE = 0.35;
/** @brief Most the water dips below vanilla's level (units) right at a bank or rock: a shore falls a little with the
 * flow, never off the bank. Out in open water there is no limit. */
static const float SHORE_DIP = 3.5;
/** @brief World units a dry cell's carried water level stays under the obstacle's own surface, so it never shows through. */
static const float SKIN_MARGIN = 2.0;
/** @brief World units under a rock's sampled surface the mesh is held in cells with no water near (the sampling is coarse). */
static const float DEEP_MARGIN = 35.0;
/** @brief Water on a bed above vanilla's (a rock's top, a bank) shows as a surface only from this depth (units, ~4 cm):
 * a film is wet rock, not water standing on it. */
static const float SHEET_MIN_DEPTH = 1.0;

float Eta(int2 logical, float fallback)
{
	uint2 storage = LogicalToStorage(clamp(logical, 0, int(GridRes) - 1));
	float depth = Depth[storage];
	// A dry neighbour has no surface; use this cell's so the shoreline does not read as a cliff
	return depth > WetThreshold ? CellMap[storage].x + depth : fallback;
}

bool IsWet(int2 logical)
{
	return Depth[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))] > WetThreshold;
}

float2 SlopeAt(int2 logical)
{
	uint2 storage = LogicalToStorage(clamp(logical, 0, int(GridRes) - 1));
	float eta = CellMap[storage].x + Depth[storage];
	return float2(Eta(logical + int2(1, 0), eta) - Eta(logical + int2(-1, 0), eta),
			   Eta(logical + int2(0, 1), eta) - Eta(logical + int2(0, -1), eta)) /
	       (2.0 * CellSize);
}

/** @brief The lowest of the water map's vanilla levels around a world position, or 1e30 where none has water. */
float LowestVanillaLevel(float2 worldPos)
{
	int2 i = int2(floor(worldPos / MapSpacing));
	float lowest = 1e30;
	[unroll] for (int y = -1; y <= 2; y++)
	{
		[unroll] for (int x = -1; x <= 2; x++)
		{
			float level = LoadMapLattice(i + int2(x, y)).y;
			if (HasWater(level))
				lowest = min(lowest, level);
		}
	}
	return lowest;
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float depth = Depth[id];
	float2 cellMap = CellMap[id];
	bool wet = depth > WetThreshold;
	// A film, or water on a steep flank, on a bed above vanilla's water is shown as the rock it wets: its surface is the
	// water's level beside it
	bool surfaced = wet && (cellMap.y > WetThreshold || depth >= SHEET_MIN_DEPTH);

	float eta = cellMap.x + depth;

	// Tent-smoothed over wet 3 x 3 cells, so water piled against a bank is not a one-cell spike
	float2 slope = 0.0;
	float slopeWeight = 0.0;
	[unroll] for (int dy = -1; dy <= 1; dy++)
	{
		[unroll] for (int dx = -1; dx <= 1; dx++)
		{
			int2 n = logical + int2(dx, dy);
			float w = (2.0 - abs(dx)) * (2.0 - abs(dy));
			if (IsWet(n)) {
				slope += SlopeAt(n) * w;
				slopeWeight += w;
			}
		}
	}
	slope = slopeWeight > 0.0 ? slope / slopeWeight : 0.0;
	float slopeLength = length(slope);
	if (slopeLength > MAX_SLOPE)
		slope *= MAX_SLOPE / slopeLength;

	// The wet cells within two cells: the shore fade, vanilla's level here, and the level a dry cell carries
	float wetCount = 0.0, surfaceSum = 0.0, vanillaSum = 0.0, vanillaCount = 0.0, poolSum = 0.0, nearSum = 0.0, nearCount = 0.0;
	[unroll] for (int fy = -2; fy <= 2; fy++)
	{
		[unroll] for (int fx = -2; fx <= 2; fx++)
		{
			int2 n = logical + int2(fx, fy);
			if (IsWet(n)) {
				float2 nMap = CellMap[LogicalToStorage(clamp(n, 0, int(GridRes) - 1))];
				wetCount += 1.0;
				surfaceSum += Eta(n, 0.0);
				if (abs(fx) <= 1 && abs(fy) <= 1) {
					nearSum += Eta(n, 0.0);
					nearCount += 1.0;
				}
				if (nMap.y > WetThreshold) {
					vanillaSum += nMap.x + nMap.y;
					vanillaCount += 1.0;
					poolSum += Eta(n, 0.0);
				}
			}
		}
	}
	float shoreFade = smoothstep(0.5, 1.0, wetCount / 25.0);

	// Wide fade over 15% of the grid, so the hand-over to vanilla has no line
	int2 fromEdge = min(logical, int(GridRes) - 1 - logical);
	float edgeFade = smoothstep(0.0, 1.0, float(min(fromEdge.x, fromEdge.y)) / (0.15 * float(GridRes)));
	float coverage = wet ? edgeFade * shoreFade : 0.0;

	float2 here = Velocity[id];
	float uE = LoadClamped(Velocity, logical + int2(1, 0)).x;
	float vN = LoadClamped(Velocity, logical + int2(0, 1)).y;
	float2 flow = wet ? 0.5 * float2(here.x + uE, here.y + vN) : 0.0;

	// The water surface above vanilla's still water: this cell's own where vanilla has water here, else the nearby
	// vanilla water's (a rock or bank the flow climbed). A dry cell carries the water's level from beside it, held just
	// under the obstacle's top so it never shows through; near a wall the water dips at most SHORE_DIP below vanilla.
	// Deep under a rock or bank (no water within two cells): only points over the obstacle read these, so they just keep
	// the plane hidden in it: the lowest vanilla level nearby, never above the pool around (the mean mixed in a higher pool,
	// and a mesh cut's capped top less DEEP_MARGIN stood above low rocks: a flat sheet over them)
	float height = min(LowestVanillaLevel(CellWorldPos(logical)), cellMap.x - DEEP_MARGIN) - RefZ;
	if (wetCount > 0.0) {
		const float vanillaLevel = cellMap.y > WetThreshold ? cellMap.x + cellMap.y : (vanillaCount > 0.0 ? vanillaSum / vanillaCount : surfaceSum / wetCount);
		// What an unsurfaced cell carries is the pool's level beside it: water on rocks around it must not lift it
		const float carried = vanillaCount > 0.0 ? poolSum / vanillaCount : surfaceSum / wetCount;
		// Water on a rock or bank is shown at the mean of the wet cells around it, so it ramps up the rock instead of
		// jumping cell by cell
		const float sheet = cellMap.y > WetThreshold ? eta : nearSum / max(nearCount, 1.0);
		height = (surfaced ? sheet : carried) - vanillaLevel;
		if (!surfaced)
			height = min(height, max(cellMap.x - vanillaLevel - SKIN_MARGIN, min(height, 0.0)));
		// Eased out away from the wall (full within about a cell of it, none two cells out): cut off at two cells it left a
		// step parallel to every wall, a line of glints
		height = lerp(max(height, -SHORE_DIP), height, saturate((wetCount - 10.0) / 15.0));
		height += vanillaLevel - RefZ;
	}

	SurfaceOut[id] = float4(slope, height, coverage);
	// Foam blurred with its four neighbours so single cells do not sparkle
	float foam = 0.0;
	if (wet) {
		foam = 0.5 * Foam[id];
		foam += 0.125 * Foam[LogicalToStorage(clamp(logical + int2(1, 0), 0, int(GridRes) - 1))];
		foam += 0.125 * Foam[LogicalToStorage(clamp(logical + int2(-1, 0), 0, int(GridRes) - 1))];
		foam += 0.125 * Foam[LogicalToStorage(clamp(logical + int2(0, 1), 0, int(GridRes) - 1))];
		foam += 0.125 * Foam[LogicalToStorage(clamp(logical + int2(0, -1), 0, int(GridRes) - 1))];
	}
	// With a far level, foam is not edge-faded: the water shader cross-fades it, and fading here too leaves a seam.
	FlowOut[id] = float4(flow, SeedRes != 0 ? foam : foam * edgeFade, wet ? depth : 0.0);
}
