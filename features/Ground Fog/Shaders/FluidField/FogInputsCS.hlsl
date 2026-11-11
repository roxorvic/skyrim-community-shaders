// Ground fog v3 (D194), pass 1, per level: cells new to the grid (a scroll, a reset) start from the settled field; fog
// forms toward Ground Thickness where it may, evaporates over Pool Depth and over water; movers push it (as flux) and cut
// it; vorticity confinement keeps swirls alive.

#define FOG_BEDS t0
#include "FluidField/FogCommon.hlsli"

Texture2DArray<float2> Velocity : register(t1);  // Last step's
Texture2DArray<float4> FluxIn : register(t2);
RWTexture2DArray<float> Depth : register(u0);
RWTexture2DArray<float4> Flux : register(u1);  // Ping-pong: typed loads of RGBA16F are optional on D3D11
RWTexture2DArray<int> Force : register(u2);    // FogSplatCS's; cleared here once read

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
	if (bed.z == KIND_WATER)
		depth -= depth * saturate(WaterEvaporate * DeltaTime);
	else {
		if (bed.z == KIND_GROUND)
			depth += max(GroundThickness - depth, 0.0) * saturate(SourceRate * bed.y * DeltaTime);
		depth -= max(depth - PoolDepth, 0.0) * saturate(EvaporateRate * DeltaTime);
	}
	depth *= 1.0 - cut;
	Depth[id] = max(depth, 0.0);

	// Flux is depth per second across a face: a flow of v carries depth * v / CellSize
	float4 flux = FluxIn[id];
	float2 drive = push;
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
