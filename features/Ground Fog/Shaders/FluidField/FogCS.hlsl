// Fluid Field fog (D21): ground fog amount per wind-grid cell (1 full, 0 clear), drawn by GroundFogCS. Carried by the
// wind and the weather's drift, it settles back toward a resting pattern. Movers plough it aside (their shove, packing it up
// to MaxFog) and carve it (SplatCS); their wake curls it; strong stirring thins it. Scrolled-in and reset cells start from
// the parent level's fog (D193), or the resting pattern without one; the edge band relaxes toward the parent.

#include "FluidField/Common.hlsli"
#include "FluidField/FogPattern.hlsli"
#include "FluidField/Levels.hlsli"

cbuffer FogCB : register(b1)
{
	float2 Drift;      // Weather wind carrying the fog, world units per second
	float RefillRate;  // Per-second rate at which fog relaxes toward the resting pattern
	float StirRate;    // Per-second rate at which fully stirred air clears the fog
	float StirSpeed;   // Wind speed (world units per second) that counts as fully stirred
	float FogPad0;
	float MaxFog;  // Most fog a cell can be packed to, as a multiple of the resting amount
	float FogPad;
	float2 PatternOffset;  // Resting pattern's drift with the weather's wind, world units
	float2 FogPad2;
};

/** Heavy fog follows the air only partly, and smooths out as it moves. */
static const float FOG_DRAG = 0.6;
static const float FOG_DIFFUSION = 0.15;

/** Share of the way an edge cell moves toward its parent's fog each step. */
static const float EDGE_RELAX = 0.5;

Texture2D<float2> Velocity : register(t0);
Texture2D<float> FogIn : register(t1);
Texture2D<int> SplatShoveX : register(t2);
Texture2D<int> SplatShoveY : register(t3);
Texture2D<uint> SplatCarve : register(t4);
Texture2D<float> ParentFog : register(t5);
RWTexture2D<float> FogOut : register(u0);

/** Last step's fog at a logical cell; the resting pattern for cells not in the grid then (or after a reset). */
float LoadFog(int2 logical)
{
	int2 cell = GridMin + logical;
	bool kept = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));
	const float2 world = (float2(cell) + 0.5) * CellSize;
	const float fresh = HasOther != 0 ? Levels::SampleOther(ParentFog, world) : FogPattern::Resting(world, PatternOffset);
	return kept ? FogIn[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))] : fresh;
}

/** Last step's fog at a logical position (texel centres at +0.5), bilinear. */
float SampleFog(float2 position)
{
	float2 p = position - 0.5;
	int2 i = int2(floor(p));
	float2 t = p - float2(i);
	return lerp(lerp(LoadFog(i), LoadFog(i + int2(1, 0)), t.x), lerp(LoadFog(i + int2(0, 1)), LoadFog(i + int2(1, 1)), t.x), t.y);
}

/** The movers' shove on the fog at a logical cell (world units/s), clamped to the grid. */
float2 Shove(int2 logical)
{
	const uint2 s = LogicalToStorage(clamp(logical, 0, int(GridRes) - 1));
	return float2(SplatShoveX[s], SplatShoveY[s]) / Levels::SPLAT_SCALE;
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float2 wind = Velocity[id];

	// Advected by wind, drift and shove; the fog thins where the shove diverges and packs up where it converges
	float2 position = float2(logical) + 0.5;
	float2 shove = Shove(logical);
	float2 source = position - DeltaTime * (wind * FOG_DRAG + Drift + shove) / CellSize;
	float fog = SampleFog(source);
	// Viscous: disturbances soften rather than staying sharp
	float neighbours = 0.25 * (SampleFog(source + float2(1, 0)) + SampleFog(source - float2(1, 0)) + SampleFog(source + float2(0, 1)) + SampleFog(source - float2(0, 1)));
	fog = lerp(fog, neighbours, saturate(FOG_DIFFUSION * DeltaTime * 60.0));
	float divergence = (Shove(logical + int2(1, 0)).x - Shove(logical - int2(1, 0)).x + Shove(logical + int2(0, 1)).y - Shove(logical - int2(0, 1)).y) /
	                   (2.0 * CellSize);
	fog *= exp(-divergence * DeltaTime);
	// Cut outright where a mover carves it (a blade's path, a blast)
	fog *= 1.0 - SplatCarve[id] / Levels::CARVE_SCALE;

	// Thinned where the air is stirred, then filling back in
	float stirred = saturate(length(wind) / max(StirSpeed, 1.0));
	fog *= exp(-StirRate * stirred * DeltaTime);
	const float resting = FogPattern::Resting((float2(GridMin + logical) + 0.5) * CellSize, PatternOffset);
	fog = resting + (fog - resting) * exp(-RefillRate * DeltaTime);

	if (HasOther != 0)
		fog = lerp(fog, Levels::SampleOther(ParentFog, Levels::WorldOf(logical)), Levels::EdgeWeight(logical) * EDGE_RELAX);

	FogOut[id] = clamp(fog, 0.0, MaxFog);
}
