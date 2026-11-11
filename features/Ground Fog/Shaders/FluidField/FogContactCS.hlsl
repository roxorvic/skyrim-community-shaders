// Ground fog v3 (D194): the contact map, every frame: each close-range capsule (a leg, a body) marks the texels under it
// with the heights it spans, so the draw cuts fog exactly there and only there (fog under a ledge is cut only when touched).
// The map is cleared before (ContactLow to the largest, ContactHigh to 0).

#include "FluidField/FogCommon.hlsli"

StructuredBuffer<Mover> Contacts : register(t0);
RWTexture2D<uint> ContactLow : register(u0);   // Lowest point over the texel, encoded (EncodeHeight)
RWTexture2D<uint> ContactHigh : register(u1);  // Highest

/** Heights as orderable integers, sixteenths of a unit. */
uint EncodeHeight(float z)
{
	return uint(clamp((z + 131072.0) * 16.0, 0.0, 4294967040.0));
}

[numthreads(8, 8, 1)] void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
	if (group.x >= ContactCount)
		return;
	const Mover m = Contacts[group.x];
	const float2 lo = min(m.From, m.To) - m.Radius, hi = max(m.From, m.To) + m.Radius;
	const int2 texelLo = max(int2(floor(lo / ContactCell)) - ContactMin, 0);
	const int2 texelHi = min(int2(floor(hi / ContactCell)) - ContactMin, int(ContactResolution) - 1);
	const float2 segment = m.To - m.From;
	const float segmentLength2 = max(dot(segment, segment), 1e-6);
	const uint low = EncodeHeight(m.Bottom), high = EncodeHeight(m.Top);
	for (int y = texelLo.y + int(thread.y); y <= texelHi.y; y += 8) {
		for (int x = texelLo.x + int(thread.x); x <= texelHi.x; x += 8) {
			const float2 p = (float2(ContactMin + int2(x, y)) + 0.5) * ContactCell;
			const float2 d = p - (m.From + segment * saturate(dot(p - m.From, segment) / segmentLength2));
			if (dot(d, d) > m.Radius * m.Radius)
				continue;
			InterlockedMin(ContactLow[uint2(x, y)], low);
			InterlockedMax(ContactHigh[uint2(x, y)], high);
		}
	}
}
