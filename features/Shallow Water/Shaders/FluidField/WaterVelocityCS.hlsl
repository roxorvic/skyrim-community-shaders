// Fluid Field water, pass 4: accelerate each face by the surface slope. Faces between dry cells, or climbing onto a dry
// cell's higher bed, are closed so banks and rocks reflect waves. Between cells that both have vanilla water, the slope
// is the water's deviation from vanilla's level: vanilla's flat planes step down along a river, and pinned to them
// (LevelRelax) the sim pumped water down every step at ~3 m/s. The river's current comes from the flowmap instead.

#include "FluidField/WaterCommon.hlsli"

Texture2D<float> Depth : register(t0);
Texture2D<float2> VelocityIn : register(t1);
Texture2D<float2> CellMap : register(t2);
RWTexture2D<float2> VelocityOut : register(u0);

float2 LoadCellMap(int2 logical)
{
	return CellMap[LogicalToStorage(clamp(logical, 0, int(GridRes) - 1))];
}

/** @brief New velocity on the face between cell a (behind) and cell b (ahead). */
float FaceVelocity(float velocity, float depthA, float2 mapA, float depthB, float2 mapB)
{
	float bedA = mapA.x, bedB = mapB.x;
	bool wetA = depthA > WetThreshold;
	bool wetB = depthB > WetThreshold;
	float etaA = bedA + depthA;
	float etaB = bedB + depthB;

	if (!wetA && !wetB)
		return 0.0;
	if (!wetA && etaB < bedA)
		return 0.0;
	if (!wetB && etaA < bedB)
		return 0.0;

	float rise = etaB - etaA;
	if (mapA.y > WetThreshold && mapB.y > WetThreshold)
		rise -= (bedB + mapB.y) - (bedA + mapA.y);
	velocity -= Gravity * DeltaTime * rise / CellSize;
	return clamp(velocity, -MaxSpeed, MaxSpeed);
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	float2 velocity = VelocityIn[id];

	float depth = Depth[id];
	float2 map = CellMap[id];
	float dW = LoadDepth(Depth, logical + int2(-1, 0));
	float dS = LoadDepth(Depth, logical + int2(0, -1));
	float2 mapW = LoadCellMap(logical + int2(-1, 0));
	float2 mapS = LoadCellMap(logical + int2(0, -1));

	VelocityOut[id] = float2(
		FaceVelocity(velocity.x, dW, mapW, depth, map),
		FaceVelocity(velocity.y, dS, mapS, depth, map));
}
