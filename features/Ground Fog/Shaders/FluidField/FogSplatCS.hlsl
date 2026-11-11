// Ground fog v3 (D194): draws this step's movers into the force texture, one group per mover over its own footprint, so
// the cost follows the area moved through rather than the grid. Each mover pushes the fog along its motion and shoves it
// out from its line (mostly ahead and sideways), and may cut it. D195: into the level it stands in, the one whose floor
// is nearest under its feet; a mover above a level's fog leaves it alone.

#define FOG_BEDS t1
#include "FluidField/FogCommon.hlsli"

StructuredBuffer<Mover> Movers : register(t0);
RWTexture2DArray<int> Force : register(u0);  // Per level, three slices: push x, push y (world units per second), cut

/** Radii past which a mover's falloff is left out. */
static const float REACH = 2.5;
/** Most cells a mover's footprint spans per side (a blast wider than this is clipped). */
static const int MAX_FOOTPRINT = 64;
/** Smallest radius drawn, in cells: finer would alias; widened, its peak drops so its total stays the same. */
static const float LEAST_RADIUS = 0.75;
/** A mover this far over a level's floor is above its fog. */
static const float ABOVE_FOG = 80.0;

[numthreads(8, 8, 1)] void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
	if (group.x >= MoverCount)
		return;
	const Mover m = Movers[group.x];
	const float r = max(m.Radius, LEAST_RADIUS * CellSize);
	const float spread = (m.Radius * m.Radius) / (r * r);

	const float2 lo = min(m.From, m.To) - REACH * r;
	const float2 hi = max(m.From, m.To) + REACH * r;
	const int2 cellLo = max(int2(floor(lo / CellSize)), GridMin);
	const int2 cellHi = min(min(int2(floor(hi / CellSize)), GridMin + int(Resolution) - 1), cellLo + MAX_FOOTPRINT - 1);

	const float speed = length(m.Velocity);
	const float2 heading = speed > 0.0 ? m.Velocity / speed : 0.0;
	const float2 segment = m.To - m.From;
	const float segmentLength2 = max(dot(segment, segment), 1e-6);

	for (int y = cellLo.y + int(thread.y); y <= cellHi.y; y += 8) {
		for (int x = cellLo.x + int(thread.x); x <= cellHi.x; x += 8) {
			const int2 cell = int2(x, y);
			const float2 p = (float2(cell) + 0.5) * CellSize;
			const float2 d = p - (m.From + segment * saturate(dot(p - m.From, segment) / segmentLength2));
			const float distance2 = dot(d, d);
			const float g = exp(-distance2 / (r * r));
			if (g < 1e-3)
				continue;
			const uint2 s = TexelOf(cell);
			// The level it stands in: the lower one when its feet are under the top's floor
			const float4 top = Bed[uint3(s, 0)], under = Bed[uint3(s, 1)];
			const uint level = under.z != KIND_CLOSED && m.Bottom < top.x - 0.5 * LEVEL_STEP ? 1 : 0;
			const float floorHeight = level == 1 ? under.x : top.x;
			if (m.Bottom > floorHeight + ABOVE_FOG || m.Top < floorHeight)
				continue;

			// d/r * exp(-(d/r)^2) peaks at 1/sqrt(2) of the radius; scaled so the peak is the shove. Little behind the mover
			const float distance = sqrt(distance2);
			const float2 outward = d / max(distance, 1e-3);
			const float bow = 0.25 + 0.75 * saturate(dot(outward, heading) * 0.5 + 0.6);
			const float2 push = (m.Velocity * g + outward * m.Shove * bow * 2.33 * (distance / r) * g) * spread;
			InterlockedAdd(Force[uint3(s, level * 3 + 0)], int(round(push.x * FORCE_SCALE)));
			InterlockedAdd(Force[uint3(s, level * 3 + 1)], int(round(push.y * FORCE_SCALE)));
			if (m.Cut > 0.0)
				InterlockedAdd(Force[uint3(s, level * 3 + 2)], int(round(m.Cut * g * FORCE_SCALE)));
		}
	}
}
