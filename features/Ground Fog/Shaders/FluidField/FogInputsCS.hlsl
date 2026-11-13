// Ground fog v3 (D194), pass 1, per level: cells new to the grid (a scroll, a reset) start from the settled field; fog
// forms toward Ground Thickness where it may (less on slopes), evaporates over Pool Depth;
// movers and the ambient drift push it (as flux; the wind pushes in FogFluxCS) and movers cut it; rain strikes the top level, denting it here and
// there (the flow fills the dents back in); vorticity confinement keeps swirls alive.

#define FOG_BEDS t0
#include "FluidField/FogCommon.hlsli"

Texture2DArray<float2> Velocity : register(t1);  // Last step's
Texture2DArray<float4> FluxIn : register(t2);
Texture2D<float> PrecipOcclusion : register(t3);  // Depth from above; HasOcclusion 0: none
Texture2D<float2> DriftField : register(t4);      // FogDriftCS's, already times Drift
SamplerState LinearSampler : register(s0);
RWTexture2DArray<float> Depth : register(u0);
RWTexture2DArray<float4> Flux : register(u1);  // Ping-pong: typed loads of RGBA16F are optional on D3D11
RWTexture2DArray<int> Force : register(u2);    // FogSplatCS's; cleared here once read

/** A slope's share of the fog forming on flats. */
static const float SLOPE_SOURCE = 0.35;

/** Settled depth: Ground Thickness where fog may form, deeper in hollows (ground lower than round it), up to Pool Depth. */
float Settled(int2 cell, uint level, float4 bed)
{
	if (bed.z != KIND_GROUND || bed.y <= 0.0)
		return 0.0;
	if (level == 1)
		return GroundThickness * bed.y;
	// The ground round it, on two rings, inside the grid
	static const float RINGS[2] = { 64.0, 192.0 };
	float sum = 0.0, count = 0.0;
	[unroll] for (int r = 0; r < 2; r++)
	{
		const int radius = max(int(RINGS[r] / CellSize + 0.5), 1);
		[unroll] for (int k = 0; k < 8; k++)
		{
			const float angle = k * 0.785398;
			const int2 other = clamp(cell + int2(round(float2(cos(angle), sin(angle)) * radius)), GridMin, GridMin + int(Resolution) - 1);
			const float4 around = Bed[uint3(TexelOf(other), 0)];
			if (around.z != KIND_CLOSED) {
				sum += around.x;
				count += 1.0;
			}
		}
	}
	const float hollow = count > 0.0 ? max(sum / count - bed.x, 0.0) : 0.0;
	return min(PoolDepth, GroundThickness * bed.y + hollow * HollowGain);
}

/** Whether rain reaches a point: the precipitation occlusion map sees it from above (as Skylighting reads it). */
bool Open(int2 cell, float z)
{
	if (HasOcclusion == 0)
		return true;
	float3 position = mul(OcclusionViewProj, float4(float3((float2(cell) + 0.5) * CellSize, z) - Eye, 1.0)).xyz;
	position.y = -position.y;
	const float2 uv = position.xy * 0.5 + 0.5;
	return any(uv <= 0.0) || any(uv >= 1.0) || position.z <= PrecipOcclusion.SampleLevel(LinearSampler, uv, 0) + 0.001;
}

/** The ambient drift at a cell (FogDriftCS's texture, bilinear): slow eddies the fog wanders in. */
float2 AmbientDrift(int2 cell)
{
	const float2 c = (float2(cell) + 0.5) / DRIFT_TEXEL - 0.5;
	const int2 base = int2(floor(c));
	const float2 f = c - float2(base);
	const uint mask = DRIFT_TEXELS - 1;
	const float2 a = lerp(DriftField[uint2(base) & mask], DriftField[uint2(base + int2(1, 0)) & mask], f.x);
	const float2 b = lerp(DriftField[uint2(base + int2(0, 1)) & mask], DriftField[uint2(base + 1) & mask], f.x);
	return lerp(a, b, f.y);
}

/** 1 on flat ground, SLOPE_SOURCE on a slope steeper than about 20 degrees: fog forms most on flats; slopes keep a thin layer
 * that creeps down. */
float Flatness(int2 cell, uint level, float floorHeight)
{
	const int2 last = GridMin + int(Resolution) - 1;
	const float e = Bed[uint3(TexelOf(min(cell + int2(1, 0), last)), level)].x, w = Bed[uint3(TexelOf(max(cell - int2(1, 0), GridMin)), level)].x;
	const float n = Bed[uint3(TexelOf(min(cell + int2(0, 1), last)), level)].x, s = Bed[uint3(TexelOf(max(cell - int2(0, 1), GridMin)), level)].x;
	// A neighbour a level away (a ledge, a closed level) is no slope
	const float2 rise = float2(abs(e - w) < 2.0 * LEVEL_STEP ? e - w : 0.0, abs(n - s) < 2.0 * LEVEL_STEP ? n - s : 0.0) / (2.0 * CellSize);
	return lerp(1.0, SLOPE_SOURCE, smoothstep(0.15, 0.35, length(rise)));
}

float Curl(int2 cell, uint level)
{
	const int2 last = GridMin + int(Resolution) - 1;
	const float2 e = Velocity[uint3(TexelOf(min(cell + int2(1, 0), last)), level)];
	const float2 w = Velocity[uint3(TexelOf(max(cell - int2(1, 0), GridMin)), level)];
	const float2 n = Velocity[uint3(TexelOf(min(cell + int2(0, 1), last)), level)];
	const float2 s = Velocity[uint3(TexelOf(max(cell - int2(0, 1), GridMin)), level)];
	return ((e.y - w.y) - (n.x - s.x)) / (2.0 * CellSize);
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= Resolution))
		return;
	const uint level = id.z;
	const int2 cell = CellOf(id.xy);
	const float4 bed = Bed[id];
	const uint3 force = uint3(id.xy, level * 3);
	const float2 push = float2(Force[force], Force[force + uint3(0, 0, 1)]) / FORCE_SCALE;
	const float cut = saturate(Force[force + uint3(0, 0, 2)] / FORCE_SCALE);
	Force[force] = 0;
	Force[force + uint3(0, 0, 1)] = 0;
	Force[force + uint3(0, 0, 2)] = 0;

	const bool fresh = any(cell < PrevGridMin) || any(cell >= PrevGridMin + int(Resolution));
	if (fresh || bed.z == KIND_CLOSED) {
		Depth[id] = fresh ? Settled(cell, level, bed) : 0.0;
		Flux[id] = 0.0;
		return;
	}
	float depth = Depth[id];
	// Over water fog forms none and is not drawn (for now), but it is no sink: shores do not drain into lakes and vanish
	{
		if (bed.z == KIND_GROUND)
			depth += max(GroundThickness - depth, 0.0) * saturate(SourceRate * bed.y * Flatness(cell, level, bed.x) * DeltaTime);
		depth -= max(depth - PoolDepth, 0.0) * saturate(EvaporateRate * DeltaTime);
	}
	depth *= 1.0 - cut;
	// Rain: each tick, a share of the top level's cells is struck (as CS's puddle ripples, from intensity and time)
	if (Rain > 0.0 && level == 0 && Open(cell, bed.x + depth)) {
		static const float TICKS = 20.0, STRUCK = 0.006, DENT = 0.35;
		uint h = asuint(cell.x) * 374761393u + asuint(cell.y) * 668265263u + uint(Time * TICKS) * 2246822519u;
		h = (h ^ (h >> 13)) * 1274126177u;
		if (float((h ^ (h >> 16)) & 0xFFFF) / 65535.0 < Rain * STRUCK)
			depth *= 1.0 - DENT;
	}
	Depth[id] = max(depth, 0.0);

	// Flux is depth per second across a face: a flow of v carries depth * v / CellSize
	float4 flux = FluxIn[id];
	float2 drive = push + (Drift > 0.0 ? AmbientDrift(cell) : 0.0);
	if (Confinement > 0.0 && depth > 0.01) {
		// Vorticity confinement: a push along the swirl, toward stronger curl
		const float curl = Curl(cell, level);
		const float2 gradient = float2(abs(Curl(cell + int2(1, 0), level)) - abs(Curl(cell - int2(1, 0), level)), abs(Curl(cell + int2(0, 1), level)) - abs(Curl(cell - int2(0, 1), level)));
		const float length2 = dot(gradient, gradient);
		if (length2 > 1e-12)
			drive += Confinement * CellSize * curl * float2(gradient.y, -gradient.x) * rsqrt(length2) * DeltaTime;
	}
	// A push sets the flow at least to its own speed, so a mover held still does not pump it ever faster
	const float4 driven = float4(max(drive.x, 0.0), max(-drive.x, 0.0), max(drive.y, 0.0), max(-drive.y, 0.0)) * depth / CellSize;
	Flux[id] = max(flux, driven);
}
