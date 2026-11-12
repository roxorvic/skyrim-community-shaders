// Fluid Field ground fog (D194): a slab of fog on the ground round the player. Its floor is the ground tiles' (8 units),
// its depth the fog solver's column (FogDepthCS); D195: two of them where a column has two levels (under an overhang, at a
// ledge's foot), each sample on the level under it. Within the contact map, fog is cut where something stands in it.
// Fluff: 3D noise carried by the fog's flow and rising slowly breaks the top into clumps; deeper fog billows more, churned
// fog lifts, and a few wisps stray over the top; past the simulated grid, the loaded landscape with the settled depth. Each
// half-resolution pixel marches its view ray to the nearest of its four pixels' surfaces, lit by the sun and ambient
// light, into FogHalf (in-scattered light, transmittance) and FogHalfDistance; the COMPOSITE entry lays that over the scene
// at full resolution, after opaque lighting and before water and effects.

#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "FluidField/LandMap.hlsli"

cbuffer GroundFogCB : register(b0)
{
	int2 GridMin;         // The fog grid: world cell of logical texel (0, 0)
	uint Resolution;      // Its cells per side, a power of two
	float CellSize;       // World units per cell
	float Density;        // Extinction per world unit in the fog
	uint StepCount;       // Samples along each ray
	float MaxDistance;    // Rays stop here, world units
	float MaxDepth;       // Deepest the fog gets (Pool Depth): the band rays are clipped to
	float3 Albedo;        // Colour of the fog's scattering, times its brightness
	float FarDepth;       // Settled depth past the grid where fog may form (Ground Thickness)
	int4 SlotTiles[256];  // Ground tiles: per resident slot, xy the tile there, z 1 once valid
	int2 TileMin;
	int2 LandCell;  // The land map's centre cell (LandMap.hlsli)
	float LandPad;
	float HollowGain;    // Settled depth per unit of hollowness (FogInputsCS)
	float FarPoolDepth;  // Pool Depth, for the settled depth past the grid
	float ContactCell;   // Contact map: world units a texel, world texel of texel (0, 0), texels a side
	int2 ContactMin;
	uint ContactResolution;
	float FarDistance;  // The composite's reach for the sky, world units
	float2 LodOrigin;   // Unused
	uint HasLod;        // The landscape depth is in (the distant fog)
	float LodPad;
	uint HasSunShadow;  // Volumetric Shadows' shared shadow map is in: sun shadows in the fog
	uint HasSky;        // Skylighting's probes are in: sky light in the fog
	uint LocalLights;   // Light Limit Fix's lights glowing in the fog: 0 off, 1 the nearest few, 2 all in the cluster
	float LightPad;
};

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> GroundTiles : register(t1);  // x top height, y flags, z lower height, w flags
Texture2DArray<float4> Column : register(t2);  // Per level: x depth, yz velocity, w churn; toroidal
Texture2D<float> GroundMax : register(t3);     // Per resident tile, its highest top per TOP_TEXEL (GroundTiles::GetMaxSRV)
Texture2DArray<float4> Bed : register(t4);     // Per level: x height, z kind (2 closed)
Texture2D<uint> ContactLow : register(t5);     // Lowest and highest point of what stands over each texel (FogContactCS)
Texture2D<uint> ContactHigh : register(t6);
Texture3D<float> Noise : register(t7);        // Tiling value noise, 8 lattice cells a side: detail
Texture3D<float> ClumpNoise : register(t10);  // 4 cells: clumps
Texture2D<float> BankNoise : register(t11);
Texture2D<float2> LandMap : register(t12);
Texture2D<float> FogTop : register(t15);          // The fog's highest top per TOP_TEXEL round the player, toroidal, max mips (TOP passes)
Texture2D<float> LandscapeDepth : register(t13);  // The depth of the landscape alone (LandscapeDepth), as DepthTexture  // The loaded landscape: x height, y flags (fog may form, water), 32 units a texel   // 2D, 8 cells and 32: banks, near and far
SamplerState LinearSampler : register(s0);
SamplerState WrapSampler : register(s1);

// The lighting CS already has: the sun's shadow (Volumetric Shadows, t18 and t98), the sky's visibility (Skylighting, t14),
// local lights (Light Limit Fix, t35 to t37); and its colour helpers, which match Linear Lighting
#include "Common/ColorManagement.hlsli"
#define SKYLIGHTING_PROBE_REGISTER t14
#include "Skylighting/Skylighting.hlsli"
#define VOLUMETRIC_SHADOWS
#include "Common/ShadowSampling.hlsli"
#include "LightLimitFix/LightLimitFix.hlsli"
#if !defined(COMPOSITE) && !defined(TOP)
RWTexture2D<float4> FogHalf : register(u0);
RWTexture2D<float> FogHalfDistance : register(u1);
#elif defined(COMPOSITE)
Texture2D<float4> FogHalfIn : register(t8);
Texture2D<float> FogHalfDistanceIn : register(t9);
RWTexture2D<float4> MainRW : register(u0);
#endif

/** Henyey-Greenstein lobes: forward toward the sun, a weaker backward one away from it (silver edges), and the backward
 * one's share. */
static const float PHASE_FORWARD = 0.5;
static const float PHASE_BACKWARD = -0.25;
static const float PHASE_BACK_SHARE = 0.3;
/** Light at the bottom of the layer (darker, cooler) and at its top, as a share of the sky light: light scatters in from above. */
static const float3 BOTTOM_LIGHT = float3(0.78, 0.78, 0.8);
static const float3 TOP_LIGHT = float3(1.1, 1.1, 1.1);
/** Fog scatters light many times, washing the sky's colour toward white: the share of the sky's colour kept, and the sun's
 * share scattered in from every direction (not by the phase, not shadowed). */
static const float SKY_SATURATION = 0.3;
static const float SUN_MULTIPLE = 0.06;
/** Local lights in the fog: their strength; how far their glow reaches past their radius and its blend with their own
 * falloff (a softer, wider glow); the most one light gives (as a share of its colour). */
static const float LOCAL_STRENGTH = 0.5;
static const float GLOW_REACH = 1.5;
static const float GLOW_SHARE = 0.6;
static const float GLOW_CAP = 0.6;
/** Local lights: the nearest few at the low setting, at most this many at the high one. */
static const uint FEW_LIGHTS = 4;
static const uint MANY_LIGHTS = 32;
/** Sun's share of the fog's light: low ground fog is mostly sky-lit. */
static const float SUN_SHARE = 0.15;
/** Share of the fog's depth over which its top softens into clear air. */
static const float TOP_SOFTNESS = 0.8;
/** How strongly fog above a point shades it from the sky. */
static const float SELF_SHADING = 1.65;
/** The sun's self-shadow: the fog between a point and the sun, along the sun's slant (its height in the sky, at least this). */
static const float SUN_SELF_SHADING = 1.1;
static const float LOWEST_SUN = 0.35;
/** First step length from each end of a ray, world units: finer than the layer is thick. */
static const float FIRST_STEP = 4.0;
/** Ground texels more than this apart in height are two levels (a ledge's top and its foot): never blended. */
static const float LEVEL_STEP = 24.0;
/** Cells along the grid's edge over which the simulated fog hands over to the settled fog past it. */
static const float EDGE_BAND = 24.0;
/** World units one noise texture tile spans across, and up (clumps flatter than wide). */
static const float NOISE_SPAN = 320.0;
static const float NOISE_SPAN_UP = 160.0;
/** Seconds between restarts of each of the noise's two flow phases, fastest flow carrying it, and its rise. */
static const float FLOW_PERIOD = 2.0;
static const float MAX_FLOW = 250.0;
static const float RISE = 12.0;
/** How much the top rises and falls with the noise in fog Pool Depth deep (puff by depth). */
static const float BILLOW = 0.45;
/** How much the top rises at full churn (LIFT_SPEED). */
static const float LIFT = 0.8;
static const float LIFT_SPEED = 60.0;
/** Share of the fog's height over which it breaks into clumps, and how deep the clumps' gaps go. */
static const float CLUMPING = 0.7;
/** Wisps: how far over the top they stray (times the depth), and their density relative to the fog's. */
static const float WISP_REACH = 1.0;
static const float WISP_DENSITY = 0.35;
/** The fog's motion carries the noise this many times as fast, so its drift reads. */
static const float NOISE_FLOW_GAIN = 6.0;
/** World units one bank noise tile spans; how much the banks thin and thicken the fog; their drift, world units a second. */
static const float BANK_SPAN = 8000.0;
static const float2 BANK_RANGE = float2(0.45, 1.3);
static const float BANK_DRIFT = 8.0;
/** Most a ray's end corrects its ground map by, and how far round it the correction reaches, world units. */
static const float GROUND_FIX = 16.0;
static const float GROUND_FIX_REACH = 200.0;
/** The distant fog: the longest slant path through it, in thicknesses; its density as a share of the near fog's (the near
 * fog's soft top averages about half); how far off the LOD ground a surface may lie and still be it, world units (more
 * far away, where the map's 128-unit texels average the hills). */
static const float LOD_PATH = 4.0;
static const float LOD_DENSITY = 0.5;
/** The fluff's furthest reach over the deepest fog: the band rays are clipped to. */
static const float FLUFF_REACH = 1.0 + BILLOW + LIFT + WISP_REACH;
/** Share of MaxDistance over which the fog fades out at the end of the rays. */
static const float DISTANCE_FADE = 0.2;
static const float TILE_TEXEL = 8.0;
static const int TILE_SLOTS = 16;
static const uint TILE_MAY_FORM = 1, TILE_ROCK = 2, TILE_STEEP = 4, TILE_WATER = 8;
/** The lower level's flags carry the gap up to the overhang's underside: flags + gap * GAP_SCALE (GroundTiles). */
static const float GAP_SCALE = 32.0;
/** Fog under an overhang stays this far under its underside, fading over it. */
static const float CEILING_MARGIN = 6.0;
static const float NO_GROUND = -1.0e6;
/** World units over which a contact cut fades at its top and bottom. */
static const float CONTACT_SOFTNESS = 4.0;

/** Growth factor r for which `count` steps starting at 1 cover `ratio` (1 + r + ... + r^(count-1) = ratio). */
float StepGrowth(float ratio, uint count)
{
	if (ratio <= float(count))
		return 1.0;
	// Bisection between 1 and a bound past the root (Newton from near 1 can settle on the trivial root r = 1)
	float lo = 1.0;
	float hi = 2.0 * pow(abs(ratio), 1.0 / float(count - 1));
	[unroll] for (int k = 0; k < 20; k++)
	{
		const float r = 0.5 * (lo + hi);
		if (pow(abs(r), count) - ratio * (r - 1.0) - 1.0 > 0.0)
			hi = r;
		else
			lo = r;
	}
	return hi;
}

float HenyeyGreenstein(float cosTheta, float g)
{
	const float g2 = g * g;
	return (1.0 - g2) / (4.0 * Math::PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

/** The middle tier where no tile is resident: the loaded landscape, bilinear, and its share fog may form on (not water). */
float LandGround(float2 xy, out float forming)
{
	float water;
	return LandHeight(LandMap, LandCell, xy, forming, water);
}

/** A ground texel (top height, flags, lower height, flags), or NO_GROUND where its tile is not resident yet. */
float4 Texel(int2 texel)
{
	const int2 tile = texel >> 7;
	const int4 slot = SlotTiles[(tile.y & (TILE_SLOTS - 1)) * TILE_SLOTS + (tile.x & (TILE_SLOTS - 1))];
	if (any(tile < TileMin) || any(tile >= TileMin + TILE_SLOTS) || slot.z == 0 || any(slot.xy != tile))
		return float4(NO_GROUND, 0.0, NO_GROUND, 0.0);
	return GroundTiles.Load(int3(texel & (TILE_SLOTS * 128 - 1), 0));
}

/** Of a texel's levels, the one under height z: its floor under an overhang when z is below the top's (D195). */
float2 LevelUnder(float4 g, float z)
{
	return g.z > NO_GROUND && z < g.x - 0.5 * LEVEL_STEP ? g.zw : g.xy;
}

/** Of a texel's levels, the one whose height is nearest a reference. */
float2 LevelNearest(float4 g, float reference)
{
	return g.z > NO_GROUND && abs(g.z - reference) < abs(g.x - reference) ? g.zw : g.xy;
}

/**
 * Ground under a position at height z: the level under it of the nearest texel, bilinear over the four nearest texels'
 * levels nearest that (those a level away left out: no slope hung across a ledge). forming: their share fog may form on;
 * carries: their share neither rock nor steep (an 8-unit map cannot follow a rock's face: fog drawn on it flickers in
 * and out of the mesh); ceiling: the overhang's underside over the floor under it (very high in the open). The loaded
 * landscape where no tile is resident.
 */
float Ground(float3 position, out float forming, out float carries, out float ceiling)
{
	const float2 t = position.xy / TILE_TEXEL - 0.5;
	const int2 base = int2(floor(t));
	const float2 f = t - float2(base);
	const float4 nearestTexel = Texel(int2(round(t)));
	const float reference = LevelUnder(nearestTexel, position.z).x;
	const bool underOverhang = nearestTexel.z > NO_GROUND && reference == nearestTexel.z;
	ceiling = underOverhang ? nearestTexel.z + floor(nearestTexel.w / GAP_SCALE) : 1e30;
	float height = 0.0, weights = 0.0, form = 0.0, carry = 0.0;
	[unroll] for (uint k = 0; k < 4; k++)
	{
		const int2 offset = int2(k & 1, k >> 1);
		const float2 g = LevelNearest(Texel(base + offset), reference);
		const float w = (offset.x ? f.x : 1.0 - f.x) * (offset.y ? f.y : 1.0 - f.y) * (abs(g.x - reference) < LEVEL_STEP ? 1.0 : 0.0);
		height += g.x * w;
		form += ((uint(g.y) & TILE_MAY_FORM) ? 1.0 : 0.0) * w;
		carry += ((uint(g.y) & (TILE_ROCK | TILE_STEEP | TILE_WATER)) ? 0.0 : 1.0) * w;
		weights += w;
	}
	const bool resident = nearestTexel.x > NO_GROUND;
	float landForming;
	const float land = resident ? NO_GROUND : LandGround(position.xy, landForming);
	forming = resident ? form / max(weights, 1e-4) : landForming;
	carries = resident ? carry / max(weights, 1e-4) : landForming;
	return !resident ? land : weights > 1e-4 ? height / weights :
	                                           reference;
}

float Banks(float2 xy);

/**
 * The fog over ground of the given height (x depth, yz velocity, w churn): the column (bilinear) of each nearest cell's
 * level whose floor is that ground's (D195; cells a level away left out), handing over to the settled depth at the grid's
 * edge.
 */
float4 FogColumn(float2 xy, float ground, float forming)
{
	const float2 c = xy / CellSize - 0.5;
	const float2 logical = c - float2(GridMin);
	const float fromEdge = min(min(logical.x, logical.y), min(float(Resolution) - 1.0 - logical.x, float(Resolution) - 1.0 - logical.y));
	const float settled = FarDepth * forming * Banks(xy);
	if (fromEdge < 0.0)
		return float4(settled, 0.0, 0.0, 0.0);
	const int2 base = int2(floor(c));
	const float2 f = c - float2(base);
	const uint mask = Resolution - 1;
	float4 column = 0.0;
	float weights = 0.0;
	[unroll] for (uint k = 0; k < 4; k++)
	{
		const int2 offset = int2(k & 1, k >> 1);
		const uint2 texel = uint2(base + offset) & mask;
		const float4 top = Bed[uint3(texel, 0)], lower = Bed[uint3(texel, 1)];
		// A cell's bed is the highest floor under it, so it may stand up to a level over this ground
		const uint level = lower.z != 2.0 && abs(lower.x - ground) < abs(top.x - ground) ? 1 : 0;
		const float floorHeight = level == 1 ? lower.x : top.x;
		const float w = (offset.x ? f.x : 1.0 - f.x) * (offset.y ? f.y : 1.0 - f.y) * (floorHeight - ground < LEVEL_STEP && ground - floorHeight < LEVEL_STEP ? 1.0 : 0.0);
		column += Column[uint3(texel, level)] * w;
		weights += w;
	}
	const float4 simulated = weights > 1e-4 ? column / weights : 0.0;
	const float edge = saturate(fromEdge / EDGE_BAND);
	return float4(lerp(settled, simulated.x, edge), simulated.yzw * edge);
}

/** Noise 0 to 1 at a position, carried by the fog's flow: two phases half a period apart, cross-faded so no restart shows. */
float FlowNoise(float3 position, float2 velocity)
{
	const float speed = length(velocity);
	const float2 flow = velocity * NOISE_FLOW_GAIN * min(1.0, MAX_FLOW / max(speed * NOISE_FLOW_GAIN, 1e-3)) * FLOW_PERIOD;
	const float time = SharedData::Timer / FLOW_PERIOD;
	const float phaseA = frac(time), phaseB = frac(time + 0.5);
	const float3 span = float3(NOISE_SPAN, NOISE_SPAN, NOISE_SPAN_UP);
	const float3 p = (position - float3(0.0, 0.0, SharedData::Timer * RISE)) / span;
	const float3 a = p - float3(flow * phaseA, 0.0) / span;
	const float3 b = p - float3(flow * phaseB, 0.0) / span + 0.37;
	// Clumps at the noise's span, detail at a fifth of it
	const float noiseA = ClumpNoise.SampleLevel(WrapSampler, a, 0) * 0.6 + Noise.SampleLevel(WrapSampler, a * 2.3 + 0.13, 0) * 0.4;
	const float noiseB = ClumpNoise.SampleLevel(WrapSampler, b, 0) * 0.6 + Noise.SampleLevel(WrapSampler, b * 2.3 + 0.13, 0) * 0.4;
	return lerp(noiseA, noiseB, abs(phaseA * 2.0 - 1.0));
}

/** Banks: where the fog lies thicker or thinner over hundreds of metres, drifting slowly. */
float Banks(float2 xy)
{
	return lerp(BANK_RANGE.x, BANK_RANGE.y, BankNoise.SampleLevel(WrapSampler, (xy + SharedData::Timer * BANK_DRIFT) / BANK_SPAN, 0));
}

/** How much of the fog at a position is left by what stands in it (the contact map): 0 inside, 1 clear of it. */
float Uncut(float3 position)
{
	const int2 texel = int2(floor(position.xy / ContactCell)) - ContactMin;
	if (any(texel < 0) || any(texel >= int(ContactResolution)))
		return 1.0;
	const uint low = ContactLow[texel], high = ContactHigh[texel];
	if (low > high)
		return 1.0;
	const float bottom = float(low) / 16.0 - 131072.0, top = float(high) / 16.0 - 131072.0;
	return 1.0 - smoothstep(-CONTACT_SOFTNESS, 0.0, position.z - bottom) * (1.0 - smoothstep(0.0, CONTACT_SOFTNESS, position.z - top));
}

/** The fog top map (TOP passes): texels of TOP_TEXEL, TOP_TEXELS a side round the land map's centre cell, TOP_MIPS max mips. */
static const float TOP_TEXEL = 64.0;
static const int TOP_TEXELS = 512;
static const uint TOP_MIPS = 7;
static const float TOP_NONE = -1.0e30;
/** Probes along a ray for its band's top. */
static const uint TOP_PROBES = 8;

/** World texel of the top map's window's south-west corner. */
int2 TopMin()
{
	const int perCell = int(LAND_CELL / TOP_TEXEL);
	return LandCell * perCell + perCell / 2 - TOP_TEXELS / 2;
}

/** The fog's highest top over the mip texel holding xy. */
float FogTopAt(float2 xy, uint level)
{
	const int2 texel = int2(floor(xy / TOP_TEXEL)) >> level;
	return FogTop.Load(int3(texel & ((TOP_TEXELS >> level) - 1), level));
}

/** The surface a ray ends on, and how far its ground map is off there (the landscape is drawn as triangles, sampled
 * bilinearly): samples near the end take the surface's height (GROUND_FIX). */
static float3 rayEnd = 0.0;
static float groundError = 0.0;

/** Fog extinction per world unit at a position, the depth of fog over it (its self-shadow), and how high in the layer it lies (0 bottom, 1 top). */
float FogAt(float3 position, out float shading, out float upInLayer)
{
	shading = 1.0;
	upInLayer = 0.0;
	float forming, carries, ceiling;
	const float ground = Ground(position, forming, carries, ceiling) + groundError * exp(-distance(position.xy, rayEnd.xy) / GROUND_FIX_REACH);
	const float height = position.z - ground;
	if (ground <= NO_GROUND || height < 0.0 || height > MaxDepth * FLUFF_REACH)
		return 0.0;
	const float4 column = FogColumn(position.xy, ground, forming);
	const float depth = column.x;
	if (depth <= 0.5 || height > depth * FLUFF_REACH)
		return 0.0;
	// The top: billowing more where the fog is deep, lifted where it churns
	const float noise = FlowNoise(position, column.yz);
	const float puff = saturate(depth / MaxDepth);
	// Under an overhang, never into its underside
	const float top = min(depth * (1.0 + BILLOW * puff * (noise * 2.0 - 1.0) + LIFT * saturate(column.w / LIFT_SPEED)), ceiling - CEILING_MARGIN - ground);
	if (top <= 0.5)
		return 0.0;
	// Dense at the bottom, breaking into clumps toward the top; a few wisps stray over it
	const float up = saturate(height / max(top, 1.0));
	upInLayer = up;
	const float clumps = lerp(1.0, saturate(noise * 2.2 - 0.6), CLUMPING * up);
	const float body = (1.0 - smoothstep(top * (1.0 - TOP_SOFTNESS), top, height)) * clumps;
	const float wisps = WISP_DENSITY * (puff + saturate(column.w / LIFT_SPEED)) * saturate(noise * 4.0 - 2.6) * (1.0 - smoothstep(top, top * (1.0 + WISP_REACH), height));
	// The fog's own depth over it, not the lifted top (lifted wisps and clumps are thin: counted whole, wakes went dark)
	shading = max(min(depth, top) - height, 0.0);
	return Density * carries * max(body, wisps) * Uncut(position) * Banks(position.xy);
}

/**
 * Marches the ray between two distances with StepCount / 2 steps growing from FIRST_STEP at the start (the far end when
 * a_fromEnd). Returns in-scattered light (rgb) and transmittance (a).
 */
/** The sun reaching a point (Volumetric Shadows' shadow map), 1 where it is not in. Camera-relative position. */
float SunVisibility(float3 positionCR)
{
	if (HasSunShadow == 0)
		return 1.0;
	float detailed;
	return VolumetricShadows::GetVSMShadow2D(positionCR, detailed);
}

/** The open sky a point sees (Skylighting's probes), 1 where they are not in. Camera-relative position. */
float SkyVisibility(float3 positionCR)
{
	if (HasSky == 0)
		return 1.0;
	return Skylighting::MixDiffuse(Skylighting::EvaluateVisibility(Skylighting::SampleNoBias(positionCR), Skylighting::GetFadeOutFactor(positionCR)));
}

/** Local lights (Light Limit Fix's clusters) at a camera-relative point seen at uv: the nearest few or all in its cluster. */
float3 LocalLight(float3 positionCR, float2 uv)
{
	uint clusterIndex = 0;
	if (LocalLights == 0 || !LightLimitFix::GetClusterIndex(uv, length(positionCR), clusterIndex))
		return 0.0;
	const LightLimitFix::LightGrid grid = LightLimitFix::lightGrid[clusterIndex];
	const uint count = min(grid.lightCount, LocalLights == 1 ? FEW_LIGHTS : MANY_LIGHTS);
	float3 sum = 0.0;
	[loop] for (uint i = 0; i < count; i++)
	{
		const LightLimitFix::Light light = LightLimitFix::lights[LightLimitFix::lightList[grid.offset + i]];
		const float distance = length(light.positionWS.xyz - positionCR);
		const float edge = saturate(1.0 - distance / (light.radius * GLOW_REACH));
		const float glow = edge * edge;
		const float falloff = min(lerp(LightLimitFix::GetAttenuation(distance, light), glow, GLOW_SHARE), GLOW_CAP);
		sum += Color::PointLight(light.color) * falloff * LOCAL_STRENGTH;
	}
	return sum;
}

/**
 * Marches the ray between two distances, as below; a_sun and a_sky are the light per unit visibility (the phase and the
 * albedo in), shaded per sample. Also sums where along the ray the fog scatters (for the local lights): o_where by weight.
 */
float4 MarchHalf(float3 camera, float3 direction, float from, float to, bool a_fromEnd, float jitter, float3 a_sun, float3 a_sky, inout float4 o_where)
{
	const float span = to - from;
	const uint count = max(StepCount / 2, 1u);
	if (span <= 0.0)
		return float4(0.0, 0.0, 0.0, 1.0);
	const float firstStep = min(FIRST_STEP, span / count);
	const float growth = StepGrowth(span / firstStep, count);
	float transmittance = 1.0;
	float3 inscattering = 0.0;
	float travelled = 0.0;
	float stepLength = firstStep;
	[loop] for (uint i = 0; i < count && travelled < span; i++)
	{
		// The last step always reaches the end, whatever the growth
		const float segment = i + 1 == count ? span - travelled : min(stepLength, span - travelled);
		const float along = travelled + segment * jitter;
		travelled += segment;
		stepLength *= growth;
		const float distance = a_fromEnd ? to - along : from + along;
		const float3 position = camera + direction * distance;
		float shading, upInLayer;
		const float extinction = FogAt(position, shading, upInLayer) * (1.0 - smoothstep(MaxDistance * (1.0 - DISTANCE_FADE), MaxDistance, distance));
		if (extinction <= 0.0)
			continue;
		const float stepTransmittance = exp(-extinction * segment);
		const float3 positionCR = position - FrameBuffer::CameraPosAdjust.xyz;
		// Self-shadow: the sky dimmed by the fog straight above, the sun by the fog toward it along its slant
		const float above = Density * shading;
		const float sunShade = exp(-SUN_SELF_SHADING * above / max(SharedData::DirLightDirection.z, LOWEST_SUN));
		const float skyShade = exp(-SELF_SHADING * above);
		const float3 light = a_sun * SunVisibility(positionCR) * sunShade + a_sky * SkyVisibility(positionCR) * skyShade * lerp(BOTTOM_LIGHT, TOP_LIGHT, upInLayer);
		const float3 scattered = (1.0 - stepTransmittance) * light;
		const float weight = (1.0 - stepTransmittance) * transmittance;
		o_where += float4(positionCR * weight, weight);
		// Marching back to front, each step sits in front of everything marched so far
		inscattering = a_fromEnd ? inscattering * stepTransmittance + scattered : inscattering + transmittance * scattered;
		transmittance *= stepTransmittance;
	}
	return float4(inscattering, transmittance);
}

/** This pixel's landscape distance (LandscapeDepth), set before FogOverRay. */
static float landscapeDistance = 1e30;
/** How flat the landscape is at this pixel, 1 flat to 0 steeper than about 50 degrees (no ground fog on a cliff). */
static float landscapeFlat = 1.0;

/**
 * The distant fog past the loaded landscape: a layer of Ground Thickness (in banks) on the surface the pixel shows, where
 * that surface lies on the LOD terrain (not water, not treetops or rock tops). One lookup, no march. Fades in as the near
 * fog fades out.
 */
float4 DistantFog(float depth, float3 direction, float surfaceDistance, float3 light)
{
	if (depth >= 1.0 || HasLod == 0)
		return float4(0.0, 0.0, 0.0, 1.0);
	const float distance = surfaceDistance;
	const float weight = smoothstep(MaxDistance * (1.0 - DISTANCE_FADE), MaxDistance, distance) * (1.0 - smoothstep(FarDistance * 0.85, FarDistance, distance));
	if (weight <= 0.0)
		return float4(0.0, 0.0, 0.0, 1.0);
	const float3 surface = FrameBuffer::CameraPosAdjust.xyz + direction * distance;
	// Landscape where the scene's depth is the landscape's own: trees and rocks stand in front, water lies over its bed
	const float tolerance = max(16.0, distance * 0.005);
	const float onGround = (1.0 - smoothstep(tolerance, 2.0 * tolerance, abs(distance - landscapeDistance))) * landscapeFlat;
	const float thickness = FarDepth * Banks(surface.xy);
	const float path = min(thickness / max(abs(direction.z), 0.02), LOD_PATH * thickness);
	const float transmittance = exp(-LOD_DENSITY * Density * path * weight * onGround);
	return float4((1.0 - transmittance) * light, transmittance);
}

/** The fog over one view ray to a surface (camera-relative), or to MaxDistance where depth is the sky. */
float4 FogOverRay(float depth, float2 uv, float2 pixel)
{
	float4 positionCS = mul(FrameBuffer::CameraViewProjInverse, float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1));
	const float3 surface = positionCS.xyz / positionCS.w;
	const float surfaceDistance = length(surface);
	const float3 direction = surface / max(surfaceDistance, 1e-3);
	const float rayLength = min(depth >= 1.0 ? MaxDistance : surfaceDistance, MaxDistance);
	const float jitter = Random::InterleavedGradientNoise(pixel, SharedData::FrameCount);

	// Light: the sun scattered toward the eye (forward and backward lobes), and the sky's average, through CS's colour helpers
	const float cosTheta = dot(direction, SharedData::DirLightDirection.xyz);
	const float phase = lerp(HenyeyGreenstein(cosTheta, PHASE_FORWARD), HenyeyGreenstein(cosTheta, PHASE_BACKWARD), PHASE_BACK_SHARE) * 4.0 * Math::PI;
	const float3 sunLight = Albedo * Color::DirectionalLight(SharedData::DirLightColor.xyz) * phase * SUN_SHARE * (1.0 - SharedData::InInterior);
	const float3 sky = Color::Ambient(max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886);
	const float3 sunColour = Color::DirectionalLight(SharedData::DirLightColor.xyz) * (1.0 - SharedData::InInterior);
	const float3 skyLight = Albedo * (lerp(Color::RGBToLuminance(sky).xxx, sky, SKY_SATURATION) + sunColour * SUN_MULTIPLE);
	const float3 light = sunLight + skyLight;

	// The LOD fog behind the simulated fog
	const float4 none = DistantFog(depth, direction, surfaceDistance, light);

	// Clip the ray to the band under the fog's highest top along it: probes on the mip whose texels span their spacing
	const float3 camera = FrameBuffer::CameraPosAdjust.xyz;
	const float3 endPoint = camera + direction * rayLength;
	float formingB, carriesB, ceilingB;
	const float endGround = Ground(endPoint + float3(0.0, 0.0, 1.0), formingB, carriesB, ceilingB);
	// A surface just over or under the ground map (not a wall or a tree): the map is that far off there
	rayEnd = endPoint;
	groundError = depth < 1.0 && abs(endPoint.z - endGround) < GROUND_FIX ? endPoint.z - endGround : 0.0;
	const float probeSpan = rayLength * length(direction.xy) / TOP_PROBES;
	const uint level = min(uint(ceil(log2(max(probeSpan / TOP_TEXEL, 1.0)))), TOP_MIPS - 1);
	float bandTop = TOP_NONE;
	[loop] for (uint p = 0; p <= TOP_PROBES; p++)
		bandTop = max(bandTop, FogTopAt(camera.xy + direction.xy * (rayLength * p / TOP_PROBES), level));
	if (rayLength <= 0.0 || bandTop <= TOP_NONE || min(camera.z, endPoint.z) > bandTop)
		return none;
	float rayStart = 0.0;
	float rayEnd = rayLength;
	if (camera.z > bandTop)
		rayStart = (camera.z - bandTop) / max(-direction.z, 1e-4);
	if (endPoint.z > bandTop)
		rayEnd = (bandTop - camera.z) / max(direction.z, 1e-4);
	rayStart = clamp(rayStart, 0.0, rayLength);
	rayEnd = clamp(rayEnd, 0.0, rayLength);
	if (rayEnd <= rayStart)
		return none;
	const float span = rayEnd - rayStart;

	// Both ends can be in the fog (camera and hit ground), so each half marches from its own end with fine first steps
	const float middle = rayStart + 0.5 * span;
	float4 where = 0.0;
	const float4 nearHalf = MarchHalf(camera, direction, rayStart, middle, false, jitter, sunLight, skyLight, where);
	const float4 farHalf = MarchHalf(camera, direction, middle, rayEnd, true, jitter, sunLight, skyLight, where);
	float4 near = float4(nearHalf.rgb + nearHalf.a * farHalf.rgb, nearHalf.a * farHalf.a);
	// Local lights: once a ray, where the fog along it scatters most
	if (LocalLights != 0 && where.w > 1e-4)
		near.rgb += Albedo * LocalLight(where.xyz / where.w, uv) * (1.0 - near.a);
	return float4(near.rgb + near.a * none.rgb, near.a * none.a);
}

/** Distance from the camera to a pixel's surface (FarDistance for the sky), as the composite reads it. */
float SurfaceDistance(float depth, float2 uv)
{
	float4 positionCS = mul(FrameBuffer::CameraViewProjInverse, float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1));
	return depth >= 1.0 ? FarDistance : min(length(positionCS.xyz / positionCS.w), FarDistance);
}

#if defined(TOP)
// The fog top map, for the rays' bounds. TOP 0: each texel's highest fog top: the simulated fog over its cells' beds, and
// where the settled fog may lie (in the grid's edge band and past it), the highest ground plus the settled fog's deepest;
// over that, the most the ground map and the ray's end may shift it. TOP 1: dilated a texel (the march's bilinear reach
// into the neighbours). TOP 2: one max mip from the one before.
RWTexture2D<float> TopOut : register(u0);
Texture2D<float> TopIn : register(t16);

#	if TOP == 0
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= uint(TOP_TEXELS)))
		return;
	const int2 topMin = TopMin();
	const int2 world = topMin + ((int2(id.xy) - topMin) & (TOP_TEXELS - 1));
	const float2 lo = float2(world) * TOP_TEXEL, hi = lo + TOP_TEXEL;
	const float2 off = max(max(lo - FrameBuffer::CameraPosAdjust.xy, FrameBuffer::CameraPosAdjust.xy - hi), 0.0);
	if (length(off) > MaxDistance) {
		TopOut[id.xy] = TOP_NONE;
		return;
	}
	float top = TOP_NONE;
	const int2 cell0 = int2(floor(lo / CellSize)), cell1 = int2(floor((hi - 0.01) / CellSize));
	[loop] for (int y = max(cell0.y, GridMin.y); y <= min(cell1.y, GridMin.y + int(Resolution) - 1); y++)
	{
		[loop] for (int x = max(cell0.x, GridMin.x); x <= min(cell1.x, GridMin.x + int(Resolution) - 1); x++)
		{
			const uint2 texel = uint2(int2(x, y)) & (Resolution - 1);
			[unroll] for (uint level = 0; level < 2; level++)
			{
				const float4 bed = Bed[uint3(texel, level)];
				const float depth = Column[uint3(texel, level)].x;
				if (bed.z != 2.0 && depth > 0.5)
					top = max(top, bed.x + depth * FLUFF_REACH);
			}
		}
	}
	// Wholly inside the edge band (FogColumn's fromEdge), only the simulated fog lies here
	const int edge = int(EDGE_BAND) + 1;
	if (any(cell0 < GridMin + edge) || any(cell1 > GridMin + int(Resolution) - 1 - edge)) {
		// A texel lies in one ground tile
		float ground = TOP_NONE;
		const int2 tile = world >> 4;
		const int4 slot = SlotTiles[(tile.y & (TILE_SLOTS - 1)) * TILE_SLOTS + (tile.x & (TILE_SLOTS - 1))];
		const bool missing = any(tile < TileMin) || any(tile >= TileMin + TILE_SLOTS) || slot.z == 0 || any(slot.xy != tile);
		if (!missing)
			ground = GroundMax.Load(int3(world & (TILE_SLOTS * 16 - 1), 0));
		if (missing && all(abs(int2(floor(lo / LAND_CELL)) - LandCell) <= LAND_REACH)) {
			const int2 l0 = int2(floor(lo / LAND_TEXEL)), l1 = int2(floor((hi - 0.01) / LAND_TEXEL));
			[loop] for (int ly = l0.y; ly <= l1.y; ly++)
			{
				[loop] for (int lx = l0.x; lx <= l1.x; lx++)
					ground = max(ground, LandMap.Load(int3(int2(lx, ly) & (LAND_TEXELS - 1), 0)).x);
			}
		}
		if (ground > NO_GROUND)
			top = max(top, ground + FarDepth * BANK_RANGE.y * FLUFF_REACH);
	}
	TopOut[id.xy] = top > TOP_NONE ? top + LEVEL_STEP + GROUND_FIX : TOP_NONE;
}
#	elif TOP == 1
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= uint(TOP_TEXELS)))
		return;
	float top = TOP_NONE;
	[unroll] for (int y = -1; y <= 1; y++)
	{
		[unroll] for (int x = -1; x <= 1; x++)
			top = max(top, TopIn[uint2(int2(id.xy) + int2(x, y)) & (TOP_TEXELS - 1)]);
	}
	TopOut[id.xy] = top;
}
#	else
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint width, height;
	TopOut.GetDimensions(width, height);
	if (any(id.xy >= width))
		return;
	const uint2 from = id.xy * 2;
	TopOut[id.xy] = max(max(TopIn[from], TopIn[from + uint2(1, 0)]), max(TopIn[from + uint2(0, 1)], TopIn[from + 1]));
}
#	endif
#elif !defined(COMPOSITE)
/** The landscape's surface at a full-resolution pixel (LandscapeDepth), camera-relative. */
float3 LandscapePoint(uint2 pixel, uint2 size)
{
	float2 uv = (float2(pixel) + 0.5) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
	const float4 positionCS = mul(FrameBuffer::CameraViewProjInverse, float4(2 * float2(uv.x, -uv.y + 1) - 1, LandscapeDepth[pixel], 1));
	return positionCS.xyz / positionCS.w;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const uint2 size = uint2(SharedData::BufferDim.xy);
	const uint2 halfSize = (size + 1) / 2;
	if (any(dispatchID.xy >= halfSize))
		return;

	// The nearest of its four pixels: fog marched to a far surface would spill over a near one's edge
	uint2 pixel = min(dispatchID.xy * 2, size - 1);
	float depth = DepthTexture[pixel];
	[unroll] for (uint k = 1; k < 4; k++)
	{
		const uint2 other = min(dispatchID.xy * 2 + uint2(k & 1, k >> 1), size - 1);
		const float otherDepth = DepthTexture[other];
		if (otherDepth < depth) {
			depth = otherDepth;
			pixel = other;
		}
	}

	float2 uv = float2(pixel + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;
	if (HasLod != 0) {
		// The landscape here and two pixels across and down: its distance, and its slope from the three
		const float3 land[3] = { LandscapePoint(pixel, size), LandscapePoint(min(pixel + uint2(2, 0), size - 1), size), LandscapePoint(min(pixel + uint2(0, 2), size - 1), size) };
		landscapeDistance = LandscapeDepth[pixel] >= 1.0 ? 1e30 : length(land[0]);
		const float3 normal = cross(land[1] - land[0], land[2] - land[0]);
		landscapeFlat = smoothstep(0.64, 0.87, abs(normal.z) / max(length(normal), 1e-6));
	}
	FogHalf[dispatchID.xy] = FogOverRay(depth, uv, float2(dispatchID.xy));
	FogHalfDistance[dispatchID.xy] = SurfaceDistance(depth, uv);
}
#else
/** Relative distance difference over which a half-resolution texel stops counting for a pixel (an edge between surfaces). */
static const float EDGE_TOLERANCE = 0.05;
/** The blur's Gaussian spread, in half-resolution texels squared. */
static const float BLUR_SPREAD = 1.2;

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const uint2 size = uint2(SharedData::BufferDim.xy);
	if (any(dispatchID.xy >= size))
		return;

	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;
	const float distance = SurfaceDistance(DepthTexture[dispatchID.xy], uv);

	// A 3 x 3 Gaussian over the nearest half-resolution texels (the march's jitter averages out), each weighed down by how far
	// its surface lies from this pixel's
	const int2 halfMax = int2((size + 1) / 2) - 1;
	const float2 position = (float2(dispatchID.xy) + 0.5) * 0.5 - 0.5;
	const int2 centre = int2(floor(position + 0.5));
	float4 fog = 0.0;
	float weights = 0.0;
	float nearestGap = 1e30;
	float4 nearest = float4(0.0, 0.0, 0.0, 1.0);
	[unroll] for (int dy = -1; dy <= 1; dy++)
	{
		[unroll] for (int dx = -1; dx <= 1; dx++)
		{
			const int2 texel = clamp(centre + int2(dx, dy), 0, halfMax);
			const float4 sample = FogHalfIn[texel];
			const float gap = abs(FogHalfDistanceIn[texel] - distance) / max(distance, 1.0);
			const float2 offset = float2(centre + int2(dx, dy)) - position;
			const float w = exp(-dot(offset, offset) / BLUR_SPREAD) * saturate(1.0 - gap / EDGE_TOLERANCE);
			fog += sample * w;
			weights += w;
			if (gap < nearestGap) {
				nearestGap = gap;
				nearest = sample;
			}
		}
	}
	// No texel on this pixel's surface (a thin edge): the closest in distance
	fog = weights > 1e-3 ? fog / weights : nearest;
	if (fog.a >= 0.999)
		return;
	const float4 scene = MainRW[dispatchID.xy];
	MainRW[dispatchID.xy] = float4(scene.rgb * fog.a + fog.rgb, scene.a);
}
#endif
