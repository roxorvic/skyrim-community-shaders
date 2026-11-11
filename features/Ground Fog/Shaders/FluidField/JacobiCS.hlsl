// Fluid Field wind, pass 4: one Jacobi iteration of the pressure Poisson equation.
// Pressure outside the grid is 0, so wind leaves through the edge freely; a wall (D193) mirrors the cell's own pressure, so
// no air crosses it.

#include "FluidField/Common.hlsli"
#include "FluidField/Levels.hlsli"

Texture2D<float> PressureIn : register(t0);
Texture2D<float> Divergence : register(t1);
RWTexture2D<float> PressureOut : register(u0);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);

	const float centre = PressureIn[id];
	float neighbours = 0.0;
	[unroll] for (uint k = 0; k < 4; k++)
	{
		const int2 n = logical + (k < 2 ? int2(k == 0 ? -1 : 1, 0) : int2(0, k == 2 ? -1 : 1));
		neighbours += Levels::Solid(n) ? centre : LoadOrZero(PressureIn, n);
	}

	PressureOut[id] = (neighbours - CellSize * CellSize * Divergence[id]) * 0.25;
}
