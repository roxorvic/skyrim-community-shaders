// Ground fog v3 (D194), item 5: the ambient drift (the curl of two scales of slowly changing noise) once per DRIFT_TEXEL
// cells, toroidal over the grid (DriftTexelOf), times Drift; FogInputsCS reads it bilinearly instead of evaluating it
// per cell per level (about 64 hash lookups each).

#include "FluidField/FogCommon.hlsli"

RWTexture2D<float2> DriftOut : register(u0);

/** Smooth value noise over a lattice, 0 to 1 (the ambient drift's stream function). */
float StreamNoise(float3 p)
{
	const float3 i = floor(p), f = p - i;
	const float3 u = f * f * (3.0 - 2.0 * f);
	float corners[8];
	[unroll] for (uint k = 0; k < 8; k++)
	{
		const float3 c = i + float3(k & 1, (k >> 1) & 1, k >> 2);
		uint h = asuint(int(c.x)) * 374761393u + asuint(int(c.y)) * 668265263u + asuint(int(c.z)) * 2147483647u;
		h = (h ^ (h >> 13)) * 1274126177u;
		corners[k] = float((h ^ (h >> 16)) & 0xFFFF) / 65535.0;
	}
	const float x0 = lerp(lerp(corners[0], corners[1], u.x), lerp(corners[2], corners[3], u.x), u.y);
	const float x1 = lerp(lerp(corners[4], corners[5], u.x), lerp(corners[6], corners[7], u.x), u.y);
	return lerp(x0, x1, u.z);
}

/** The curl of slowly changing noise at a world position, over eddies SPAN units across. */
float2 Eddies(float2 xy, float span, float period)
{
	static const float STEP = 0.05;
	const float3 p = float3(xy / span, Time / period + span);
	const float dx = StreamNoise(p + float3(STEP, 0.0, 0.0)) - StreamNoise(p - float3(STEP, 0.0, 0.0));
	const float dy = StreamNoise(p + float3(0.0, STEP, 0.0)) - StreamNoise(p - float3(0.0, STEP, 0.0));
	return float2(dy, -dx) / (2.0 * STEP);
}

// DRIFT_TEXELS² texels, 8 x 8 a group, groups in one row (FogSolver::Drift)
[numthreads(8, 8, 1)] void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
	static const uint GROUPS = DRIFT_TEXELS / 8;
	const uint2 texel = uint2(group.x % GROUPS, group.x / GROUPS) * 8 + thread.xy;
	const int2 driftMin = DriftMin();
	const int2 world = driftMin + ((int2(texel) - driftMin) & (DRIFT_TEXELS - 1));
	const float2 centre = (float2(world) + 0.5) * DRIFT_TEXEL * CellSize;
	// Large slow eddies and smaller quicker ones, so it swirls without piling fog up anywhere
	DriftOut[texel] = (Eddies(centre, 600.0, 40.0) + 0.6 * Eddies(centre, 200.0, 15.0)) * Drift;
}
