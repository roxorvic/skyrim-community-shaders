// Fluid Field wind, pass 0 (D193): draws this step's movers into the splat textures, one group per mover over its own
// footprint, so the cost follows the area moved through rather than the grid. Each mover is a segment (a point when its
// ends meet) with a Gaussian falloff: it adds its wind impulse, shoves the fog outward from its line (mostly ahead and
// sideways) and carves it where it passes. Movers a running child covers are left to the child (it is absorbed back).

#include "FluidField/Common.hlsli"
#include "FluidField/Levels.hlsli"

struct Mover
{
	float2 From;      // Segment start, world xy
	float2 To;        // Segment end, world xy
	float Radius;     // Gaussian falloff radius, world units
	float Shove;      // World units per second it shoves the fog aside at its edge
	float2 Velocity;  // Wind impulse, world units per second, added once
	float Carve;      // Share of the fog it clears outright where it passes
	float Z;          // Its height: it parts the fog layer nearest it (D193)
	float2 Pad;
};

StructuredBuffer<Mover> Movers : register(t0);
RWTexture2D<int> WindX : register(u0);
RWTexture2D<int> WindY : register(u1);
RWTexture2D<int> ShoveX : register(u2);
RWTexture2D<int> ShoveY : register(u3);
RWTexture2D<uint> Carve : register(u4);
RWTexture2D<int> ShoveX1 : register(u5);  // The same for the layer under overhangs
RWTexture2D<int> ShoveY1 : register(u6);
RWTexture2D<uint> Carve1 : register(u7);

/** Radii past which a mover's falloff is left out. */
static const float REACH = 2.5;
/** Most cells a mover's footprint spans per side (a blast wider than this is clipped). */
static const int MAX_FOOTPRINT = 64;
/** Smallest radius drawn, in cells: finer would alias; widened, its peak drops so its total stays the same. */
static const float LEAST_RADIUS = 0.75;

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
	if (group.x >= MoverCount)
		return;
	const Mover m = Movers[group.x];
	const float r = max(m.Radius, LEAST_RADIUS * CellSize);
	const float spread = (m.Radius * m.Radius) / (r * r);

	const float2 lo = min(m.From, m.To) - REACH * r;
	const float2 hi = max(m.From, m.To) + REACH * r;
	const int2 cellLo = max(int2(floor(lo / CellSize)) - GridMin, 0);
	const int2 cellHi = min(min(int2(floor(hi / CellSize)) - GridMin, int(GridRes) - 1), cellLo + MAX_FOOTPRINT - 1);

	const float speed = length(m.Velocity);
	const float2 heading = speed > 0.0 ? m.Velocity / speed : 0.0;
	const float2 segment = m.To - m.From;
	const float segmentLength2 = max(dot(segment, segment), 1e-6);

	for (int y = cellLo.y + int(thread.y); y <= cellHi.y; y += FLUIDFIELD_GROUP_SIZE) {
		for (int x = cellLo.x + int(thread.x); x <= cellHi.x; x += FLUIDFIELD_GROUP_SIZE) {
			const int2 logical = int2(x, y);
			const float2 p = Levels::WorldOf(logical);
			if (Levels::ChildCovers(p))
				continue;
			const float2 d = p - (m.From + segment * saturate(dot(p - m.From, segment) / segmentLength2));
			const float distance2 = dot(d, d);
			const float g = exp(-distance2 / (r * r));
			if (g < 1e-3)
				continue;
			const uint2 s = LogicalToStorage(logical);

			const float2 wind = m.Velocity * g * spread * Levels::SPLAT_SCALE;
			InterlockedAdd(WindX[s], int(round(wind.x)));
			InterlockedAdd(WindY[s], int(round(wind.y)));

			// Under an overhang, the layer nearest the mover's height
			const float4 ground = Levels::GroundAt(p);
			const bool lowerLayer = ground.z > Levels::NO_GROUND && abs(m.Z - ground.z) < abs(m.Z - ground.x);
			if (m.Shove > 0.0) {
				// d/r * exp(-(d/r)^2) peaks at 1/sqrt(2) of the radius; scaled so the peak is the shove. Little behind the mover
				const float distance = sqrt(distance2);
				const float2 outward = d / max(distance, 1e-3);
				const float bow = 0.25 + 0.75 * saturate(dot(outward, heading) * 0.5 + 0.6);
				const float2 shove = outward * m.Shove * bow * 2.33 * (distance / r) * g * spread * Levels::SPLAT_SCALE;
				if (lowerLayer) {
					InterlockedAdd(ShoveX1[s], int(round(shove.x)));
					InterlockedAdd(ShoveY1[s], int(round(shove.y)));
				} else {
					InterlockedAdd(ShoveX[s], int(round(shove.x)));
					InterlockedAdd(ShoveY[s], int(round(shove.y)));
				}
			}
			if (m.Carve > 0.0) {
				if (lowerLayer)
					InterlockedMax(Carve1[s], uint(saturate(m.Carve * g) * Levels::CARVE_SCALE));
				else
					InterlockedMax(Carve[s], uint(saturate(m.Carve * g) * Levels::CARVE_SCALE));
			}
		}
	}
}
