// Ground fog v3 (D194), pass 2, per level: each outflow speeds up by the fall of the fog's surface to the level it flows
// into (Target, D195). Nothing flows into a rock from ground no higher than its top (across a rock, only level or down),
// across the grid's edge or into a closed level. Outflows are scaled so a level never sends more than it holds: depth
// stays positive and fog is conserved.

#define FOG_BEDS t0
#include "FluidField/FogCommon.hlsli"

Texture2DArray<float> DepthIn : register(t1);
Texture2DArray<float4> FluxIn : register(t2);
RWTexture2DArray<float4> Flux : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= Resolution))
		return;
	const uint level = id.z;
	const int2 cell = CellOf(id.xy);
	const float4 bed = Bed[id];
	const float depth = DepthIn[id];
	if (bed.z == KIND_CLOSED || depth <= 0.0) {
		Flux[id] = 0.0;
		return;
	}
	const float surface = bed.x + depth;
	const float4 old = FluxIn[id] * exp(-FluxDamping * DeltaTime);
	float outflow[4] = { old.x, old.y, old.z, old.w };
	[unroll] for (int d = 0; d < 4; d++)
	{
		const int target = Target(cell, level, DIRECTIONS[d]);
		bool open = target >= 0;
		float fall = 0.0;
		if (open) {
			const uint3 there = uint3(TexelOf(cell + DIRECTIONS[d]), target);
			const float4 otherBed = Bed[there];
			open = !(otherBed.z == KIND_ROCK && (bed.z == KIND_ROCK ? bed.x < otherBed.x : bed.x <= otherBed.x));
			fall = surface - otherBed.x - DepthIn[there];
		}
		outflow[d] = open ? max(outflow[d] + DeltaTime * Gravity * fall / CellSize, 0.0) : 0.0;
	}
	float4 flux = float4(outflow[0], outflow[1], outflow[2], outflow[3]);
	const float total = (flux.x + flux.y + flux.z + flux.w) * DeltaTime;
	Flux[id] = total > depth ? flux * (depth / total) : flux;
}
