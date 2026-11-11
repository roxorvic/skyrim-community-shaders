// Fluid Field wind, pass 1: carry velocity over and add the movers' impulses (SplatCS). Texels that scrolled in hold
// another cell's data, so they start from the parent level's wind (D193), or from rest without one, as does every texel
// after a reset; the edge band relaxes toward the parent so the levels meet without a seam.

#include "FluidField/Common.hlsli"
#include "FluidField/Levels.hlsli"

Texture2D<float2> VelocityIn : register(t0);

/** Vorticity confinement strength: restores swirls advection smooths away, so stirred air (and its fog) keeps curling. */
static const float VORTICITY = 0.6;

/** Curl of the velocity at a logical cell, 1/s (central differences). */
float Curl(int2 logical)
{
	float2 east = LoadClamped(VelocityIn, logical + int2(1, 0));
	float2 west = LoadClamped(VelocityIn, logical - int2(1, 0));
	float2 north = LoadClamped(VelocityIn, logical + int2(0, 1));
	float2 south = LoadClamped(VelocityIn, logical - int2(0, 1));
	return ((east.y - west.y) - (north.x - south.x)) / (2.0 * CellSize);
}
Texture2D<int> SplatWindX : register(t1);
Texture2D<int> SplatWindY : register(t2);
Texture2D<float2> ParentVelocity : register(t3);
RWTexture2D<float2> VelocityOut : register(u0);

/** Share of the way an edge cell moves toward its parent's wind each step. */
static const float EDGE_RELAX = 0.5;

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	int2 cell = GridMin + logical;
	bool keptFromPrevStep = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));

	const float2 parent = HasOther != 0 ? Levels::SampleOther(ParentVelocity, Levels::WorldOf(logical)) : 0.0;
	float2 velocity = keptFromPrevStep ? VelocityIn[id] : parent;

	// Vorticity confinement: push each swirl's flow around its centre
	if (keptFromPrevStep) {
		float curl = Curl(logical);
		float2 gradient = float2(abs(Curl(logical + int2(1, 0))) - abs(Curl(logical - int2(1, 0))),
			abs(Curl(logical + int2(0, 1))) - abs(Curl(logical - int2(0, 1))));
		float magnitude = length(gradient);
		if (magnitude > 1e-6) {
			float2 n = gradient / magnitude;
			velocity += VORTICITY * CellSize * curl * float2(n.y, -n.x) * DeltaTime;
		}
	}

	velocity += float2(SplatWindX[id], SplatWindY[id]) / Levels::SPLAT_SCALE;
	if (HasOther != 0)
		velocity = lerp(velocity, parent, Levels::EdgeWeight(logical) * EDGE_RELAX);

	VelocityOut[id] = Levels::Solid(logical) ? 0.0 : velocity;
}
