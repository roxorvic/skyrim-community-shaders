// Fluid Field ground fog (D194): a slab of fog on the ground round the player. Its floor is the ground tiles' (8 units),
// its depth the fog solver's column (FogDepthCS); D195: two of them where a column has two levels (under an overhang, at a
// ledge's foot), each sample on the level under it. Within the contact map, fog is cut where something stands in it.
// Fluff: 3D noise carried by the fog's flow and rising slowly breaks the top into clumps; deeper fog billows more, churned
// fog lifts, and a few wisps stray over the top; past the simulated grid, the far height map with the settled depth. Each
// half-resolution pixel marches its view ray to the nearest of its four pixels' surfaces, lit by the sun and ambient
// light, into FogHalf (in-scattered light, transmittance) and FogHalfDistance; the COMPOSITE entry lays that over the scene
// at full resolution, after opaque lighting and before water and effects.

#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

cbuffer GroundFogCB : register(b0)
{
	int2 GridMin;        // The fog grid: world cell of logical texel (0, 0)
	uint Resolution;     // Its cells per side, a power of two
	float CellSize;      // World units per cell
	float Density;       // Extinction per world unit in the fog
	uint StepCount;      // Samples along each ray
	float MaxDistance;   // Rays stop here, world units
	float MaxDepth;      // Deepest the fog gets (Pool Depth): the band rays are clipped to
	float3 Albedo;       // Colour of the fog's scattering, times its brightness
	float FarDepth;      // Settled depth past the grid where fog may form (Ground Thickness)
	int4 SlotTiles[64];  // Ground tiles: per resident slot, xy the tile there, z 1 once valid
	int2 TileMin;
	float2 FarScale;  // Far height map: uv = xy * FarScale + FarOffset, height = lerp(FarZ.x, FarZ.y, texel)
	float2 FarOffset;
	float2 FarZ;
	uint HasFar;
	float HollowGain;    // Settled depth per unit of hollowness (FogInputsCS)
	float FarPoolDepth;  // Pool Depth, for the settled depth past the grid
	float ContactCell;   // Contact map: world units a texel, world texel of texel (0, 0), texels a side
	int2 ContactMin;
	uint ContactResolution;
	float Pad;
};

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> GroundTiles : register(t1);  // x top height, y flags, z lower height, w flags
Texture2DArray<float4> Column : register(t2);  // Per level: x depth, yz velocity, w churn; toroidal
Texture2D<float> FarHeight : register(t3);
Texture2DArray<float4> Bed : register(t4);  // Per level: x height, z kind (2 closed)
Texture2D<uint> ContactLow : register(t5);  // Lowest and highest point of what stands over each texel (FogContactCS)
Texture2D<uint> ContactHigh : register(t6);
Texture3D<float> Noise : register(t7);  // Tiling value noise, NOISE_CELLS lattice cells a side
SamplerState LinearSampler : register(s0);
SamplerState WrapSampler : register(s1);
#ifndef COMPOSITE
RWTexture2D<float4> FogHalf : register(u0);
RWTexture2D<float> FogHalfDistance : register(u1);
#else
Texture2D<float4> FogHalfIn : register(t8);
Texture2D<float> FogHalfDistanceIn : register(t9);
RWTexture2D<float4> MainRW : register(u0);
#endif

/** Henyey-Greenstein g: mist glows a little toward the sun. */
static const float PHASE_G = 0.25;
/** Sun's share of the fog's light: low ground fog is mostly sky-lit. */
static const float SUN_SHARE = 0.15;
/** Share of the fog's depth over which its top softens into clear air. */
static const float TOP_SOFTNESS = 0.8;
/** How strongly fog above a point shades it from the sky. */
static const float SELF_SHADING = 1.0;
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
static const float MAX_FLOW = 150.0;
static const float RISE = 6.0;
/** How much the top rises and falls with the noise in fog Pool Depth deep (puff by depth). */
static const float BILLOW = 0.45;
/** How much the top rises at full churn (LIFT_SPEED). */
static const float LIFT = 0.6;
static const float LIFT_SPEED = 120.0;
/** Share of the fog's height over which it breaks into clumps, and how deep the clumps' gaps go. */
static const float CLUMPING = 0.7;
/** Wisps: how far over the top they stray (times the depth), and their density relative to the fog's. */
static const float WISP_REACH = 0.6;
static const float WISP_DENSITY = 0.25;
/** The fluff's furthest reach over the deepest fog: the band rays are clipped to. */
static const float FLUFF_REACH = 1.0 + BILLOW + LIFT + WISP_REACH;
/** Share of MaxDistance over which the fog fades out at the end of the rays. */
static const float DISTANCE_FADE = 0.2;
static const float TILE_TEXEL = 8.0;
static const int TILE_SLOTS = 8;
static const uint TILE_MAY_FORM = 1, TILE_ROCK = 2, TILE_STEEP = 4;
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

float FarGround(float2 xy)
{
	return HasFar != 0 ? lerp(FarZ.x, FarZ.y, FarHeight.SampleLevel(LinearSampler, xy * FarScale + FarOffset, 0)) : NO_GROUND;
}

/**
 * Ground under a position at height z: the level under it of the nearest texel, bilinear over the four nearest texels'
 * levels nearest that (those a level away left out: no slope hung across a ledge). forming: their share fog may form on;
 * carries: their share neither rock nor steep (an 8-unit map cannot follow a rock's face: fog drawn on it flickers in
 * and out of the mesh); ceiling: the overhang's underside over the floor under it (very high in the open). The far height
 * map where no tile is resident.
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
		carry += ((uint(g.y) & (TILE_ROCK | TILE_STEEP)) ? 0.0 : 1.0) * w;
		weights += w;
	}
	const bool resident = nearestTexel.x > NO_GROUND;
	forming = resident ? form / max(weights, 1e-4) : 1.0;
	carries = resident ? carry / max(weights, 1e-4) : 1.0;
	return !resident ? FarGround(position.xy) : weights > 1e-4 ? height / weights :
	                                                             reference;
}

/** The settled depth past the grid: the far ground's hollows hold more. */
float SettledFar(float2 xy, float ground)
{
	static const float RING = 256.0;
	const float around = 0.25 * (FarGround(xy + float2(RING, 0)) + FarGround(xy - float2(RING, 0)) + FarGround(xy + float2(0, RING)) + FarGround(xy - float2(0, RING)));
	return min(FarPoolDepth, FarDepth + max(around - ground, 0.0) * HollowGain);
}

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
	const float settled = HasFar != 0 ? SettledFar(xy, ground) * forming : 0.0;
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
	const float2 flow = velocity * min(1.0, MAX_FLOW / max(speed, 1e-3)) * FLOW_PERIOD;
	const float time = SharedData::Timer / FLOW_PERIOD;
	const float phaseA = frac(time), phaseB = frac(time + 0.5);
	const float3 span = float3(NOISE_SPAN, NOISE_SPAN, NOISE_SPAN_UP);
	const float3 p = (position - float3(0.0, 0.0, SharedData::Timer * RISE)) / span;
	const float3 a = p - float3(flow * phaseA, 0.0) / span;
	const float3 b = p - float3(flow * phaseB, 0.0) / span + 0.37;
	const float noiseA = Noise.SampleLevel(WrapSampler, a, 0) * 0.65 + Noise.SampleLevel(WrapSampler, a * 2.7 + 0.13, 0) * 0.35;
	const float noiseB = Noise.SampleLevel(WrapSampler, b, 0) * 0.65 + Noise.SampleLevel(WrapSampler, b * 2.7 + 0.13, 0) * 0.35;
	return lerp(noiseA, noiseB, abs(phaseA * 2.0 - 1.0));
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

/** Fog extinction per world unit at a position, and the sky light reaching it through the fog above. */
float FogAt(float3 position, out float shading)
{
	shading = 1.0;
	float forming, carries, ceiling;
	const float ground = Ground(position, forming, carries, ceiling);
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
	const float clumps = lerp(1.0, saturate(noise * 2.2 - 0.6), CLUMPING * up);
	const float body = (1.0 - smoothstep(top * (1.0 - TOP_SOFTNESS), top, height)) * clumps;
	const float wisps = WISP_DENSITY * (puff + saturate(column.w / LIFT_SPEED)) * saturate(noise * 4.0 - 2.6) * (1.0 - smoothstep(top, top * (1.0 + WISP_REACH), height));
	shading = exp(-SELF_SHADING * Density * max(top - height, 0.0));
	return Density * carries * max(body, wisps) * Uncut(position);
}

/**
 * Marches the ray between two distances with StepCount / 2 steps growing from FIRST_STEP at the start (the far end when
 * a_fromEnd). Returns in-scattered light (rgb) and transmittance (a).
 */
float4 MarchHalf(float3 camera, float3 direction, float from, float to, bool a_fromEnd, float jitter, float3 light)
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
		float shading;
		const float extinction = FogAt(position, shading) * (1.0 - smoothstep(MaxDistance * (1.0 - DISTANCE_FADE), MaxDistance, distance));
		if (extinction <= 0.0)
			continue;
		const float stepTransmittance = exp(-extinction * segment);
		const float3 scattered = (1.0 - stepTransmittance) * light * shading;
		// Marching back to front, each step sits in front of everything marched so far
		inscattering = a_fromEnd ? inscattering * stepTransmittance + scattered : inscattering + transmittance * scattered;
		transmittance *= stepTransmittance;
	}
	return float4(inscattering, transmittance);
}

/** The fog over one view ray to a surface (camera-relative), or to MaxDistance where depth is the sky. */
float4 FogOverRay(float depth, float2 uv, float2 pixel)
{
	float4 positionCS = mul(FrameBuffer::CameraViewProjInverse, float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1));
	const float3 surface = positionCS.xyz / positionCS.w;
	const float surfaceDistance = length(surface);
	const float3 direction = surface / max(surfaceDistance, 1e-3);
	const float rayLength = min(depth >= 1.0 ? MaxDistance : surfaceDistance, MaxDistance);

	// Clip the ray to the fog band over the ground under the camera and the ray's end
	const float3 camera = FrameBuffer::CameraPosAdjust.xyz;
	const float3 endPoint = camera + direction * rayLength;
	float formingA, formingB, carriesA, carriesB, ceilingA, ceilingB;
	const float bandTop = max(Ground(camera, formingA, carriesA, ceilingA), Ground(endPoint + float3(0.0, 0.0, 1.0), formingB, carriesB, ceilingB)) + MaxDepth * FLUFF_REACH;
	if (rayLength <= 0.0 || min(camera.z, endPoint.z) > bandTop)
		return float4(0.0, 0.0, 0.0, 1.0);
	float rayStart = 0.0;
	float rayEnd = rayLength;
	if (camera.z > bandTop)
		rayStart = (camera.z - bandTop) / max(-direction.z, 1e-4);
	if (endPoint.z > bandTop)
		rayEnd = (bandTop - camera.z) / max(direction.z, 1e-4);
	rayStart = clamp(rayStart, 0.0, rayLength);
	rayEnd = clamp(rayEnd, 0.0, rayLength);
	if (rayEnd <= rayStart)
		return float4(0.0, 0.0, 0.0, 1.0);
	const float span = rayEnd - rayStart;

	const float jitter = Random::InterleavedGradientNoise(pixel, SharedData::FrameCount);

	// Light: the sun scattered toward the eye, and the ambient average
	const float3 sun = SharedData::DirLightColor.xyz * HenyeyGreenstein(dot(direction, SharedData::DirLightDirection.xyz), PHASE_G) * 4.0 * Math::PI;
	const float3 ambient = max(float3(SharedData::AmbientSHR.x, SharedData::AmbientSHG.x, SharedData::AmbientSHB.x), 0.0) * 0.886;
	const float3 light = Albedo * (sun * SUN_SHARE * (1.0 - SharedData::InInterior) + ambient);

	// Both ends can be in the fog (camera and hit ground), so each half marches from its own end with fine first steps
	const float middle = rayStart + 0.5 * span;
	const float4 nearHalf = MarchHalf(camera, direction, rayStart, middle, false, jitter, light);
	const float4 farHalf = MarchHalf(camera, direction, middle, rayEnd, true, jitter, light);
	return float4(nearHalf.rgb + nearHalf.a * farHalf.rgb, nearHalf.a * farHalf.a);
}

/** Distance from the camera to a pixel's surface (MaxDistance for the sky), as the composite reads it. */
float SurfaceDistance(float depth, float2 uv)
{
	float4 positionCS = mul(FrameBuffer::CameraViewProjInverse, float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1));
	return depth >= 1.0 ? MaxDistance : min(length(positionCS.xyz / positionCS.w), MaxDistance);
}

#ifndef COMPOSITE
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
	FogHalf[dispatchID.xy] = FogOverRay(depth, uv, float2(dispatchID.xy));
	FogHalfDistance[dispatchID.xy] = SurfaceDistance(depth, uv);
}
#else
/** Relative distance difference over which a half-resolution texel stops counting for a pixel (an edge between surfaces). */
static const float EDGE_TOLERANCE = 0.05;

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const uint2 size = uint2(SharedData::BufferDim.xy);
	if (any(dispatchID.xy >= size))
		return;

	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;
	const float distance = SurfaceDistance(DepthTexture[dispatchID.xy], uv);

	// Bilinear over the four nearest half-resolution texels, each weighed down by how far its surface lies from this pixel's
	const int2 halfMax = int2((size + 1) / 2) - 1;
	const float2 position = (float2(dispatchID.xy) + 0.5) * 0.5 - 0.5;
	const int2 base = int2(floor(position));
	const float2 t = position - float2(base);
	float4 fog = 0.0;
	float weights = 0.0;
	float nearestGap = 1e30;
	float4 nearest = float4(0.0, 0.0, 0.0, 1.0);
	[unroll] for (uint k = 0; k < 4; k++)
	{
		const int2 offset = int2(k & 1, k >> 1);
		const int2 texel = clamp(base + offset, 0, halfMax);
		const float4 sample = FogHalfIn[texel];
		const float gap = abs(FogHalfDistanceIn[texel] - distance) / max(distance, 1.0);
		const float w = (offset.x ? t.x : 1.0 - t.x) * (offset.y ? t.y : 1.0 - t.y) * saturate(1.0 - gap / EDGE_TOLERANCE);
		fog += sample * w;
		weights += w;
		if (gap < nearestGap) {
			nearestGap = gap;
			nearest = sample;
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
