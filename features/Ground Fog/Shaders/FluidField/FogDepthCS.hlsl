// Ground fog v3 (D194), pass 3, per level: depth takes in the flux its neighbours' levels send to it (Target, D195) and
// gives its own; velocity from the flux through its faces; the column the draw reads: depth, velocity and churn.

#define FOG_BEDS t1
#include "FluidField/FogCommon.hlsli"

Texture2DArray<float4> Flux : register(t0);
RWTexture2DArray<float> Depth : register(u0);
RWTexture2DArray<float2> Velocity : register(u1);
RWTexture2DArray<float4> Column : register(u2);  // x depth, yz velocity, w churn

/** Flux the neighbour's levels send into this level from direction d (their channel back toward this cell). */
float Inflow(int2 cell, uint level, int d)
{
	const int2 other = cell + DIRECTIONS[d];
	if (!InGrid(other))
		return 0.0;
	const int back = d ^ 1;  // East and west, north and south pair up as channels 0/1 and 2/3
	float sum = 0.0;
	[unroll] for (uint m = 0; m < 2; m++)
	{
		if (Target(other, m, DIRECTIONS[back]) != int(level))
			continue;
		const float4 flux = Flux[uint3(TexelOf(other), m)];
		sum += back == 0 ? flux.x : back == 1 ? flux.y :
		                        back == 2     ? flux.z :
		                                        flux.w;
	}
	return sum;
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= Resolution))
		return;
	const uint level = id.z;
	// A closed bed (most cells' lower level): nothing flows in or out, so nothing to sum
	if (Bed[id].z == KIND_CLOSED) {
		Depth[id] = 0.0;
		Velocity[id] = 0.0;
		Column[id] = 0.0;
		return;
	}
	const int2 cell = CellOf(id.xy);
	const float4 outflow = Flux[id];
	const float4 inflow = float4(Inflow(cell, level, 0), Inflow(cell, level, 1), Inflow(cell, level, 2), Inflow(cell, level, 3));
	const float depth = max(Depth[id] + DeltaTime * (dot(inflow, 1.0.xxxx) - dot(outflow, 1.0.xxxx)), 0.0);
	Depth[id] = depth;
	// Mean flow through the east-west and north-south faces, over the depth it carries
	const float2 through = 0.5 * float2((outflow.x - inflow.x) + (inflow.y - outflow.y), (outflow.z - inflow.z) + (inflow.w - outflow.w));
	const float2 velocity = through * CellSize / max(depth, 1.0);
	Velocity[id] = velocity;
	Column[id] = float4(depth, velocity, length(velocity));
}
