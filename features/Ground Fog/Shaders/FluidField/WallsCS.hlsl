// Fluid Field wind, pass 0 (D193): this step's walls per cell (a rock, log or trunk standing there), from the fog's ground
// map, so the passes after (the pressure solve's many iterations above all) read one texel, not the ground map.

#define WALL_MASK_WRITER
#include "FluidField/Common.hlsli"
#include "FluidField/Levels.hlsli"

RWTexture2D<uint> WallMaskOut : register(u0);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;
	WallMaskOut[id] = Levels::SolidFromGround(StorageToLogical(id)) ? 1u : 0u;
}
