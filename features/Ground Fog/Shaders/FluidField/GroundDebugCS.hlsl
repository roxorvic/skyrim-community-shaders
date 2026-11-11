// Fluid Field ground fog debug view (D194): every surface coloured by what the fog's ground takes it for. Green: fog may
// form; red: rock or log; yellow: steep; cyan: water; grey: a surface under the ground's height (a wall's side); magenta: over it (a surface the map missed); blue: no tile
// yet (the far height map there, darker past it). Provisional tiles (their area not settled yet) are striped.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

cbuffer GroundDebugCB : register(b0)
{
	int4 SlotTiles[64];  // Per resident slot: xy the tile uploaded there, z 1 once valid
	int2 TileMin;        // The window's south-west tile
	float2 FarScale;     // Far height map: uv = xy * FarScale + FarOffset, height = lerp(FarZ.x, FarZ.y, texel)
	float2 FarOffset;
	float2 FarZ;
	uint HasFar;
	float3 Pad;
};

Texture2D<float> DepthTexture : register(t0);
Texture2D<float2> GroundTiles : register(t1);  // x height, y flags (GroundTiles::Flag)
Texture2D<float> FarHeight : register(t2);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> MainRW : register(u0);

static const float TEXEL = 8.0;
static const int TILE_TEXELS = 128;
static const int SLOTS = 8;
static const uint FOG_MAY_FORM = 1, ROCK = 2, STEEP = 4, WATER = 8, PROVISIONAL = 16;
/** A surface this close to the ground's height is the ground. */
static const float ON_GROUND = 12.0;

float3 GroundColour(float3 world, uint2 pixel)
{
	const int2 texel = int2(floor(world.xy / TEXEL));
	const int2 tile = texel >> 7;
	const int4 slot = SlotTiles[(tile.y & (SLOTS - 1)) * SLOTS + (tile.x & (SLOTS - 1))];
	const float2 g = GroundTiles.Load(int3(texel & (SLOTS * TILE_TEXELS - 1), 0));
	const uint flags = uint(g.y);
	// Off the ground: grey under the map's height, magenta over it (a surface the map missed)
	float3 colour = world.z > g.x + ON_GROUND ? float3(0.85, 0.1, 0.85) : float3(0.45, 0.45, 0.45);
	if (any(tile < TileMin) || any(tile >= TileMin + SLOTS) || slot.z == 0 || any(slot.xy != tile)) {
		const float far = lerp(FarZ.x, FarZ.y, FarHeight.SampleLevel(LinearSampler, world.xy * FarScale + FarOffset, 0));
		colour = HasFar != 0 && abs(world.z - far) < 64.0 ? float3(0.15, 0.3, 1.0) : float3(0.05, 0.05, 0.35);
	} else if (g.x < -1e6)
		colour = float3(0.15, 0.3, 1.0);
	else if (flags & ROCK)
		colour = world.z > g.x - 400.0 ? float3(0.9, 0.15, 0.1) : colour;
	else if (abs(world.z - g.x) < ON_GROUND)
		colour = (flags & WATER) ? float3(0.1, 0.8, 0.9) : (flags & STEEP) ? float3(0.95, 0.85, 0.1) :
		                                                                     float3(0.2, 0.8, 0.2);
	if ((flags & PROVISIONAL) && ((pixel.x + pixel.y) / 6) % 2 == 0)
		colour *= 0.4;
	return colour;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	const uint2 size = uint2(SharedData::BufferDim.xy);
	const float depth = DepthTexture[dispatchID.xy];
	if (any(dispatchID.xy >= size) || depth >= 1.0)
		return;
	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;
	const float4 positionCS = mul(FrameBuffer::CameraViewProjInverse, float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1));
	const float3 world = FrameBuffer::CameraPosAdjust.xyz + positionCS.xyz / positionCS.w;
	const float4 scene = MainRW[dispatchID.xy];
	MainRW[dispatchID.xy] = float4(lerp(scene.rgb, GroundColour(world, dispatchID.xy) * max(dot(scene.rgb, 0.333), 0.15) * 2.0, 0.6), scene.a);
}
