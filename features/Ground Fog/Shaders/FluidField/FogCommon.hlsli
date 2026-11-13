// Ground fog v3 (D194): heavy fog on a toroidal grid round the player, moved by a virtual-pipe model. Fog flows only from
// a higher fog surface (ground + depth) to a lower one; rocks take fog only from ground above their tops. D195: up to two
// levels a cell (the top surface, and the floor under an overhang or at a ledge's foot), each with its own depth; texture
// arrays hold one slice a level. Shared by FogBedCS, FogSplatCS, FogInputsCS, FogFluxCS, FogDepthCS and FogContactCS.

cbuffer FogCB : register(b0)
{
	int2 GridMin;      // World cell of logical texel (0, 0)
	int2 PrevGridMin;  // Last step's; cells new since then are seeded with the settled field
	float CellSize;    // World units per cell
	float DeltaTime;   // Seconds
	uint Resolution;   // Cells per side, a power of two
	float Gravity;     // Fog's reduced gravity, world units per second squared
	float GroundThickness;
	float PoolDepth;
	float SourceRate;     // Per second, toward Ground Thickness where fog may form
	float EvaporateRate;  // Per second, of the depth over Pool Depth
	float FluxDamping;    // Per second
	float Confinement;    // Vorticity confinement strength
	float HollowGain;     // Settled depth added per unit of hollowness
	float Pad0;
	int4 SlotTiles[256];  // Ground tiles: per resident slot, xy the tile there, z 1 once valid
	int2 TileMin;
	int2 LandCell;  // The land map's centre cell (LandMap.hlsli)
	uint HasLand;
	uint MoverCount;
	uint ContactCount;
	float ContactCell;  // World units per contact map texel
	int2 ContactMin;    // Contact map: world texel of texel (0, 0)
	uint ContactResolution;
	float WindX;  // The weather's wind carrying the fog, world units per second
	float WindY;
	float Drift;                           // Ambient drift's speed, world units per second
	float Time;                            // Seconds, for the ambient drift
	float Rain;                            // How hard it rains, 0 to 1: drops strike the top level
	row_major float4x4 OcclusionViewProj;  // The precipitation occlusion map's projection, from the eye (Skylighting's use)
	float3 Eye;
	uint HasOcclusion;
};

/** Bed kinds (FogBed z). */
static const float KIND_GROUND = 0.0, KIND_ROCK = 1.0, KIND_CLOSED = 2.0, KIND_WATER = 3.0;
/** GroundTiles' texels: 8 units, 128 a tile, 8 tiles a side. */
static const float TILE_TEXEL = 8.0;
static const int TILE_SLOTS = 16;
static const uint TILE_MAY_FORM = 1, TILE_ROCK = 2, TILE_STEEP = 4, TILE_WATER = 8;
/** Floors more than this apart in height are two levels, not one slope. */
static const float LEVEL_STEP = 24.0;
/** Movers' pushes and cuts are summed as fixed point (InterlockedAdd). */
static const float FORCE_SCALE = 256.0;

/** The ambient drift's texture (FogDriftCS): a texel every DRIFT_TEXEL cells, DRIFT_TEXELS a side, toroidal. */
static const int DRIFT_TEXEL = 4;
static const int DRIFT_TEXELS = 128;

/** World drift texel of the drift texture's texel (0, 0) window corner. */
int2 DriftMin()
{
	return GridMin >> 2;
}

/** Toroidal texel of a world cell. */
uint2 TexelOf(int2 cell)
{
	return uint2(cell & int(Resolution - 1));
}

/** World cell held by a texel. */
int2 CellOf(uint2 texel)
{
	return GridMin + ((int2(texel) - GridMin) & int(Resolution - 1));
}

bool InGrid(int2 cell)
{
	return all(cell >= GridMin) && all(cell < GridMin + int(Resolution));
}

/** Outflow directions, as Flux's channels: east, west, north, south. */
static const int2 DIRECTIONS[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };

/** A moving thing (FluidFieldSim::Mover): a segment with a Gaussian falloff, and the heights it spans. */
struct Mover
{
	float2 From;
	float2 To;
	float Radius;
	float Shove;      // World units per second it shoves the fog aside at its edge
	float2 Velocity;  // Push, world units per second
	float Cut;        // Share of the fog it clears where it passes
	float Bottom;     // Lowest and highest point, world units
	float Top;
	float Pad;
};

#ifdef FOG_BEDS
Texture2DArray<float4> Bed : register(FOG_BEDS);  // Per level: x height, y fog source share, z kind

/**
 * The level of a neighbouring cell that a level's fog flows into: its floor the nearest at or under this floor (a step up
 * at most), under this level's ceiling (the top's floor, for the lower level); -1 for none. Both sides of a face agree.
 */
int Target(int2 cell, uint level, int2 direction)
{
	const int2 other = cell + direction;
	if (!InGrid(other))
		return -1;
	const float floorHere = Bed[uint3(TexelOf(cell), level)].x;
	const float ceiling = level == 1 ? Bed[uint3(TexelOf(cell), 0)].x - 0.5 * LEVEL_STEP : 1e30;
	int best = -1, lowest = -1;
	float bestFloor = -1e30, lowestFloor = 1e30;
	[unroll] for (uint m = 0; m < 2; m++)
	{
		const float4 bed = Bed[uint3(TexelOf(other), m)];
		if (bed.z == KIND_CLOSED || bed.x >= ceiling)
			continue;
		if (bed.x <= floorHere + LEVEL_STEP && bed.x > bestFloor) {
			best = int(m);
			bestFloor = bed.x;
		}
		if (bed.x < lowestFloor) {
			lowest = int(m);
			lowestFloor = bed.x;
		}
	}
	return best >= 0 ? best : lowest;
}
#endif
