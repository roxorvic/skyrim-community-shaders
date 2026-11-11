// Fluid Field wind, pass 5: subtract the pressure gradient so the velocity is divergence-free.

#include "FluidField/Common.hlsli"
#include "FluidField/Levels.hlsli"

Texture2D<float2> VelocityIn : register(t0);
Texture2D<float> Pressure : register(t1);
RWTexture2D<float2> VelocityOut : register(u0);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);

	// A wall's side mirrors this cell's pressure (D193): no push through it
	const float centre = Pressure[id];
	float left = Levels::Solid(logical + int2(-1, 0)) ? centre : LoadOrZero(Pressure, logical + int2(-1, 0));
	float right = Levels::Solid(logical + int2(1, 0)) ? centre : LoadOrZero(Pressure, logical + int2(1, 0));
	float down = Levels::Solid(logical + int2(0, -1)) ? centre : LoadOrZero(Pressure, logical + int2(0, -1));
	float up = Levels::Solid(logical + int2(0, 1)) ? centre : LoadOrZero(Pressure, logical + int2(0, 1));

	VelocityOut[id] = Levels::Solid(logical) ? 0.0 : VelocityIn[id] - float2(right - left, up - down) / (2.0 * CellSize);
}
