// Fluid Field fog (D21): ground fog amount per wind-grid cell (1 full, 0 clear), drawn by GroundFogCS; D193: two layers,
// x on the ground and y on the floor under an overhang there (y follows x where there is none), each carried on its own and
// meeting the other only on the same height (fog under a ledge joins the floor beside it, not the ledge's top). Carried by the
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
/** World units two cells' levels may differ by and still be one layer (more is a step the fog does not cross). */
static const float LAYER_STEP = 40.0;

Texture2D<float2> Velocity : register(t0);
Texture2D<float2> FogIn : register(t1);
Texture2D<int> SplatShoveX : register(t2);
Texture2D<int> SplatShoveY : register(t3);
Texture2D<uint> SplatCarve : register(t4);
Texture2D<float2> ParentFog : register(t5);
Texture2D<int> SplatShoveX1 : register(t8);
Texture2D<int> SplatShoveY1 : register(t9);
Texture2D<uint> SplatCarve1 : register(t10);
RWTexture2D<float2> FogOut : register(u0);

/** Last step's fog (both layers) at a logical cell; the parent's, or the resting pattern, for cells not in the grid then. */
float2 LoadFog(int2 logical)
{
	int2 cell = GridMin + logical;
	bool kept = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));
	const float2 world = (float2(cell) + 0.5) * CellSize;
	const float2 fresh = HasOther != 0 ? Levels::SampleOther(ParentFog, world) : FogPattern::Resting(world, PatternOffset).xx;
	return kept ? FogIn[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))] : fresh;
}

/** A cell's fog on the layer at height a_height: whichever of its levels lies there; a_fallback where none does (a step). */
float LoadLayer(int2 logical, float a_height, float a_fallback)
{
	const float2 fog = LoadFog(logical);
	if (HasWalls == 0)
		return fog.x;
	const float4 g = Levels::GroundAt(Levels::WorldOf(clamp(logical, 0, int(GridRes) - 1)));
	if (g.x <= Levels::NO_GROUND)
		return a_fallback;
	const float upper = abs(g.x - a_height);
	const float lower = g.z > Levels::NO_GROUND ? abs(g.z - a_height) : 1e30;
	return min(upper, lower) > LAYER_STEP ? a_fallback : lower < upper ? fog.y :
	                                                                     fog.x;
}

/** The layer at a_height at a logical position (texel centres at +0.5), bilinear. */
float SampleLayer(float2 position, float a_height, float a_fallback)
{
	float2 p = position - 0.5;
	int2 i = int2(floor(p));
	float2 t = p - float2(i);
	return lerp(lerp(LoadLayer(i, a_height, a_fallback), LoadLayer(i + int2(1, 0), a_height, a_fallback), t.x),
		lerp(LoadLayer(i + int2(0, 1), a_height, a_fallback), LoadLayer(i + int2(1, 1), a_height, a_fallback), t.x), t.y);
}

/** The movers' shove on a layer's fog at a logical cell (world units/s), clamped to the grid. */
float2 Shove(int2 logical, uint a_layer)
{
	const uint2 s = LogicalToStorage(clamp(logical, 0, int(GridRes) - 1));
	return (a_layer == 0 ? float2(SplatShoveX[s], SplatShoveY[s]) : float2(SplatShoveX1[s], SplatShoveY1[s])) / Levels::SPLAT_SCALE;
}

/** One layer's step: carried, softened, shoved, carved, stirred, refilled, and held to the parent at the edge. */
float StepLayer(uint2 id, int2 logical, uint a_layer, float a_height, float a_own, float2 a_parent)
{
	const float2 wind = Velocity[id];
	// Advected by wind, drift and shove; the fog thins where the shove diverges and packs up where it converges
	const float2 position = float2(logical) + 0.5;
	const float2 shove = Shove(logical, a_layer);
	float2 source = position - DeltaTime * (wind * FOG_DRAG + Drift + shove) / CellSize;
	// Never carried out of a wall (D193): the drift and a wake go round rocks and logs, not through them
	if (Levels::Solid(int2(floor(source))) || Levels::Solid(logical))
		source = position;
	float fog = SampleLayer(source, a_height, a_own);
	// Viscous: disturbances soften rather than staying sharp
	const float neighbours = 0.25 * (SampleLayer(source + float2(1, 0), a_height, a_own) + SampleLayer(source - float2(1, 0), a_height, a_own) +
										SampleLayer(source + float2(0, 1), a_height, a_own) + SampleLayer(source - float2(0, 1), a_height, a_own));
	fog = lerp(fog, neighbours, saturate(FOG_DIFFUSION * DeltaTime * 60.0));
	const float divergence = (Shove(logical + int2(1, 0), a_layer).x - Shove(logical - int2(1, 0), a_layer).x + Shove(logical + int2(0, 1), a_layer).y -
								 Shove(logical - int2(0, 1), a_layer).y) /
	                         (2.0 * CellSize);
	fog *= exp(-divergence * DeltaTime);
	// Cut outright where a mover carves it (a blade's path, a blast)
	fog *= 1.0 - (a_layer == 0 ? SplatCarve[id] : SplatCarve1[id]) / Levels::CARVE_SCALE;

	// Thinned where the air is stirred, then filling back in
	const float stirred = saturate(length(wind) / max(StirSpeed, 1.0));
	fog *= exp(-StirRate * stirred * DeltaTime);
	const float resting = FogPattern::Resting((float2(GridMin + logical) + 0.5) * CellSize, PatternOffset);
	fog = resting + (fog - resting) * exp(-RefillRate * DeltaTime);

	if (HasOther != 0)
		fog = lerp(fog, a_layer == 0 ? a_parent.x : a_parent.y, Levels::EdgeWeight(logical) * EDGE_RELAX);
	return clamp(fog, 0.0, MaxFog);
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	const int2 logical = StorageToLogical(id);
	const float2 own = LoadFog(logical);
	const float2 parent = HasOther != 0 ? Levels::SampleOther(ParentFog, Levels::WorldOf(logical)) : 0.0;
	const float4 g = HasWalls != 0 ? Levels::GroundAt(Levels::WorldOf(logical)) : float4(0.0, 0.0, Levels::NO_GROUND, 0.0);
	const float upper = StepLayer(id, logical, 0, g.x, own.x, parent);
	// The floor under an overhang: a layer of its own; elsewhere it follows the ground's
	const float lower = g.z > Levels::NO_GROUND ? StepLayer(id, logical, 1, g.z, own.y, parent) : upper;
	FogOut[id] = float2(upper, lower);
}
