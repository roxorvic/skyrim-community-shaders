// Fluid Field fog (D193): a level takes its child's wind and fog where the child covers it, after both have stepped, so
// what the finer level did (a wake, a parted bank) carries on here once it leaves the child. The child's cells over each of
// this level's are averaged; its own edge band is left out (it follows this level there).

#include "FluidField/Common.hlsli"
#include "FluidField/Levels.hlsli"

Texture2D<float2> VelocityIn : register(t0);
Texture2D<float> FogIn : register(t1);
Texture2D<float2> ChildVelocity : register(t2);
Texture2D<float> ChildFog : register(t3);
RWTexture2D<float2> VelocityOut : register(u0);
RWTexture2D<float> FogOut : register(u1);

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	const float2 world = Levels::WorldOf(StorageToLogical(id));
	float2 velocity = VelocityIn[id];
	float fog = FogIn[id];
	const float weight = HasOther != 0 ? Levels::OtherInterior(world) : 0.0;
	if (weight > 0.0) {
		const float quarter = 0.25 * CellSize;
		float2 childVelocity = 0.0;
		float childFog = 0.0;
		[unroll] for (int k = 0; k < 4; k++)
		{
			const float2 p = world + quarter * float2(k & 1 ? 1.0 : -1.0, k & 2 ? 1.0 : -1.0);
			childVelocity += Levels::SampleOther(ChildVelocity, p);
			childFog += Levels::SampleOther(ChildFog, p);
		}
		velocity = lerp(velocity, childVelocity * 0.25, weight);
		fog = lerp(fog, childFog * 0.25, weight);
	}
	VelocityOut[id] = velocity;
	FogOut[id] = fog;
}
