// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Original code Copyright (c) 2022 Ilya Perapechka (MIT), modifications GPL-3.0 with Modding Exception (see EXCEPTIONS.md)

#if defined(HORIZON_FIX)
namespace HorizonFix
{
	// Depth (z/w) that water folded back from beyond the far clip plane lands at: eight
	// depth quanta inside the far plane, exactly representable in both D24 and D32F
	// buffers - behind everything the scene rendered, in front of the depth clear.
	static const float FoldedDepth = 1.0 - 8.0 / 16777216.0;

	// Scene depth at or beyond this counts as "nothing rendered behind the water": the
	// clear value, folded far water (FoldedDepth), or an external plugin's depth-clamped
	// far-water backdrop a few quanta inside that. The margin is a few world units at the
	// far plane, where no real surface can render.
	static const float EmptyDepthThreshold = 1.0 - 64.0 / 16777216.0;
}
#endif

#if defined(UNDERWATERMASK)

#	if defined(VSHADER)
struct VS_INPUT
{
	float4 Position: POSITION0;
};
#	endif

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
};

#	ifdef VSHADER

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float z = min(1, 1e-4 * max(0, input.Position.z - 70000)) * 0.5 + input.Position.z;
	vsout.Position = float4(input.Position.xy, z, 1);

	return vsout;
}
#	endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#	ifdef PSHADER
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	psout.Color = 1;

	return psout;
}
#	endif

#else

#	include "Common/FrameBuffer.hlsli"
#	include "Common/MotionBlur.hlsli"
#	include "Common/Permutation.hlsli"
#	include "Common/Random.hlsli"
#	include "Common/Shading.hlsli"
#	include "Common/ColorManagement.hlsli"

#	define WATER

#	if defined(SPECULAR) || defined(UNDERWATER)
// WADING is undefined in pixel builds (read from the descriptor): `defined(WADING)` there is always false.
#		define WADING_RT
#		if defined(PSHADER)
#			undef WADING
#		endif
#	endif

#	include "Common/SharedData.hlsli"

// D159: Fluid Field's see-through falls drawn by the river's shader (FallRefraction): the refracting technique only. The
// per-vertex and flowmap variants, Water Effects' parallax and Wetness Effects' ripples assume a level surface and are off.
#	if defined(FLUID_FIELD_FALLS) && !(defined(SPECULAR) && NUM_SPECULAR_LIGHTS == 0)
#		undef FLUID_FIELD_FALLS
#	endif
#	if defined(FLUID_FIELD_FALLS)
#		if !defined(FLUID_FIELD)
#			define FLUID_FIELD
#		endif
#		undef FLOWMAP
#		undef BLEND_NORMALS
#		undef VC
#		undef NORMAL_TEXCOORD
#		undef WADING
#		undef WADING_RT
#		undef VERTEX_ALPHA_DEPTH
#		undef WATER_EFFECTS
#		undef WETNESS_EFFECTS
// Falls keep their own shading: no PBR Water (its tessellation, outputs and shading) in this technique
#		undef PBR_WATER
#	endif

#	if defined(VSHADER) || defined(HSHADER) || defined(DSHADER)
struct VS_INPUT
{
#		if defined(SPECULAR) || defined(UNDERWATER) || defined(STENCIL) || defined(SIMPLE)
	float4 Position: POSITION0;
#			if defined(NORMAL_TEXCOORD)
	float2 TexCoord0: TEXCOORD0;
#			endif
#			if defined(VC)
	float4 Color: COLOR0;
#			endif
#		endif

#		if defined(LOD)
	float4 Position: POSITION0;
#			if defined(VC)
	float4 Color: COLOR0;
#			endif
#		endif
};
#	endif

struct VS_OUTPUT
{
#	if defined(SPECULAR) || defined(UNDERWATER)
	float4 HPosition: SV_POSITION0;
#		if !defined(UNIFIED_WATER)
	float4 FogParam: COLOR0;
#		endif
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
	float4 TexCoord2: TEXCOORD2;
#		if defined(WADING_RT) || defined(WADING) || (defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS))) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)) || ((defined(SPECULAR) && NUM_SPECULAR_LIGHTS == 0) && defined(FLOWMAP) /*!defined(NORMAL_TEXCOORD) && !defined(BLEND_NORMALS) && !defined(VC)*/)
	float4 TexCoord3: TEXCOORD3;
#		endif
#		if defined(FLOWMAP)
	nointerpolation float2 TexCoord4: TEXCOORD4;
#		endif
#		if NUM_SPECULAR_LIGHTS == 0
	float4 MPosition: TEXCOORD5;
#		endif
#	endif

#	if defined(SIMPLE)
	float4 HPosition: SV_POSITION0;
	float4 FogParam: COLOR0;
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
	float4 TexCoord2: TEXCOORD2;
	float4 MPosition: TEXCOORD5;
#	endif

#	if defined(LOD)
	float4 HPosition: SV_POSITION0;
	float4 FogParam: COLOR0;
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
#	endif

#	if defined(STENCIL)
	float4 HPosition: SV_POSITION0;
	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
#	endif

	float4 NormalsScale: TEXCOORD8;

#	if defined(FLUID_FIELD_FALLS)
	float3 FallNormal: TEXCOORD9;        // D159: the sheet's geometric normal, world axes
	float2 FallTexCoord: TEXCOORD10;     // ...its authored texture coordinates (FallGround::Flow's input)
	float3 FallFade: TEXCOORD11;         // ...FallGround::Fade: x seam fade, y over the water below, z its foot
	float4 FallData: TEXCOORD12;         // ...x water or air behind, y under its source water, z to the nearest other sheet
	float4 FallOutward: TEXCOORD13;      // ...normal turned outward; w 1 where another sheet lies behind its inner side
	float3 FallTangent: TEXCOORD14;      // D174: the way texture v runs, world axes
	float4 FallWobble: TEXCOORD15;       // D188: FallGround::WobbleAt (z in world units)
	float2 FallWobbleSizes: TEXCOORD16;  // FallGround::WobbleSizes, world units: the lighting follows all the wobble
	float3 FallPrevious: TEXCOORD17;     // its place a frame ago (camera-relative, the previous camera's): the motion vectors
	float FallClip: SV_ClipDistance0;
#	endif
#	if defined(PBR_WATER) && !defined(STENCIL)
	float4 WaveParam: TEXCOORD9;      // xyz undisplaced camera-relative position, w wave damping (flow)
	float2 SurfaceNoise: TEXCOORD10;  // x sediment patches (TurbidityPatches), y wind gust factor (WindGust)
	float3 Barycentric: TEXCOORD11;
	float2 FlowVelocity: TEXCOORD13;  // the river current (units/s, PBRWater::FlowVelocity), 0 on still water
#	endif
#	if defined(PBR_WATER)
	float4 ShoreState: TEXCOORD12;  // x swash tip, y swash mark (units above the still water), z waves without the swash (units above the still water), w still water level (camera-relative z; DisplaceSurface)
#	endif
};

#	if defined(PBR_WATER)
#		if defined(FLUID_FIELD) && !defined(LOD)
#			include "FluidField/WaterSurface.hlsli"
#		endif
#		include "PBRWater/PBRWater.hlsli"
// Draws that may be tessellated. LOD water is never tessellated (it is far away and flat).
#		if defined(SPECULAR) || defined(UNDERWATER) || defined(SIMPLE) || defined(STENCIL)
#			define PBRW_TESSELLATION
#		endif
#		if defined(STENCIL)
#			define PBRW_RAW_COLOR WorldPosition
#		else
#			define PBRW_RAW_COLOR WPosition
#		endif
#		define PBR_WATER_SPACING PBRWater::Draw0.x
// Tessellated and stencil/colour passes must rasterise identical depth: forbid reassociation.
#		define PBRW_PRECISE precise
#	else
#		define PBR_WATER_SPACING 0.0
#		define PBRW_PRECISE
#	endif

#	if defined(VSHADER) || defined(HSHADER) || defined(DSHADER)

cbuffer PerTechnique : register(b0)
{
	float4 QPosAdjust : packoffset(c0);
};

cbuffer PerMaterial : register(b1)
{
	float4 VSFogParam : packoffset(c0);
	float4 VSFogNearColor : packoffset(c1);
	float4 VSFogFarColor : packoffset(c2);
	float4 NormalsScroll0 : packoffset(c3);
	float4 NormalsScroll1 : packoffset(c4);
	float4 NormalsScale : packoffset(c5);
};

cbuffer PerGeometry : register(b2)
{
	row_major float4x4 World : packoffset(c0);
	row_major float4x4 PreviousWorld : packoffset(c4);
	row_major float4x4 WorldViewProj : packoffset(c8);
	float3 ObjectUV : packoffset(c12);
	float4 CellTexCoordOffset : packoffset(c13);
};

#		if defined(FLUID_FIELD_FALLS)
#			include "FluidField/FallGround.hlsli"

// D159: FallRefraction's per-piece constants (FallSurface.hlsl's FallSurfaceCB): the model to world transform
cbuffer FallSurfaceCB : register(b7)
{
	row_major float3x4 FallWorld : packoffset(c0);
};

struct FallSurfaceVertex
{
	float3 Position;  // Model space
	float2 TexCoord;  // Vanilla's
	float3 Normal;    // Model space
	float3 Tangent;   // D174: the direction v runs, model space (FallRefraction::Vertex)
};

StructuredBuffer<FallSurfaceVertex> FallVertices : register(t125);

/**
 * D159: a falls sheet's vertex, moved as FallSurface.hlsl moves it (seams, swell, wobble at the frame's latched phase, cut),
 * output as the river's vertex shader outputs its own.
 */
VS_OUTPUT main(uint vertexID : SV_VertexID)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;
	vsout.NormalsScale = NormalsScale;

	const FallSurfaceVertex fallVertex = FallVertices[vertexID];
	const float3 fallShift = FallGround::Shift(vertexID, FallGround::Scroll.x, FallWorld[2].xyz);
	const float3 model = fallVertex.Position + fallShift;
	const float3 relative = mul(FallWorld, float4(model, 1.0)) - FrameBuffer::CameraPosAdjust.xyz;
	const float4 worldViewPos = mul(FrameBuffer::CameraViewProj, float4(relative, 1.0));
	vsout.HPosition = worldViewPos;

#			if !defined(UNIFIED_WATER)
	float fogDistanceFactor = min(VSFogFarColor.w, pow(saturate(length(worldViewPos.xyz) * VSFogParam.y - VSFogParam.x), NormalsScale.w));
	vsout.FogParam.xyz = lerp(ColorManagement::SRGBToWorking(VSFogNearColor.xyz), ColorManagement::SRGBToWorking(VSFogFarColor.xyz), fogDistanceFactor);
	vsout.FogParam.w = fogDistanceFactor;
#			endif

	vsout.WPosition = float4(relative, length(relative));
	vsout.MPosition = float4(relative, 1.0);
	vsout.TexCoord1 = NormalsScroll0;
	vsout.TexCoord2 = float4(NormalsScroll1.xy, worldViewPos.w, 0.0);

	vsout.FallNormal = mul((float3x3)FallWorld, fallVertex.Normal);
	vsout.FallTangent = mul((float3x3)FallWorld, fallVertex.Tangent);
	vsout.FallWobble = FallGround::WobbleAt(vertexID, FallGround::Scroll.x);
	vsout.FallWobble.z *= length(FallWorld[0].xyz);  // its size in world units
	vsout.FallWobbleSizes = FallGround::WobbleSizes(vertexID, FallGround::Scroll.x, FallWorld[2].xyz, fallVertex.Normal) * length(FallWorld[0].xyz);
	vsout.FallPrevious = mul(FallWorld, float4(fallVertex.Position + FallGround::Shift(vertexID, FallGround::PreviousPhase, FallWorld[2].xyz), 1.0)) -
	                     FrameBuffer::CameraPreviousPosAdjust.xyz;
	vsout.FallTexCoord = fallVertex.TexCoord;
	vsout.FallFade.xy = FallGround::Fade(vertexID);
	vsout.FallFade.y = min(vsout.FallFade.y, 512.0);  // D139: the "none" value held before interpolation
	vsout.FallFade.z = (!FallGround::Enabled || FallGround::VertexBase == FallGround::NO_VERTEX_CLIP) ? 1.0 : fmod(FallGround::VertexClip[FallGround::VertexBase + vertexID].a.w, 2.0);
	// The depth behind moves with the vertex: pushed out along its normal, the gap behind widens
	const float fallShiftOut = dot(fallShift, fallVertex.Normal) / max(dot(fallVertex.Normal, fallVertex.Normal), 1e-6) * length(FallWorld[0].xyz);
	vsout.FallData = float4(max(FallGround::Behind(vertexID) + fallShiftOut, 0.0), min(FallGround::BelowSource(vertexID), 512.0), min(FallGround::ToOtherSheet(vertexID), 512.0), fallShiftOut);  // w: its push out
	vsout.FallOutward = float4(vsout.FallNormal * FallGround::OutwardSign(vertexID), FallGround::InnerCovered(vertexID));
	vsout.FallClip = FallGround::ClipDistance(relative, vertexID);
	return vsout;
}
#		else
#			if defined(PBR_WATER)
SamplerState FlowMapSamplerVS : register(s8);
Texture2D<float4> FlowMapTexVS : register(t8);

/// Inverse of the linear (rotation/scale) part of a world matrix: maps world offsets to model space.
float3x3 InverseLinear(float4x4 m)
{
	float3x3 a = (float3x3)m;
	float3 c0 = cross(a[1], a[2]);
	float3 c1 = cross(a[2], a[0]);
	float3 c2 = cross(a[0], a[1]);
	float det = dot(a[0], c0);
	return transpose(float3x3(c0, c1, c2)) / det;
}

/**
 * River current flattens the open-water waves. The worldspace flowmap is looked up by world position,
 * not by mesh UVs: the stencil pass (depth and motion vectors) is compiled without FLOWMAP and must
 * displace the surface exactly like the colour pass, and neighbouring meshes must agree on shared edges.
 * The same sample gives the current the waves are carried along (PBRWater::FlowPhases).
 */
float GetWaveFlowDamping(float3 positionWS, out float2 velocity)
{
	velocity = 0.0;
	float damping = 1.0;
	float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
	if (PBRWater::Flow0.x != 0.0) {
		float4 flow = FlowMapTexVS.SampleLevel(FlowMapSamplerVS, absXY * PBRWater::Flow0.xz + PBRWater::Flow0.yw, 0);
		velocity = PBRWater::FlowVelocity(flow);
		damping = 1.0 - PBRWater::Foam1.w * saturate(flow.z * flow.w);
	}
#				if defined(FLUID_FIELD) && !defined(LOD)
	// In the sim's area the waves ride the sim's flow (around rocks, through eddies), undamped: they are the surface's
	// moving detail there, and damping them by the flow hid them exactly where the water runs
	const float fluidShare = PBRWater::FluidShare(positionWS);
	if (fluidShare > 0.0) {
		velocity = lerp(velocity, FluidField::SampleFlowOutput(absXY).xy, fluidShare);
		damping = lerp(damping, 1.0, fluidShare);
	}
#				endif
	return damping;
}
#			endif

/**
 * The complete water vertex program. Runs in the vertex shader for regular draws and in the
 * domain shader for every tessellated vertex, so both paths produce bit-identical attributes.
 * @param spacing local vertex spacing in world units (for wave Nyquist filtering)
 */
VS_OUTPUT WaterVertex(VS_INPUT input, float spacing)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;

	vsout.NormalsScale = NormalsScale;

	PBRW_PRECISE float4 inputPosition = float4(input.Position.xyz, 1.0);
	PBRW_PRECISE float4 previousInputPosition = inputPosition;
	PBRW_PRECISE float4 scrollWorldPos = mul(World, inputPosition);

#			if defined(PBR_WATER)
	{
		float2 flowVelocity;
		float flowDamping = GetWaveFlowDamping(scrollWorldPos.xyz, flowVelocity);
#				if defined(STENCIL)
		PBRWater::SurfaceDisplacement surface = PBRWater::DisplaceSurface(scrollWorldPos.xyz, flowDamping, flowVelocity, spacing, true);
#				else
		PBRWater::SurfaceDisplacement surface = PBRWater::DisplaceSurface(scrollWorldPos.xyz, flowDamping, flowVelocity, spacing, false);
#				endif
		float3x3 worldToModel = InverseLinear(World);
		previousInputPosition.xyz += mul(worldToModel, surface.previous);
		inputPosition.xyz += mul(worldToModel, surface.current);
		vsout.ShoreState = surface.shore;
#				if !defined(STENCIL)
		vsout.WaveParam = float4(scrollWorldPos.xyz, flowDamping);
		vsout.FlowVelocity = flowVelocity;
		float2 absXY = scrollWorldPos.xy + FrameBuffer::CameraPosAdjust.xy;
		vsout.SurfaceNoise = PBRWater::SurfaceNoise(absXY);
		vsout.Barycentric = 1.0 / 3.0;
#				endif
	}
#			endif

	PBRW_PRECISE float4 worldPos = mul(World, inputPosition);
	PBRW_PRECISE float4 worldViewPos = mul(WorldViewProj, inputPosition);

	float heightMult = min((1.0 / 10000.0) * max(worldViewPos.z - 70000, 0), 1);

	vsout.HPosition.xy = worldViewPos.xy;
	vsout.HPosition.z = heightMult * 0.5 + worldViewPos.z;
	vsout.HPosition.w = worldViewPos.w;

#			if defined(HORIZON_FIX)
	vsout.HPosition.z = min(vsout.HPosition.z, vsout.HPosition.w * HorizonFix::FoldedDepth);
#			endif

#			if defined(STENCIL)
	vsout.WorldPosition = worldPos;
	vsout.PreviousWorldPosition = mul(PreviousWorld, previousInputPosition);
#			else

#				if !defined(UNIFIED_WATER)
	float fogDistanceFactor = min(VSFogFarColor.w, pow(saturate(length(worldViewPos.xyz) * VSFogParam.y - VSFogParam.x), NormalsScale.w));
	vsout.FogParam.xyz = lerp(ColorManagement::SRGBToWorking(VSFogNearColor.xyz), ColorManagement::SRGBToWorking(VSFogFarColor.xyz), fogDistanceFactor);
	vsout.FogParam.w = fogDistanceFactor;
#				endif

	vsout.WPosition.xyz = worldPos.xyz;
	vsout.WPosition.w = length(worldPos.xyz);

#				if defined(LOD)
	float4 posAdjust =
		ObjectUV.x ? 0.0 : (QPosAdjust.xyxy + scrollWorldPos.xyxy) / NormalsScale.xxyy;

	vsout.TexCoord1.xyzw = NormalsScroll0 + posAdjust;
#				else
#					if !defined(SPECULAR) || (NUM_SPECULAR_LIGHTS == 0)
	vsout.MPosition.xyzw = inputPosition.xyzw;
#					endif

	float2 posAdjust = scrollWorldPos.xy + QPosAdjust.xy;

	float2 scrollAdjust1 = posAdjust / NormalsScale.xx;
	float2 scrollAdjust2 = posAdjust / NormalsScale.yy;
	float2 scrollAdjust3 = posAdjust / NormalsScale.zz;

#					if defined(UNIFIED_WATER) && defined(NORMAL_TEXCOORD)
	float2 cellShift = float2(floor(ObjectUV.z * 0.5), floor((ObjectUV.z - 1.0) * 0.5));
	float2 scaledUV = input.TexCoord0.xy * ObjectUV.z - cellShift;
#					endif

#					if !(defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS) || defined(DEPTH) || NUM_SPECULAR_LIGHTS == 0))
#						if defined(NORMAL_TEXCOORD)
	float3 normalsScale = 0.001 * NormalsScale.xyz;
	if (ObjectUV.x) {
		scrollAdjust1 = input.TexCoord0.xy / normalsScale.xx;
		scrollAdjust2 = input.TexCoord0.xy / normalsScale.yy;
		scrollAdjust3 = input.TexCoord0.xy / normalsScale.zz;
	}
#						else
	if (ObjectUV.x) {
		scrollAdjust1 = 0.0;
		scrollAdjust2 = 0.0;
		scrollAdjust3 = 0.0;
	}
#						endif
#					endif

	vsout.TexCoord1 = 0.0;
	vsout.TexCoord2 = 0.0;
#					if defined(WADING_RT)
	vsout.TexCoord3 = 0.0;
#					endif
#					if defined(FLOWMAP)
#						if !(((defined(SPECULAR) || NUM_SPECULAR_LIGHTS == 0) || (defined(UNDERWATER) && defined(REFRACTIONS))) && !defined(NORMAL_TEXCOORD))
#							if defined(BLEND_NORMALS)
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = NormalsScroll0.zw + scrollAdjust2;
	vsout.TexCoord2.xy = NormalsScroll1.xy + scrollAdjust3;
#							else
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = 0.0;
	vsout.TexCoord2.xy = 0.0;
#							endif
#						endif
#						if !defined(NORMAL_TEXCOORD)
	vsout.TexCoord3 = 0.0;
#						elif defined(WADING)
#							if defined(UNIFIED_WATER)
	float2 wadingUV = (input.TexCoord0.xy - 0.5f) * 0.5f;
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + wadingUV) / ObjectUV.xy;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + wadingUV;
#							else
	vsout.TexCoord2.zw = ((-0.5 + input.TexCoord0.xy) * 0.1 + CellTexCoordOffset.xy) +
	                     float2(CellTexCoordOffset.z, -CellTexCoordOffset.w + ObjectUV.x) / ObjectUV.xx;
	vsout.TexCoord3.xy = -0.25 + (input.TexCoord0.xy * 0.5 + ObjectUV.yz);
#							endif
	vsout.TexCoord3.zw = input.TexCoord0.xy;
#						elif (defined(REFRACTIONS) || NUM_SPECULAR_LIGHTS == 0 || defined(BLEND_NORMALS))
#							if defined(UNIFIED_WATER)
	float2 dims = float2(ObjectUV.x, ObjectUV.y);
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + scaledUV) / dims;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + scaledUV;
	vsout.TexCoord3.zw = scaledUV;
#							else
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + input.TexCoord0.xy) / ObjectUV.xx;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + input.TexCoord0.xy;
	vsout.TexCoord3.zw = input.TexCoord0.xy;
#							endif
#						endif
	vsout.TexCoord4 = ObjectUV.xy;
#					else
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = NormalsScroll0.zw + scrollAdjust2;
	vsout.TexCoord2.xy = NormalsScroll1.xy + scrollAdjust3;
	vsout.TexCoord2.z = worldViewPos.w;
	vsout.TexCoord2.w = 0;
#						if (defined(WADING) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)))
	vsout.TexCoord3 = 0.0;
#							if (defined(NORMAL_TEXCOORD) && ((!defined(BLEND_NORMALS) && !defined(VERTEX_ALPHA_DEPTH)) || defined(WADING)))
	vsout.TexCoord3.xy = input.TexCoord0;
#							endif
#							if defined(VERTEX_ALPHA_DEPTH) && defined(VC)
	vsout.TexCoord3.z = input.Color.w;
#							endif
#						endif
#					endif
#				endif
#			endif

	return vsout;
}
#		endif  // FLUID_FIELD_FALLS

#	endif  // VSHADER || HSHADER || DSHADER

#	if defined(PBR_WATER) && defined(PBRW_TESSELLATION) && (defined(VSHADER) || defined(HSHADER) || defined(DSHADER))
// ============================================================================
// Tessellation (VS -> HS -> tessellator -> DS -> [GS] -> PS)
//
// While a draw is tessellated the vertex shader only forwards the raw vertex. The domain shader
// re-runs WaterVertex() on each generated vertex, so every permutation (including STENCIL, which
// writes the water mask and motion vectors) gets exactly the attributes a regular draw would.
// The hull/domain/geometry shaders are compiled per descriptor alongside the vertex shader, which
// keeps the stage signatures matched for every permutation.
// ============================================================================

/// Forwards the vertex inputs through VS_OUTPUT fields that exist in every tessellated permutation.
VS_OUTPUT PackControlPoint(VS_INPUT input)
{
	VS_OUTPUT o = (VS_OUTPUT)0;
	o.HPosition = float4(input.Position.xyz, 1.0);
#		if defined(NORMAL_TEXCOORD)
	o.NormalsScale.xy = input.TexCoord0.xy;
#		endif
#		if defined(VC)
	o.PBRW_RAW_COLOR = input.Color;
#		endif
	return o;
}

VS_INPUT UnpackControlPoint(VS_OUTPUT cp)
{
	VS_INPUT v = (VS_INPUT)0;
	v.Position = float4(cp.HPosition.xyz, 1.0);
#		if defined(NORMAL_TEXCOORD)
	v.TexCoord0 = cp.NormalsScale.xy;
#		endif
#		if defined(VC)
	v.Color = cp.PBRW_RAW_COLOR;
#		endif
	return v;
}

struct HS_CONSTANT_OUTPUT
{
	float Edge[3] : SV_TessFactor;
	float Inside: SV_InsideTessFactor;
};
#	endif

#	if defined(VSHADER) && !defined(FLUID_FIELD_FALLS)
VS_OUTPUT main(VS_INPUT input)
{
#		if defined(PBR_WATER) && defined(PBRW_TESSELLATION)
	if (PBRWater::Tess0.x > 0.5)
		return PackControlPoint(input);
#		endif
	return WaterVertex(input, PBR_WATER_SPACING);
}
#	endif

#	if defined(PBR_WATER) && defined(PBRW_TESSELLATION) && (defined(HSHADER) || defined(DSHADER))
#		include "PBRWater/Tessellation.hlsli"
#	endif

#	if defined(DSHADER) && defined(PBRW_TESSELLATION)
[domain("tri")] VS_OUTPUT main(HS_CONSTANT_OUTPUT patchConstants, float3 barycentric : SV_DomainLocation, const OutputPatch<VS_OUTPUT, 3> patch) {
	VS_INPUT corners[3] = { UnpackControlPoint(patch[0]), UnpackControlPoint(patch[1]), UnpackControlPoint(patch[2]) };
	VS_INPUT v = PBRWater::InterpolateVertex(corners, barycentric);
	return WaterVertex(v, PBRWater::TessellatedSpacing(mul(World, float4(v.Position.xyz, 1.0)).xyz));
}
#	endif

#	if defined(GSHADER)
	// Only bound for the wireframe debug view: tags each triangle corner with its barycentric.
	[maxvertexcount(3)] void main(triangle VS_OUTPUT input[3], inout TriangleStream<VS_OUTPUT> stream)
{
	[unroll] for (uint i = 0; i < 3; i++)
	{
		VS_OUTPUT v = input[i];
#		if defined(PBR_WATER) && !defined(STENCIL)
		v.Barycentric = float3(i == 0, i == 1, i == 2);
#		endif
		stream.Append(v);
	}
	stream.RestartStrip();
}
#	endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
#	if defined(UNDERWATER) || defined(SIMPLE) || defined(LOD) || defined(SPECULAR)
	float4 Lighting: SV_Target0;
#	endif

#	if defined(STENCIL)
	float4 WaterMask: SV_Target0;
	float2 MotionVector: SV_Target1;
#	elif defined(FLUID_FIELD_FALLS)
	float2 MotionVector: SV_Target1;  // the sheet's own (its wobble), where the falls pass binds them (FallRefraction)
#	endif
};

#	ifdef PSHADER

SamplerState ReflectionSampler : register(s0);
SamplerState RefractionSampler : register(s1);
SamplerState DisplacementSampler : register(s2);
SamplerState CubeMapSampler : register(s3);
SamplerState Normals01Sampler : register(s4);
SamplerState Normals02Sampler : register(s5);
SamplerState Normals03Sampler : register(s6);
SamplerState DepthSampler : register(s7);
SamplerState FlowMapSampler : register(s8);
SamplerState FlowMapNormalsSampler : register(s9);
SamplerState SSRReflectionSampler : register(s10);
SamplerState RawSSRReflectionSampler : register(s11);

Texture2D<float4> ReflectionTex : register(t0);
Texture2D<float4> RefractionTex : register(t1);
Texture2D<float4> DisplacementTex : register(t2);
TextureCube<float4> CubeMapTex : register(t3);
Texture2D<float4> Normals01Tex : register(t4);
Texture2D<float4> Normals02Tex : register(t5);
Texture2D<float4> Normals03Tex : register(t6);
Texture2D<float4> DepthTex : register(t7);
Texture2D<float4> FlowMapTex : register(t8);
Texture2D<float4> FlowMapNormalsTex : register(t9);
Texture2D<float4> SSRReflectionTex : register(t10);
Texture2D<float4> RawSSRReflectionTex : register(t11);

cbuffer PerTechnique : register(b0)
{
	float4 VPOSOffset : packoffset(c0);  // inverse main render target width and height in xy, 0 in zw
	float4 PosAdjust : packoffset(c1);   // inverse framebuffer range in w
	float4 CameraDataWater : packoffset(c2);
	float4 SunDir : packoffset(c3);
	float4 SunColor : packoffset(c4);
}

cbuffer PerMaterial : register(b1)
{
	float4 ShallowColor : packoffset(c0);
	float4 DeepColor : packoffset(c1);
	float4 ReflectionColor : packoffset(c2);
	float4 FresnelRI : packoffset(c3);    // Fresnel amount in x, specular power in z
	float4 BlendRadius : packoffset(c4);  // flowmap scale in y, specular radius in z
	float4 VarAmounts : packoffset(c5);   // Sun specular power in x, reflection amount in y, alpha in z, refraction magnitude in w
	float4 NormalsAmplitude : packoffset(c6);
	float4 WaterParams : packoffset(c7);   // noise falloff in x, reflection magnitude in y, sun sparkle power in z, framebuffer range in w
	float4 FogNearColor : packoffset(c8);  // above water fog amount in w
	float4 FogFarColor : packoffset(c9);
	float4 FogParam : packoffset(c10);      // above water fog distance far in z, above water fog range in w
	float4 DepthControl : packoffset(c11);  // depth reflections factor in x, depth refractions factor in y, depth normals factor in z, depth specular lighting factor in w
	float4 SSRParams : packoffset(c12);     // fWaterSSRIntensity in x, fWaterSSRBlurAmount in y, inverse main render target width and height in zw
	float4 SSRParams2 : packoffset(c13);    // fWaterSSRNormalPerturbationScale in x
}

cbuffer PerGeometry : register(b2)
{
	float4x4 TextureProj : packoffset(c0);
	float4 ReflectPlane[1] : packoffset(c4);
	float4 ProjData : packoffset(c5);
	float4 LightPos[8] : packoffset(c6);
	float4 LightColor[8] : packoffset(c14);
}

#		if defined(FLUID_FIELD_FALLS)
#			include "FluidField/FallShading.hlsli"

// D159: FallRefraction's per-piece constants: whether the piece has a normal map and whether the front surface (D125) is bound
cbuffer FallSurfaceCB : register(b7)
{
	uint FallHasNormal : packoffset(c8.w);
	uint FallHasFront : packoffset(c9.x);
};

Texture2D<float4> FallNormalTex : register(t122);    // The piece's own normal map (vanilla's falls), on its flow
Texture2D<float4> FallFrontDepth : register(t105);   // D125: the scene's depth with the falls' front surface drawn in
Texture2D<float4> FallBehindDepth : register(t112);  // D174: the falls' surface nearest behind their front (cleared to far where none)
Texture2D<float4> FallFoamTex : register(t113);      // D174: the river's own Normals01, read as the foam's bubbles

/** D159: the sheet's geometric normal at this pixel, turned to the camera: the falls' "up" wherever the river's is (0, 0, 1). */
static float3 FallUp = float3(0, 0, 1);
/** D159: the plane through this pixel of the sheet, as ReflectPlane[0] (camera-relative): the depth behind is measured from it. */
static float4 FallPlane = float4(0, 0, 1, 0);
/** How far the water has begun to fall: 0 on the crest (the river plane's own shading), 1 once it drops. */
static float FallFalling = 1.0;
/** D189: how far this pixel's reflected ray spreads (fast and aerated water, FallShading::SSR_ROUGH_*). */
static float FallRoughness = 0.0;
#		endif

/** The surface's plane (camera-relative, n.p + w = 0), from which the water behind it is measured. */
float4 GetWaterPlane()
{
#		if defined(FLUID_FIELD_FALLS)
	return FallPlane;
#		else
	return ReflectPlane[0];
#		endif
}

#		define SampColorSampler Normals01Sampler
#		define LinearSampler Normals01Sampler

#		if defined(SKYLIGHTING)
#			include "Skylighting/Skylighting.hlsli"
#		endif

#		if defined(EXP_HEIGHT_FOG)
#			include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#		endif

#		include "Common/ShadowSampling.hlsli"

#		if defined(PBR_WATER)
#			include "PBRWater/Shading.hlsli"
#		endif

#		if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
float GetWaterFogFade()
{
#			if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		return ExponentialHeightFog::GetVanillaFogFade(PosAdjust.w);
	}
#			endif
	return PosAdjust.w;
}

#			if defined(LOD)
#				undef FLUID_FIELD
#			endif

#			if defined(FLUID_FIELD)
#				include "FluidField/WaterNormal.hlsli"
#			endif

#			if defined(FLOWMAP)

/**
 * Structure containing complete flowmap information
 */
struct FlowmapData
{
	float4 color;       // Raw flowmap color (R=flow_x, G=flow_y, B=flow_strength, A=flow_mask)
	float2 flowVector;  // Flow vector (coordinate space depends on source function)
};

/**
 * Gets raw flowmap data before UV-space coordinate transformation
 *
 * @param input Pixel shader input containing texture coordinates
 * @param uvShift UV offset for sampling the flowmap texture
 * @return FlowmapData with raw components:
 *         - color: Raw flowmap texture sample (RG=rotation, B=strength, A=mask)
 *         - flowVector: Base flow vector before any coordinate transformation
 *                      Ready for direct application of rotation matrix for world positioning
 *
 * @details This function provides flowmap data in its original coordinate space, suitable
 *          for world-space positioning effects (like ripple movement). The flowVector has
 *          NOT been transformed for UV-space normal sampling - that transformation is only
 *          applied in GetFlowmapDataUV() which uses transpose for UV coordinate perturbation.
 *
 *          Use this function when you need to apply the rotation matrix directly for
 *          world-space effects without needing to reverse any existing transformations.
 *
 * @see GetFlowmapDataUV() for UV-space normal sampling (applies transpose transformation)
 */
FlowmapData GetFlowmapDataTextureSpace(PS_INPUT input, float2 uvShift)
{
	FlowmapData data;
	data.color = FlowMapTex.SampleLevel(FlowMapSampler, input.TexCoord2.zw + uvShift, 0);
	data.flowVector = (64 * input.TexCoord3.xy) * sqrt(1.01 - data.color.z);
	// NOTE: flowVector is NOT transformed yet - this is the raw vector before rotation matrix
	return data;
}
/**
 * Samples flowmap texture and calculates UV-space flow data for texture sampling
 *
 * @param input Pixel shader input containing texture coordinates and world position data
 * @param uvShift UV offset for sampling the flowmap texture (used for animation/variation)
 * @return FlowmapData Complete flowmap information with UV-space flow vector
 *
 * @details This function:
 *          - Samples the flowmap texture at the specified UV coordinates
 *          - Decodes flow direction from RG channels (remapped from [0,1] to [-1,1])
 *          - Calculates flow strength using the blue channel with sqrt falloff
 *          - Applies transpose rotation matrix to transform flow direction to UV space
 *          - Scales flow vector by world position and strength factors
 *
 * @note Flowmap format:
 *       - Red channel: Flow direction X component (0.5 = no flow, 0/1 = negative/positive flow)
 *       - Green channel: Flow direction Y component (0.5 = no flow, 0/1 = negative/positive flow)
 *       - Blue channel: Flow strength (0 = no flow, 1 = maximum flow)
 *       - Alpha channel: Flow mask/intensity multiplier
 */
FlowmapData GetFlowmapDataUV(PS_INPUT input, float2 uvShift)
{
	FlowmapData data = GetFlowmapDataTextureSpace(input, uvShift);
	float2 flowSinCos = data.color.xy * 2 - 1;
	float2x2 flowRotationMatrix = float2x2(flowSinCos.x, flowSinCos.y, -flowSinCos.y, flowSinCos.x);
	data.flowVector = mul(transpose(flowRotationMatrix), data.flowVector);
	return data;
}
// ----------------------------------------------------------------
// Flowmap Parallax Functions
// ----------------------------------------------------------------

/**
 * Samples height from flowmap texture using the same 4-sample blend as flowmap normals
 * This ensures height transitions match the normal transitions exactly
 *
 * @param input PS_INPUT for flowmap coordinate access
 * @param normalMul The blend weights from the flowmap system (same as used for normals)
 * @param uvShift The UV shift value (1 / (128 * flowmapDimensions))
 * @param mipLevel Mip level for texture sampling
 */
float GetFlowmapHeightBlended(PS_INPUT input, float2 normalMul, float2 uvShift, float mipLevel)
{
	// Sample height using the EXACT same UV computation as GetFlowmapNormal
	// This ensures the height blending matches the normal blending perfectly

	// Sample 0: uvShift, multiplier=9.92, offset=0
	FlowmapData flowData0 = GetFlowmapDataUV(input, uvShift);
	float2 uv0 = 0 + (flowData0.flowVector - float2(9.92 * ((0.001 * ReflectionColor.w) * flowData0.color.w), 0));
	float height0 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv0, mipLevel).w;

	// Sample 1: float2(0, uvShift.y), multiplier=10.64, offset=0.27
	FlowmapData flowData1 = GetFlowmapDataUV(input, float2(0, uvShift.y));
	float2 uv1 = 0.27 + (flowData1.flowVector - float2(10.64 * ((0.001 * ReflectionColor.w) * flowData1.color.w), 0));
	float height1 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv1, mipLevel).w;

	// Sample 2: 0.0.xx, multiplier=8, offset=0
	FlowmapData flowData2 = GetFlowmapDataUV(input, 0.0.xx);
	float2 uv2 = 0 + (flowData2.flowVector - float2(8 * ((0.001 * ReflectionColor.w) * flowData2.color.w), 0));
	float height2 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv2, mipLevel).w;

	// Sample 3: float2(uvShift.x, 0), multiplier=8.48, offset=0.62
	FlowmapData flowData3 = GetFlowmapDataUV(input, float2(uvShift.x, 0));
	float2 uv3 = 0.62 + (flowData3.flowVector - float2(8.48 * ((0.001 * ReflectionColor.w) * flowData3.color.w), 0));
	float height3 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv3, mipLevel).w;

	// Use the EXACT same blending formula as flowmap normals
	float blendedHeight =
		normalMul.y * (normalMul.x * height2 + (1 - normalMul.x) * height3) +
		(1 - normalMul.y) * (normalMul.x * height1 + (1 - normalMul.x) * height0);

	return blendedHeight;
}

// Keep this for compatibility - just forwards to the proper function
float GetFlowmapHeightBarycentric(PS_INPUT input, float2 flowmapDimensions, float2 baseUV, float mipLevel)
{
	// This is now unused - we use GetFlowmapHeightBlended directly
	return FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, baseUV, mipLevel).w;
}

/**
 * Computes mip level from continuous (non-flowmapped) UVs.
 * Flow-rotated UVs have discontinuous ddx/ddy and must not be used for LOD.
 */
float GetFlowmapMipLevel(float2 baseUV)
{
	float2 textureDims;
	FlowMapNormalsTex.GetDimensions(textureDims.x, textureDims.y);

	float2 texCoordsPerSize = baseUV * textureDims;
	float2 dxSize = ddx(texCoordsPerSize);
	float2 dySize = ddy(texCoordsPerSize);
	float2 dTexCoords = dxSize * dxSize + dySize * dySize;
	float minTexCoordDelta = max(dTexCoords.x, dTexCoords.y);
	return max(0.5 * log2(minTexCoordDelta), 0);
}

/**
 * Samples height from flowmap texture (riverflow.dds alpha channel)
 * Uses the same UV calculation as GetFlowmapNormal for consistency
 */

/**
 * Generates flowmap-based normal (no parallax - flowmap normals are not parallax-shifted).
 * Mip LOD uses non-flowmapped TexCoord3 derivatives — flow-rotated UVs break hardware/manual
 * mip selection (stuck mip 0 shimmer, or excessive LOD + UV shrink that flattens normals).
 */
float3 GetFlowmapNormal(PS_INPUT input, float2 uvShift, float multiplier, float offset, float2 dx, float2 dy)
{
	FlowmapData flowData = GetFlowmapDataUV(input, uvShift);
	float2 uv = offset + (flowData.flowVector - float2(multiplier * ((0.001 * ReflectionColor.w) * flowData.color.w), 0));

	// Match UV footprint: flowVector scales by this (see GetFlowmapDataTextureSpace)
	float flowStrength = sqrt(1.01 - flowData.color.z);
	float mipScale = exp2(SharedData::MipBias) * flowStrength;

	return float3(FlowMapNormalsTex.SampleGrad(FlowMapNormalsSampler, uv, dx * mipScale, dy * mipScale).xy, flowData.color.z);
}

/**
 * Gets flowmap data with world-space flow vector for positioning effects
 *
 * @param input Pixel shader input containing texture coordinates
 * @param uvShift UV offset for flowmap sampling (used for animation phases)
 * @return FlowmapData Complete flowmap information with world-space flow vector
 *
 * @details This function:
 *          - Samples raw flowmap data (before UV-space transformations)
 *          - Decodes flow direction from flowmap RG channels
 *          - Applies component-wise directional transformation
 *          - Returns complete flowmap data with world-space flow vector
 *
 * @note Use this for effects that need to move with water current (ripples, debris, foam, etc.)
 *       For UV-space normal sampling, use GetFlowmapDataUV() instead
 */
FlowmapData GetFlowmapDataWorldSpace(PS_INPUT input, float2 uvShift)
{
	FlowmapData data = GetFlowmapDataTextureSpace(input, uvShift);
	float2 flowDirection = -(data.color.xy * 2 - 1);    // Decode direction with 180° correction
	data.flowVector = data.flowVector * flowDirection;  // Transform to world space
	return data;
}

/**
 * Converts existing texture-space flowmap data to world-space (avoids duplicate sampling)
 *
 * @param textureSpaceData FlowmapData from GetFlowmapDataTextureSpace()
 * @return FlowmapData Complete flowmap data with world-space flow vector
 *
 * @note Use this overload when you already have texture-space flowmap data to avoid duplicate texture sampling
 */
FlowmapData GetFlowmapDataWorldSpace(FlowmapData textureSpaceData)
{
	FlowmapData data = textureSpaceData;
	float2 flowDirection = -(data.color.xy * 2 - 1);    // Decode direction with 180° correction
	data.flowVector = data.flowVector * flowDirection;  // Transform to world space
	return data;
}
#			endif

#			if defined(LOD)
#				undef WATER_EFFECTS
#				undef WETNESS_EFFECTS
#			endif

#			if defined(WATER_EFFECTS) && !defined(VC)
#				define WATER_PARALLAX
#				include "WaterEffects/WaterParallax.hlsli"
#			endif

#			if defined(DYNAMIC_CUBEMAPS)
#				include "DynamicCubemaps/DynamicCubemaps.hlsli"
#			endif

#			if defined(WETNESS_EFFECTS)
#				include "WetnessEffects/WetnessEffects.hlsli"
#			endif

// Structure to return both normal and ripple/splash color information
struct WaterNormalData
{
	float3 normal;
	float4 rippleInfo;  // xyz = scaled ripple normal (normalized normal * intensity), w = splash effect intensity
#			if defined(PBR_WATER)
	float detailVariance;  // slope variance the mip filter averaged out of the normal maps (Toksvig), at their blended amplitude
#			endif
};

#			if defined(PBR_WATER)
/**
 * Toksvig (2005): a mip-filtered normal map texel averages normals that point different ways, so the decoded
 * normal comes out shorter than 1. Its shortening gives the slope variance the filter removed,
 * (1 - |n|) / |n|, which the roughness takes over so distant detail turns into gloss instead of aliasing.
 * A magnified map shortens its normals too, between texels, where nothing was filtered away: that share is
 * dropped (PBRWater::MinifiedShare).
 * @param n   decoded tangent-space normal before normalisation
 * @param map the normal map it was sampled from, at `uv` with the material mip bias
 */
float ToksvigVariance(float3 n, Texture2D<float4> map, float2 uv)
{
	float width, height;
	map.GetDimensions(width, height);
	float2 texels = float2(width, height) * exp2(SharedData::MipBias);
	float len = length(n);
	return max(0.0, (1.0 - len) / max(len, 1e-3)) * PBRWater::MinifiedShare(ddx_fine(uv) * texels, ddy_fine(uv) * texels);
}
#			endif

WaterNormalData GetWaterNormal(PS_INPUT input, float distanceFactor, float normalsDepthFactor, float3 viewDirection, float depth, float wetnessOcclusion)
{
	WaterNormalData result;
	result.rippleInfo = float4(0, 0, 0, 0);
#			if defined(PBR_WATER)
	result.detailVariance = 0.0;
#			endif

	float3 normalScalesRcp = rcp(input.NormalsScale.xyz);

	float4 normalsAmplitude = NormalsAmplitude;
	if (SharedData::enbSettings.EnableWater) {
		normalsAmplitude *= SharedData::enbSettings.WaterWavesAmplitude;
	}

#			if defined(FLUID_FIELD) && !defined(FLUID_FIELD_FALLS)
	FluidField::LayerFlow fluidLayerFlow;
#				define FLUID_LAYER(tex, smp, uv, parallaxUv, k) FluidField::SampleLayer(tex, smp, uv, parallaxUv, normalScalesRcp.k, fluidLayerFlow)
	{
		// Fluid Field's surface deformation reshapes vanilla's own normals (D17): every normal
		// texture lookup below, base and flowmap, shifts with the simulated slope, and the ripples
		// get rougher where the surface is steep
		float2 fluidWarp;
		float fluidRoughness;
		FluidField::GetVanillaWarp(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, fluidWarp, fluidRoughness);
		FluidField::PixelRoughness = fluidRoughness;
		input.TexCoord1.xy += fluidWarp * normalScalesRcp.x;
		input.TexCoord1.zw += fluidWarp * normalScalesRcp.y;
		input.TexCoord2.xy += fluidWarp * normalScalesRcp.z;
		fluidLayerFlow = FluidField::GetLayerFlow(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, fluidWarp);
#				if defined(FLOWMAP)
		// Flowmap normals use their own coordinates; convert world units with the screen derivatives
		float worldPerPixel = length(ddx(input.WPosition.xy)) + 1e-4;
		input.TexCoord3.xy += fluidWarp * (length(ddx(input.TexCoord3.xy)) / worldPerPixel);
#				endif
		normalsAmplitude.xyz *= FluidField::RippleBoost(fluidRoughness, viewDirection);
	}
#			else
#				define FLUID_LAYER(tex, smp, uv, parallaxUv, k) tex.SampleBias(smp, (uv) + (parallaxUv), SharedData::MipBias)
#			endif

#			if defined(WATER_PARALLAX)
	float2 parallaxOffset = WaterEffects::GetParallaxOffset(input, normalsAmplitude.xyz, normalScalesRcp);
#			endif

#			if defined(FLOWMAP)
#				if defined(UNIFIED_WATER)
	float2 flowmapDimensions = input.TexCoord4.xy;
#				else
	float2 flowmapDimensions = input.TexCoord4.xx;
#				endif
	float2 uvShift = 1 / (128 * flowmapDimensions);

	// Compute flowmap parallax and create parallaxed input for normal sampling
	PS_INPUT flowmapInput = input;
	float2 flowmapParallaxOffset = float2(0, 0);
#				if defined(WATER_PARALLAX) && !defined(LOD)
	float parallaxAmount = WaterEffects::GetFlowmapParallaxAmount(input, flowmapDimensions, viewDirection);
	float2 parallaxDir = viewDirection.xy / -viewDirection.z;
	parallaxDir.y = -parallaxDir.y;
	float viewDotUp = -viewDirection.z;
	parallaxDir *= 0.008 * saturate(viewDotUp * 2.0);
	flowmapInput.TexCoord3.xy = input.TexCoord3.xy + parallaxAmount * parallaxDir;
	flowmapParallaxOffset = WaterEffects::GetFlowmapParallaxOffset(input, flowmapDimensions, viewDirection, normalsAmplitude.x, normalScalesRcp);
#				endif

	// Calculate cell blend weights using parallaxed input
	float2 normalMul = 0.5 + -(-0.5 + abs(frac(flowmapInput.TexCoord2.zw * (64 * flowmapDimensions)) * 2 - 1));

	// Same UV scale as flowVector before rotation — shared so all 4 blend samples use matching LOD
	float2 baseUV = 64 * flowmapInput.TexCoord3.xy;
	float2 flowNormalDx = ddx(baseUV);
	float2 flowNormalDy = ddy(baseUV);

	// Sample flowmap normals with parallax applied
	float3 flowmapNormal0 = GetFlowmapNormal(flowmapInput, uvShift, 9.92, 0, flowNormalDx, flowNormalDy);
	float3 flowmapNormal1 = GetFlowmapNormal(flowmapInput, float2(0, uvShift.y), 10.64, 0.27, flowNormalDx, flowNormalDy);
	float3 flowmapNormal2 = GetFlowmapNormal(flowmapInput, 0.0.xx, 8, 0, flowNormalDx, flowNormalDy);
	float3 flowmapNormal3 = GetFlowmapNormal(flowmapInput, float2(uvShift.x, 0), 8.48, 0.62, flowNormalDx, flowNormalDy);

	float2 flowmapNormalWeighted =
		normalMul.y * (normalMul.x * flowmapNormal2.xy + (1 - normalMul.x) * flowmapNormal3.xy) +
		(1 - normalMul.y) *
			(normalMul.x * flowmapNormal1.xy + (1 - normalMul.x) * flowmapNormal0.xy);
	float2 flowmapDenominator = sqrt(normalMul * normalMul + (1 - normalMul) * (1 - normalMul));
	float3 flowmapNormal =
		float3(((-0.5 + flowmapNormalWeighted) / (flowmapDenominator.x * flowmapDenominator.y)) *
				   max(0.4, normalsDepthFactor),
			0);
	flowmapNormal.z =
		sqrt(1 - flowmapNormal.x * flowmapNormal.x - flowmapNormal.y * flowmapNormal.y);
	float2 baseNormalParallax = 0.0;
#				if defined(WATER_PARALLAX)
	// Use flowmap-derived parallax offset for base normals
	baseNormalParallax = flowmapParallaxOffset.xy * normalScalesRcp.x;
#				endif
	float3 normals1 = FLUID_LAYER(Normals01Tex, Normals01Sampler, input.TexCoord1.xy, baseNormalParallax, x).xyz * 2.0 + float3(-1, -1, -2);
	float2 baseNormalUv = input.TexCoord1.xy + baseNormalParallax;
#			endif  // End of FLOWMAP block

#			if !defined(FLOWMAP)
#				if defined(PBR_WATER)
	float2 baseNormalUv = input.TexCoord1.xy;
#				endif
#				if defined(WATER_PARALLAX)
	float3 normals1 = FLUID_LAYER(Normals01Tex, Normals01Sampler, input.TexCoord1.xy, parallaxOffset.xy * normalScalesRcp.x, x).xyz * 2.0 + float3(-1, -1, -2);
#				else
	float3 normals1 = FLUID_LAYER(Normals01Tex, Normals01Sampler, input.TexCoord1.xy, 0.0, x).xyz * 2.0 + float3(-1, -1, -2);
#				endif
#			endif  // End of !FLOWMAP block
#			if defined(FLOWMAP) && !defined(BLEND_NORMALS)
#				ifdef DISABLE_FLOWMAP_NORMALS
	// FLOWMAP NORMALS DISABLED: Using only base normals (flow system still active for ripples/splashes)
	float3 finalNormal = normalize(normals1 + float3(0, 0, 1));
#					if defined(PBR_WATER)
	result.detailVariance = ToksvigVariance(normals1 + float3(0, 0, 1), Normals01Tex, baseNormalUv);
#					endif
#				else
	// FLOWMAP NORMALS ENABLED: Blending flow-based normals with base normals
#					if defined(FLUID_FIELD)
#						if defined(FLUID_FIELD_FALLS)
	// D174: no flowmap normal on the falls (the river simulation's weight at their xy switched it on under overhangs)
	float3 finalNormal = normalize(normals1 + float3(0, 0, 1));
#						else
	float3 finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), flowmapNormal, distanceFactor * FluidField::FlowmapWeight(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz)));
#						endif
#					else
	float3 finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), flowmapNormal, distanceFactor));
#					endif
#					if defined(PBR_WATER)
	result.detailVariance = ToksvigVariance(normals1 + float3(0, 0, 1), Normals01Tex, baseNormalUv) * (1.0 - distanceFactor) * (1.0 - distanceFactor);
#					endif
#				endif
#			elif !defined(LOD)

#				if defined(WATER_PARALLAX)
	float3 normals2 = FLUID_LAYER(Normals02Tex, Normals02Sampler, input.TexCoord1.zw, parallaxOffset.xy * normalScalesRcp.y, y).xyz * 2.0 - 1.0;
	float3 normals3 = FLUID_LAYER(Normals03Tex, Normals03Sampler, input.TexCoord2.xy, parallaxOffset.xy * normalScalesRcp.z, z).xyz * 2.0 - 1.0;
#				else
	float3 normals2 = FLUID_LAYER(Normals02Tex, Normals02Sampler, input.TexCoord1.zw, 0.0, y).xyz * 2.0 - 1.0;
	float3 normals3 = FLUID_LAYER(Normals03Tex, Normals03Sampler, input.TexCoord2.xy, 0.0, z).xyz * 2.0 - 1.0;
#				endif

	float3 blendedNormal = normalize(float3(0, 0, 1) + normalsAmplitude.x * normals1 +
									 normalsAmplitude.y * normals2 + normalsAmplitude.z * normals3);
#				if defined(UNDERWATER)
	float3 finalNormal = blendedNormal;
#				else
	float3 finalNormal = normalize(lerp(float3(0, 0, 1), blendedNormal, normalsDepthFactor));
#				endif
#				if defined(PBR_WATER)
	{
		// The maps' slopes enter the blend with their amplitudes over its z of about 1 + a.y + a.z (normals1 is
		// stored offset by -1 in z); the depth fade scales the slopes once more.
		float3 a = normalsAmplitude.xyz;
		float blendZ = 1.0 + a.y + a.z;
		float variance = (a.x * a.x * ToksvigVariance(normals1 + float3(0, 0, 1), Normals01Tex, baseNormalUv) + a.y * a.y * ToksvigVariance(normals2, Normals02Tex, input.TexCoord1.zw) + a.z * a.z * ToksvigVariance(normals3, Normals03Tex, input.TexCoord2.xy)) / (blendZ * blendZ);
#					if !defined(UNDERWATER)
		variance *= normalsDepthFactor * normalsDepthFactor;
#					endif
		result.detailVariance = variance;
	}
#				endif

#				if defined(FLOWMAP)
	float normalBlendFactor =
		normalMul.y * ((1 - normalMul.x) * flowmapNormal3.z + normalMul.x * flowmapNormal2.z) +
		(1 - normalMul.y) * (normalMul.x * flowmapNormal1.z + (1 - normalMul.x) * flowmapNormal0.z);
#					if defined(FLUID_FIELD)
	// Still simulated water keeps vanilla's wind ripples instead of flow normals
#						if defined(FLUID_FIELD_FALLS)
	normalBlendFactor = 0.0;  // D174: as above, no flowmap normal on the falls
#						else
	normalBlendFactor *= FluidField::FlowmapWeight(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz);
#						endif
#					endif
	finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), normalize(lerp(finalNormal, flowmapNormal, normalBlendFactor)), distanceFactor));
#					if defined(PBR_WATER)
	result.detailVariance = lerp(ToksvigVariance(normals1 + float3(0, 0, 1), Normals01Tex, baseNormalUv), result.detailVariance * (1.0 - normalBlendFactor) * (1.0 - normalBlendFactor), distanceFactor);
#					endif
#				endif
#			else
	float3 finalNormal =
		normalize(float3(0, 0, 1) + normalsAmplitude.xxx * normals1);
#				if defined(PBR_WATER)
	result.detailVariance = normalsAmplitude.x * normalsAmplitude.x * ToksvigVariance(normals1 + float3(0, 0, 1), Normals01Tex, baseNormalUv);
#				endif
#			endif

	// PBR water has no vanilla wading displacement (TESWaterSystem_InitializeWater: noDisplacement), its ripple simulation
	// stirs the real surface instead. The engine still flags the water as waded while the player is in it: reading the
	// never-written DisplacementTex then tipped every pixel's normal over and the whole surface turned into a mirror.
#			if (defined(WADING_RT) || defined(WADING)) && !defined(PBR_WATER)
#				if defined(WADING_RT)
	const bool isWading = Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Wading;
#				else
	const bool isWading = true;
#				endif
	if (isWading) {
#				if defined(FLOWMAP)
		float2 displacementUv = input.TexCoord3.zw;
#				else
		float2 displacementUv = input.TexCoord3.xy;
#				endif
		float3 displacement = normalize(float3(normalsAmplitude.w * (-0.5 + DisplacementTex.Sample(DisplacementSampler, displacementUv).zw),
			0.04));
#				if defined(FLUID_FIELD)
		// Fluid Field's wakes replace vanilla's displacement trail where it simulates the water (D17)
		displacement = normalize(lerp(displacement, float3(0, 0, 1), FluidField::GetCoverage(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz)));
#				endif
		finalNormal = lerp(displacement, finalNormal, displacement.z);
	}
#			endif

#			if defined(FLUID_FIELD) && !defined(FLUID_FIELD_FALLS) && !defined(PBR_WATER)
	{
		// Fluid Field's simulated waves on top of the vanilla normal, at the parallax-shifted position (PBR Water takes
		// the sim's slope in its own shading instead)
		float3 fluidFieldPosition = input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
#				if defined(WATER_PARALLAX)
		fluidFieldPosition.xy += parallaxOffset;
#				endif
		finalNormal = FluidField::ApplyWaterNormal(fluidFieldPosition, finalNormal);
#				if !defined(UNDERWATER)
		finalNormal = FluidField::KeepReflectionAboveHorizon(finalNormal, viewDirection, fluidFieldPosition);
#				endif
	}
#			endif

#			if defined(WETNESS_EFFECTS)
	// Wetness Effects Debug System:
	// DEBUG_WETNESS_EFFECTS Color Legend:
	// - BRIGHT MAGENTA: Ripples, BRIGHT GREEN: Splashes, CYAN: Both effects
	float4 raindropInfo = float4(0, 0, 1, 0);
	float maxRainDropDistance = SharedData::wetnessEffectsSettings.RaindropFxRange * SharedData::wetnessEffectsSettings.RaindropFxRange * 3;
	float rainDropDistance = dot(input.WPosition.xyz, input.WPosition.xyz);
	float distanceFadeout = saturate((1 - saturate(rainDropDistance / maxRainDropDistance)) * 3);
	if (finalNormal.z > 0 && SharedData::wetnessEffectsSettings.Raining > 0.0f && SharedData::wetnessEffectsSettings.EnableRaindropFx &&
		(rainDropDistance < maxRainDropDistance) && wetnessOcclusion > 0.05) {
		float rippleStrengthModifier = (wetnessOcclusion * wetnessOcclusion) * distanceFadeout;
		float3 rippleWPosition = input.WPosition.xyz + finalNormal * 16;
#				if defined(WATER_PARALLAX)
		rippleWPosition.xy += parallaxOffset;
#				endif
#				if defined(FLOWMAP)
		// Flow-following ripple enhancement: Makes raindrops follow water current
		FlowmapData worldFlowData = GetFlowmapDataWorldSpace(input, float2(0, 0));

		// Calculate flow-aware ripple offset using centralized timing logic
		// Parameters: avgFlowmapMultiplier=9.26 (average of GetWaterNormal flowmap normal multipliers: 9.92, 10.64, 8, 8.48)
		// uvToWorldScale=0.125 (1/8 - relates to 64× texture coordinate scaling factor)
		float2 flowOffset = WetnessEffects::GetFlowAwareRippleOffset(
			worldFlowData.flowVector,
			worldFlowData.color.w,      // Flow strength from flowmap alpha
			0.001 * ReflectionColor.w,  // Reflection timing scale (matches GetFlowmapNormal)
			9.26,                       // Average flowmap normal multiplier
			0.125                       // UV-to-world scale factor (1/8)
		);

		rippleWPosition.xy += flowOffset;
#				endif
		raindropInfo = WetnessEffects::GetRainDrops(rippleWPosition + FrameBuffer::CameraPosAdjust.xyz, SharedData::wetnessEffectsSettings.Time, finalNormal, rippleStrengthModifier);

		// Calculate ripple and splash color intensities
		float rippleIntensity = length(raindropInfo.xy) * rippleStrengthModifier;
		float splashIntensity = raindropInfo.w * distanceFadeout;

		// Store ripple and splash information for color effects
		result.rippleInfo.xyz = raindropInfo.xyz * rippleIntensity;
		result.rippleInfo.w = splashIntensity;
	}
	float3 rippleNormal = normalize(raindropInfo.xyz);
	finalNormal = ReorientNormal(rippleNormal, finalNormal);
#			endif

	result.normal = finalNormal;
	return result;
}

float3 GetWaterSpecularColor(PS_INPUT input, float3 normal, float3 viewDirection, float distanceFactor, float skylightingSpecular, float roughness = 0.0)
{
#			if !defined(DYNAMIC_CUBEMAPS)
	if (!(Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Reflections))
		return ReflectionColor.xyz * VarAmounts.y;
#			endif

#			if defined(FLUID_FIELD_FALLS)
	float3 R = reflect(viewDirection, WaterParams.y * normal + FallUp * (1 - WaterParams.y));
	// D174: falling water is rough: a blurred sky
	const float pbrCubeLevel = FallShading::REFLECTION_BLUR_MIPS;
	const float pbrEnvironmentLevel = FallShading::REFLECTION_BLUR_MIPS;
#			else
	float3 R = reflect(viewDirection, WaterParams.y * normal + float3(0, 0, 1 - WaterParams.y));
#				if defined(PBR_WATER)
	// Glossy reflections: rough water (wind, rain, or wave detail filtered out at a distance) reflects a
	// blurred environment, through the reflection cubemap's mips when it has them.
	uint pbrCubeWidth, pbrCubeHeight, pbrCubeLevels;
	CubeMapTex.GetDimensions(0, pbrCubeWidth, pbrCubeHeight, pbrCubeLevels);
	float pbrCubeLevel = roughness * (float)(pbrCubeLevels - 1);
	float pbrEnvironmentLevel = roughness * 8.0;
#				else
	float pbrCubeLevel = 0;
	float pbrEnvironmentLevel = 0;
#				endif
#			endif

#			if defined(DYNAMIC_CUBEMAPS)
	if (SharedData::InInterior)
		return DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, pbrEnvironmentLevel).xyz;

	float3 workingSpecularIrradiance = 0.0;
	if (skylightingSpecular < 1.0)
		workingSpecularIrradiance = DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, pbrEnvironmentLevel).xyz;

	float3 workingSpecularIrradianceReflections = 0.0;
	if (skylightingSpecular > 0.0)
		workingSpecularIrradianceReflections = DynamicCubemaps::EnvReflectionsTexture.SampleLevel(CubeMapSampler, R, pbrEnvironmentLevel).xyz;

	float3 dynamicCubemap = ColorManagement::SceneColor::LerpInLinear(workingSpecularIrradiance, workingSpecularIrradianceReflections, skylightingSpecular);

	float3 reflectionColor = dynamicCubemap;
#				if !defined(PBR_WATER)
	// PBR water keeps one environment at every distance: switching to the vanilla reflection cubemap (much darker)
	// past 1024 units made the whole water change brightness as the camera rose above it.
	if ((Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Reflections) && !SharedData::HideSky) {
		float reflectionAmount = saturate(length(input.WPosition.xyz) / 1024.0);
		reflectionColor = lerp(dynamicCubemap, CubeMapTex.SampleLevel(CubeMapSampler, R, pbrCubeLevel).xyz, reflectionAmount);
	}
#				endif
#			else
	float3 reflectionColor = CubeMapTex.SampleLevel(CubeMapSampler, R, pbrCubeLevel).xyz;
#			endif

#			if defined(FLUID_FIELD_FALLS)
	{
		// D159: the engine's SSR assumes a level plane and the water mask: the falls march their own ray (D155) through the scene behind
		const float pointingDirection = dot(viewDirection, R) * 0.5 + 0.5;
		const float pointingAlignment = dot(reflect(viewDirection, FallUp), R) * 0.5 + 0.5;
		const float ssrAmount = sqrt(saturate(min(pointingAlignment, pointingDirection)));
		float2 dimensions;
		RefractionTex.GetDimensions(dimensions.x, dimensions.y);
		// D189: per-pixel, per-frame tilt on rough water and a jittered first step, resolved over frames
		const float3 spread = float3(Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount),
								  Random::InterleavedGradientNoise(input.HPosition.xy + 17.3, SharedData::FrameCount),
								  Random::InterleavedGradientNoise(input.HPosition.xy + 41.7, SharedData::FrameCount)) -
		                      0.5;
		float3 rough = FallShading::SafeNormalize(R + spread * (2.0 * FallRoughness), R);
		rough = dot(rough, FallUp) < 0.0 ? rough - FallUp * (2.0 * dot(rough, FallUp)) : rough;
		const float jitter = Random::InterleavedGradientNoise(input.HPosition.xy + 5.1, SharedData::FrameCount);
		const float4 hit = FallShading::ScreenReflection(RefractionTex, DepthTex, RefractionSampler, input.WPosition.xyz, FallShading::SafeNormalize(rough, FallUp), dimensions, dimensions * FrameBuffer::DynamicResolutionParams1.xy, jitter);
		reflectionColor = lerp(reflectionColor, max(0, hit.xyz), saturate(hit.w * distanceFactor * ssrAmount));
	}
#			elif !defined(LOD) && NUM_SPECULAR_LIGHTS == 0
	float pointingDirection = dot(viewDirection, R) * 0.5 + 0.5;
	float pointingAlignment = dot(reflect(viewDirection, float3(0, 0, 1)), R) * 0.5 + 0.5;
#				if defined(PBR_WATER)
	// The reflection normal is a blend, not unit length, so the dots can dip just below -1 and the
	// sqrt would return NaN.
	float ssrAmount = sqrt(max(min(pointingAlignment, pointingDirection), 0.0));
#				else
	float ssrAmount = sqrt(min(pointingAlignment, pointingDirection));
#				endif
	float2 ssrReflectionUv = ((FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy) * SSRParams.zw) + 0.05 * normal.xy;
	float2 ssrReflectionUvDR = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(ssrReflectionUv);
	float4 ssrReflectionColorBlurred = SSRReflectionTex.Sample(SSRReflectionSampler, ssrReflectionUvDR);
	float4 ssrReflectionColorRaw = RawSSRReflectionTex.Sample(RawSSRReflectionSampler, ssrReflectionUvDR);
#				if defined(PBR_WATER)
	// Rough water only shows the blurred screen-space reflection.
	float4 ssrReflectionColor = lerp(ssrReflectionColorBlurred, ssrReflectionColorRaw, ssrAmount * 0.7 * saturate(1.0 - roughness * 3.0));
#				else
	float4 ssrReflectionColor = lerp(ssrReflectionColorBlurred, ssrReflectionColorRaw, ssrAmount * 0.7);
#				endif
	float3 finalSsrReflectionColor = max(0, ssrReflectionColor.xyz);
	float ssrFraction = saturate(ssrReflectionColor.w * distanceFactor * ssrAmount);
	reflectionColor = lerp(reflectionColor, finalSsrReflectionColor, ssrFraction);
#			endif

	return reflectionColor;
}

// Effects 11 water: depth over which the surface reaches full opacity at the shore
static const float ShoreFadeDepth = 12.0;

float GetScreenDepthWater(float2 screenPosition)
{
	float depth = DepthTex.Load(float3(screenPosition, 0)).x;
	return (CameraDataWater.w / (-depth * CameraDataWater.z + CameraDataWater.x));
}

float3 GetLdotN(float3 normal)
{
#			if defined(UNDERWATER)
	return 1;
#			else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 1;
	return saturate(dot(SunDir.xyz, normal));
#			endif
}

float GetFresnelValue(float3 normal, float3 viewDirection)
{
#			if defined(UNDERWATER)
	float3 actualNormal = -normal;
#			else
	float3 actualNormal = normal;
#			endif
	float viewAngle = 1 - saturate(dot(-viewDirection, actualNormal));

	if (SharedData::enbSettings.EnableWater) {
		float fresnelExponent = SharedData::enbSettings.WaterFresnelMultiplier;
		// pow(0, 0) is NaN on GPU
		float fresnelRI = fresnelExponent != 0.0 ? pow(abs(FresnelRI.x), fresnelExponent) : 1.0;
		float fresnel = (1 - fresnelRI) * pow(viewAngle, 5) + fresnelRI;
		fresnel = lerp(SharedData::enbSettings.WaterFresnelMin, SharedData::enbSettings.WaterFresnelMax, fresnel);
		return fresnel * SharedData::enbSettings.WaterReflectionAmount;
	}

	return (1 - FresnelRI.x) * pow(viewAngle, 5) + FresnelRI.x;
}

struct DiffuseOutput
{
	float3 refractionColor;
	float3 refractionDiffuseColor;
	float depth;
	float refractionMul;
	float3 refractedViewDirection;
};

DiffuseOutput GetWaterDiffuseColor(PS_INPUT input, float3 normal, float3 viewDirection, inout float4 distanceMul, float refractionsDepthFactor, float fresnel, float3 viewPosition, float depth)
{
#			if defined(REFRACTIONS)
#				if defined(FLUID_FIELD_FALLS)
	// D159: the river's offset along the sheet, by the normal's tilt off the sheet's own normal (MPosition is camera-relative here)
	const float3 fallTilt = normal - dot(normal, FallUp) * FallUp;
	const float4 refractionClip = mul(FrameBuffer::CameraViewProj, float4(input.MPosition.xyz + VarAmounts.w * refractionsDepthFactor * FallShading::FLOW_REFRACTION * fallTilt, 1));
	float2 refractionUvRaw = saturate(refractionClip.xy / max(refractionClip.w, 1e-4) * float2(0.5, -0.5) + 0.5);
#				else
#					if defined(PBR_WATER)
	float refractionMagnitude = VarAmounts.w * PBRWater::Light1.z;
#					else
	float refractionMagnitude = VarAmounts.w;
#					endif
	float4 refractionNormal = mul(transpose(TextureProj), float4((refractionMagnitude * refractionsDepthFactor * normal.xy) + input.MPosition.xy, input.MPosition.z, 1));

	float2 refractionUvRaw = float2(refractionNormal.x, refractionNormal.w - refractionNormal.y) / refractionNormal.ww;
#				endif
	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

	float2 refractionScreenPosition = FrameBuffer::DynamicResolutionParams1.xy * (refractionUvRaw / VPOSOffset.xy);
	float4 refractionWorldPosition = float4(input.WPosition.xyz * depth / viewPosition.z, 0);

#				if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	float refractionDepth = GetScreenDepthWater(refractionScreenPosition);
	depth = refractionDepth;
	float refractionDepthMul = length(float3((((VPOSOffset.zw + refractionUvRaw) * 2 - 1)) * refractionDepth / ProjData.xy, refractionDepth));

	float3 refractionDepthAdjustedViewDirection = -viewDirection * refractionDepthMul;
	float refractionViewSurfaceAngle = dot(refractionDepthAdjustedViewDirection, GetWaterPlane().xyz);

	float refractionPlaneMul = (1 - GetWaterPlane().w / refractionViewSurfaceAngle);

	if (refractionPlaneMul < 0.0) {
		refractionUvRaw = FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;
	} else {
		float muddiness = SharedData::enbSettings.EnableWater ? SharedData::enbSettings.WaterMuddiness : 1.0;
		distanceMul = saturate(float4(1, muddiness, 1, 1) * refractionPlaneMul * float4(length(refractionDepthAdjustedViewDirection).xx, abs(refractionViewSurfaceAngle).xx) / FogParam.z);

		refractionWorldPosition = mul(FrameBuffer::CameraViewProjInverse, float4((refractionUvRaw * 2 - 1) * float2(1, -1), DepthTex.Load(float3(refractionScreenPosition, 0)).x, 1));
		refractionWorldPosition.xyz /= refractionWorldPosition.w;
	}

#					if defined(HORIZON_FIX)
	if (DepthTex.Load(float3(refractionScreenPosition, 0)).x >= HorizonFix::EmptyDepthThreshold)
		distanceMul = 1.0.xxxx;
#					endif
#					if defined(FLUID_FIELD_FALLS)
	// D180: a falling sheet holds its own thickness along the view, not the (mostly air) path behind it, which read as deep, opaque water
	// ...and so does a crest bulging over its lip (where FallFalling is 0): through it the view reached the bed far below and read as
	// deep, opaque water. A falls sheet is a film, whatever lies behind it (under a dome that is mostly air): at most SHEET_FILM
	distanceMul = min(distanceMul, saturate(min(FallShading::SHEET_DEPTH / max(abs(dot(viewDirection, FallUp)), 0.25), FallShading::SHEET_FILM) / FogParam.z));
#					endif
#				endif

	float2 refractionUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(refractionUvRaw);
	float3 refractionColor = RefractionTex.Sample(RefractionSampler, refractionUV).xyz;

#				if defined(UNDERWATER)
	float refractionMul = 0;
#				else
	float refractionMul = 1 - pow(saturate((-distanceMul.x * FogParam.z + FogParam.z) / FogParam.w), FogNearColor.w);
#				endif

	float3 refractionDiffuseColor;

#				if defined(PBR_WATER) && !defined(UNDERWATER)
	// PBR Water: the body colour is the light an optically deep column scatters back, the water form's deep colour.
	// Its shallow colour is the column's transmission tint (Beer-Lambert in the water column, Water.hlsl), not a colour
	// of its own. Vanilla's blend by distanceMul left the body at the shallow colour wherever its refraction-plane test
	// failed (water at the eye seen edge-on, anything just under the surface): an olive line along the horizon and
	// olive shapes around everything in the water, the underwater fog's colour.
	refractionDiffuseColor = DeepColor.xyz;
#				else
	if (SharedData::enbSettings.EnableWater) {
		float3 shallowColor = ShallowColor.xyz;
		float maxValue = max(shallowColor.x, max(shallowColor.y, shallowColor.z));
		if (maxValue > 0.0)
			shallowColor /= maxValue;
		else
			shallowColor = 1.0;

		// Tinted refraction only reads near the surface; deeper water fades to the flat shallow color
		shallowColor = lerp(shallowColor.xyz * refractionColor, ShallowColor.xyz, lerp(SharedData::enbSettings.WaterMuddiness, 1.0, refractionMul));
		refractionDiffuseColor = lerp(shallowColor.xyz, DeepColor.xyz, distanceMul.y);
	} else {
		refractionDiffuseColor = lerp(ShallowColor.xyz, DeepColor.xyz, distanceMul.y);
	}
#				endif

	DiffuseOutput output;
	output.refractionColor = refractionColor;
	output.refractionDiffuseColor = refractionDiffuseColor;
	output.depth = depth;
	output.refractionMul = refractionMul;
	output.refractedViewDirection = normalize(refractionWorldPosition.xyz - input.WPosition.xyz);
	return output;
#			else
	DiffuseOutput output;
	output.refractionColor = lerp(ShallowColor.xyz, DeepColor.xyz, fresnel) * GetLdotN(normal);
	output.refractionDiffuseColor = output.refractionColor;
	output.depth = 1;
	output.refractionMul = 1;
	output.refractedViewDirection = viewDirection;
	return output;
#			endif
}

float3 GetSunColor(float3 normal, float3 viewDirection, float3 worldPosition, float roughness = 0.0)
{
#			if defined(UNDERWATER)
	return 0.0.xxx;
#			else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 0.0.xxx;

#				if defined(PBR_WATER)
	// Clamp what one pixel can reflect: a sub-pixel glint the AA kernel misses must not bloom across the screen.
	float reflectionMul = min(PBRWater::SpecularGGX(normal, -viewDirection, SunDir.xyz, roughness, 1.0 / PBRWater::WaterIOR, PBRWater::SunAngularRadius), 12.0) * PBRWater::Light0.w;
#				else
	float3 reflectionDirection = reflect(viewDirection, normal);
#					if defined(FLUID_FIELD)
	// The glint spreads on rough simulated water and stays tight on calm water
	float reflectionMul = FluidField::SunGlint(saturate(dot(reflectionDirection, SunDir.xyz)), VarAmounts.x);
#					else
	float reflectionMul = VarAmounts.x > 0 ? exp2(VarAmounts.x * log2(saturate(dot(reflectionDirection, SunDir.xyz)))) : 1.0;
#					endif
#				endif

	float3 sunColor = Color::DirectionalLight(ColorManagement::SRGBToWorking(SunColor.xyz)) * SunDir.w * (1.0 - exp(-DeepColor.w));
#				if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		sunColor *= ExponentialHeightFog::GetSunlightFogAttenuation(worldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
	}
#				endif
	return reflectionMul * sunColor;
#			endif
}
#		endif

#		if defined(LIGHT_LIMIT_FIX)
#			include "LightLimitFix/LightLimitFix.hlsli"
#		endif

#		if defined(IBL)
#			include "IBL/IBL.hlsli"
#		endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#		if defined(PBR_WATER)
	// Dry: the bed rises above the waves and the swash, where DisplaceSurface held the mesh just above it. Both passes
	// test the same pixels; the colour pass then finds the exact waterline against the ground behind it.
#			if defined(STENCIL)
	if (PBRWater::AboveWaterline(input.WorldPosition.xyz, input.ShoreState) && (uint)PBRWater::Draw0.z != PBRWater::DebugEdgeTest)
		discard;
#			else
	if (PBRWater::AboveWaterline(input.WPosition.xyz, input.ShoreState) && (uint)PBRWater::Draw0.z != PBRWater::DebugEdgeTest)
		discard;
#			endif
#		endif

	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

#		if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
	float3 viewDirection = normalize(input.WPosition.xyz);

#			if defined(FLUID_FIELD_FALLS)
	// D159: only the front surface shows (D125, D126), not an inner side over another sheet (D100, D103)
	const float fallDepth = SharedData::GetScreenDepth(input.HPosition.z);
	if (FallHasFront && FallShading::BehindFront(fallDepth, SharedData::GetScreenDepth(FallFrontDepth.Load(int3(input.HPosition.xy, 0)).x)))
		discard;
	if (FallShading::HiddenInnerSide(input.FallOutward, -viewDirection))
		discard;
	float3 fallGeometric = FallShading::SafeNormalize(input.FallNormal, -viewDirection);
	if (dot(fallGeometric, viewDirection) > 0.0)
		fallGeometric = -fallGeometric;  // D120: turned to the camera (the meshes' winding is not consistent)
	FallUp = fallGeometric;
	FallPlane = float4(fallGeometric, -dot(fallGeometric, input.WPosition.xyz));

	// The water type's normal maps run on the sheet's flow coordinates (a whole number of repeats along the scroll, D130)
	const FallGround::FlowSample fallFlow = FallGround::Flow(input.FallTexCoord / max(FallGround::InverseScale, 1e-4) + FallGround::TexOffset, input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz);
	// D188: across widened by the stretch so the texture keeps its proportions on fast water (along would shear)
	const float2 fallEvenUv = float2(fallFlow.uvA.x / clamp(fallFlow.stretch, FallShading::FLOW_STRETCH_RANGE.x, FallShading::FLOW_STRETCH_RANGE.y), fallFlow.uvA.y);
	input.TexCoord1.xy += fallEvenUv * FallShading::FlowScale(input.NormalsScale.x);
	input.TexCoord1.zw += fallEvenUv * FallShading::FlowScale(input.NormalsScale.y);
	input.TexCoord2.xy += fallEvenUv * FallShading::FlowScale(input.NormalsScale.z);

	// The falls' white water and fades (FallShading). D189: aeration from the landings
	const float fallPlungeAir = saturate(fallFlow.plunge.x * FallShading::PLUNGE_AERATION_IMPACT + fallFlow.plunge.y * FallShading::PLUNGE_AERATION_CARRIED);
	// D189: streaks down the fall; a whole number of foam tiles along per scroll cycle, or they jump as the phase wraps
	const float fallStreakTiles = FallShading::FlowScale(FallShading::FOAM_TILE).y;
	const float fallStreakAlong = max(round(FallShading::MILK_STREAK_ALONG * fallStreakTiles), 1.0) / fallStreakTiles;
	const float fallStreaks = FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFlow.uvA * float2(FallShading::MILK_STREAK_ACROSS, fallStreakAlong) + 0.71, SharedData::MipBias, fallFlow.stretch);
	// No air on the crest: white water and aeration only once the water has fallen
	const float fallCrestClear = smoothstep(0.0, max(FallGround::AerationLook.x, 1.0), fallFlow.plunge.z);
	// The crest is the river plane's own until it begins to fall
	FallFalling = smoothstep(0.0, FallShading::CREST_RIVER_FALL, fallFlow.plunge.z);
	// The bake's aeration turned to foam (the bake says where, the settings how much); only near-whole aeration whitens the surface
	const float fallBakeAir = smoothstep(max(FallGround::AerationLook.w, FallShading::SURFACE_WHITE_FROM), 1.0, fallFlow.fields.w) * fallCrestClear;
	// The white water's ribbons widen with the bake's aeration (from the threshold, scaled by Aeration Foam); nearer the foot and
	// where the water is stirred they break up in a churn
	float2 fallRibbons;
	float fallDropped;
	float fallStreakTone;   // a streak's own brightness (its column's, while it is still a streak)
	float fallStreakOn;     // its column's on/off timer (the foam's lumps come with it)
	float fallStreakStart;  // the height fallen where its column's streak begins (its bubbles gather there)
	{
		fallDropped = smoothstep(FallGround::AerationLook.x, FallGround::AerationLook.x + FallShading::BAKED_DROP_FULL, fallFlow.plunge.z);
		// the pockets between merged ribbons curve: their pattern warped across the flow by a slow noise (whole cells along, so it
		// repeats with the scroll), in rounder cells; where the ribbons are still thin streaks (low reach) it stays straight
		const float reachNow = min(saturate(fallBakeAir * fallBakeAir * FallGround::AerationLook.y), FallShading::RIBBON_MOST_REACH);
		const float curve = smoothstep(FallShading::POCKET_CURVE_FROM, FallShading::RIBBON_MOST_REACH, reachNow);
		const float warp = (FallShading::RibbonNoise(float2(fallFlow.uvA.x * FallShading::POCKET_WARP_CELLS.x, fallFlow.uvA.y * FallShading::POCKET_WARP_CELLS.y), FallShading::POCKET_WARP_CELLS.y) - 0.5) * 2.0;
		const float2 pocketUv = fallFlow.uvA + float2(warp * FallShading::POCKET_WARP * curve, 0.0);
		// Each noise at its own fixed cells, their results blended (blending the cells warps the noise into rings); both morph in place
		const float churn = lerp(FallShading::Morphing(fallFlow.uvA, FallShading::CHURN_FREQUENCY, FallGround::Time), FallShading::Morphing(pocketUv, FallShading::POCKET_FREQUENCY, FallGround::Time + 7.3), curve);
		// where the water is churned (the bake's stirring: behind rock, where its strands scatter) the ribbons hand over to the foam's
		// pattern instead of breaking into speckle
		const float lower = saturate(max(smoothstep(FallShading::CHURN_FROM, FallShading::CHURN_FULL, fallFlow.plunge.z), curve));
		// pockets close before the water reaches the pool (their gaps morph shut over POCKET_CLOSE above it), never open at the front edge
		const float poolAbove = FallShading::AboveWater(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, input.FallFade.z, input.FallFade.y, input.FallData.y);
		const float closing = curve * (1.0 - smoothstep(FallShading::POCKET_CLOSE.x, FallShading::POCKET_CLOSE.y, poolAbove)) * (1.0 - fallFlow.streak);
		// The streaks from the drop: straight, morphing in place, each fading in and out on its own timer (STREAK_LIFE)
		const float streakLife = FallShading::RibbonNoise(float2(fallFlow.column * FallShading::STREAK_FREQUENCY.x, FallGround::Time / FallShading::STREAK_LIFE), 1e4);
		const float2 streakUv = float2(fallFlow.column, fallFlow.uvA.y);  // the streaks' own columns: straight down, parted by rock
		// on or off over a short window (STREAK_SWITCH): fully on it is bright and crisp, off it is gone
		const float streakOn = smoothstep(1.0 - FallShading::STREAK_ON_SHARE - FallShading::STREAK_SWITCH, 1.0 - FallShading::STREAK_ON_SHARE + FallShading::STREAK_SWITCH, streakLife);
		fallStreakOn = streakOn;
		fallStreakTone = lerp(1.0, lerp(FallShading::STREAK_TONE.x, FallShading::STREAK_TONE.y, FallShading::RibbonNoise(float2(fallFlow.column * FallShading::STREAK_FREQUENCY.x, 0.37), 1e4)) * lerp(FallShading::STREAK_FADING_TONE, 1.0, streakOn), 1.0 - curve);
		const float streaks = lerp(FallShading::MorphingColumns(streakUv, FallShading::STREAK_FREQUENCY, FallGround::Time + 3.7), fallStreaks, FallShading::RIBBON_STREAK_DETAIL) *
		                      lerp(FallShading::STREAK_FADE_LOW, FallShading::STREAK_FADE_HIGH, streakOn);
		// the streaks stay wherever the bake keeps them alive: no churn or pockets take over there
		const float field = lerp(lerp(streaks, churn, lower * FallShading::CHURN_SHARE * (1.0 - fallFlow.streak)), 1.0, closing);
		// The reach rides a noise on the water (whole cells along), so the tongues' ends run down and re-form
		const float reachRide = lerp(FallShading::REACH_RIDE.x, FallShading::REACH_RIDE.y, FallShading::RibbonNoise(float2(fallFlow.column * FallShading::REACH_RIDE_CELLS.x, fallFlow.uvA.y * FallShading::REACH_RIDE_CELLS.y), FallShading::REACH_RIDE_CELLS.y));
		// Thin streaks before the aeration has built, widening into one another as it builds; each starts at its own height and
		// widens from a point (a V, not a flat cut top)
		const float streakStart = FallGround::AerationLook.x + FallShading::RibbonNoise(float2(fallFlow.column * FallShading::STREAK_FREQUENCY.x, 5.1), 1e4) * FallShading::STREAK_START_SPREAD;
		fallStreakStart = streakStart;
		const float streakReach = lerp(FallShading::STREAK_TOP_REACH, FallShading::RIBBON_MOST_REACH, smoothstep(FallShading::STREAK_MERGE.x, FallShading::STREAK_MERGE.y, fallFlow.fields.w)) *
		                          saturate((fallFlow.plunge.z - streakStart) / FallShading::STREAK_RISE) * fallFlow.streak;  // None once it has met rock
		// Squared: half the curtain bakes above 0.7. Capped at RIBBON_MOST_REACH so even wholly aerated water keeps gaps
		const float reach = min(max(saturate(fallBakeAir * fallBakeAir * FallGround::AerationLook.y * reachRide), streakReach), FallShading::RIBBON_MOST_REACH);
		// Soft, ragged edges: the threshold broken by the fine streaks
		fallRibbons = FallShading::RibbonsEdge(field + (fallStreaks - 0.5) * FallShading::EDGE_BREAK * lerp(1.0, FallShading::STREAK_CRISP, streakOn), reach,
			FallShading::RIBBON_EDGE_SOFT * lerp(1.0, FallShading::STREAK_CRISP, streakOn));
		// Frayed edges: only a ribbon's inside is solid, toward its edge it thins into fibres running with the water
		const float fibre = FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFlow.uvA * float2(FallShading::STREAK_FIBRE_ACROSS, fallStreakAlong * 2.0) + 0.29, SharedData::MipBias, fallFlow.stretch);
		// And bubbly: round holes from the foam's own pattern, many at the edge, a few inside
		const float bubbles = smoothstep(FallShading::STREAK_BUBBLES.x, FallShading::STREAK_BUBBLES.y,
			FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFlow.uvA * FallShading::STREAK_BUBBLE_SCALE + 0.53, SharedData::MipBias, fallFlow.stretch));
		const float solid = smoothstep(FallShading::STREAK_SOLID.x, FallShading::STREAK_SOLID.y, fallRibbons.x);
		fallRibbons.x *= lerp(smoothstep(FallShading::STREAK_FIBRE.x, FallShading::STREAK_FIBRE.y, fibre) * bubbles, lerp(1.0, bubbles, FallShading::STREAK_BUBBLES_INSIDE), solid);
	}
	const float fallWhite = fallRibbons.x * fallCrestClear;
	fallDropped = max(fallDropped, fallWhite);
	// D192: rapids: undropped fast or stirred water gets the dropped water's texture and air, not its colour
	const float fallChurn = max(fallDropped, FallShading::Rapids(fallFlow.fields, fallFlow.plunge.w, 1.0 - abs(FallUp.z)));
	// D191: the dropped water's milk, aeration and bubbles kept between ribbons (little) and in their haze (all)
	const float fallGap = lerp(1.0, lerp(FallShading::RIBBON_GAP, 1.0, fallRibbons.y), fallDropped);
	// D190: the run's aeration grows with the drop (the crest stays the river's)
	const float fallAerated = max(max(max(smoothstep(0.0, 1.0, fallFlow.fields.w) * 0.7 * fallDropped, FallShading::Aerated(fallFlow.runTime) * 0.3 * fallChurn) * fallGap,
									  smoothstep(FallShading::STIRRED_FROM, FallShading::STIRRED_FULL, fallFlow.fields.z) * FallShading::STIRRED_AERATION),
								  max(max(fallPlungeAir, saturate(fallFlow.foam) * FallShading::BAKE_FOAM_AERATION), fallWhite * FallShading::PLUNGE_WHITE_AERATION)) *
	                          fallCrestClear;
	// D186: eased inward: faint at a patch's rim, whole well inside it
	const float fallAeratedLook = fallAerated * smoothstep(FallShading::AERATION_EASE_FROM, FallShading::AERATION_EASE_FULL, fallAerated);
	// D189: rough where fast and aerated (its reflected rays spread)
	FallRoughness = saturate(fallFlow.fields.x * 300.0 / 600.0) * FallShading::SSR_ROUGH_SPEED * fallChurn + fallAeratedLook * FallShading::SSR_ROUGH_AERATION;
	// The sheet's axes: along its flow (D186: the bake's frame) and across it
	const float3 fallWay = dot(fallFlow.frame, fallFlow.frame) > 0.01 ? fallFlow.frame : input.FallTangent;
	const float3 fallAlong = FallShading::SafeNormalize(fallWay - FallUp * dot(fallWay, FallUp), float3(0.0, 0.0, -1.0));
	const float3 fallAcross = cross(fallAlong, FallUp);
	// The view across the sheet in flow coordinates per unit of depth (the bubbles' parallax), held back at grazing angles
	float2 fallViewShift;
	{
		const float3 toEye = -viewDirection;
		const float facing = max(dot(toEye, FallUp), 0.35);
		fallViewShift = -float2(dot(toEye, fallAcross), dot(toEye, fallAlong)) / facing / FallShading::FALLS_UV_WORLD;
	}
	// Screen pixels per world unit (the bubbles' size on screen)
	const float fallPixelsPerUnit = 1.0 / max(length(fwidth(input.WPosition.xyz)) * 0.7071, 1e-4);
	// Bubbles from the baked aeration, gathering most where the streaks begin (ramping up to each streak's start, easing off after it)
	const float fallToStart = fallFlow.plunge.z - fallStreakStart;
	const float fallStartBubbles = smoothstep(-FallShading::BUBBLE_START_LEAD, 0.0, fallToStart) *
	                               lerp(FallShading::BUBBLE_AFTER_START, 1.0, exp(-max(fallToStart, 0.0) / FallShading::BUBBLE_START_TAIL)) *
	                               max(FallShading::Lip(input.FallData.y, fallFlow.runTime), fallDropped);  // (fallLip, defined later)
	const float fallUnifiedBubbles = max(fallStartBubbles, smoothstep(FallShading::UNIFIED_BUBBLES.x, FallShading::UNIFIED_BUBBLES.y, fallFlow.fields.w) * FallShading::BUBBLE_AFTER_START) *
	                                 FallShading::UNIFIED_BUBBLE_AMOUNT;
	const float4 fallBubbleSample = FallShading::Bubbles(fallFlow.uvA, fallViewShift, fallUnifiedBubbles, fallPixelsPerUnit, fallUnifiedBubbles * FallShading::UNIFIED_BUBBLE_DENSITY,
		FallShading::UNIFIED_BUBBLE_CELL, FallShading::UNIFIED_BUBBLE_FAR);
	const float fallBubbles = fallBubbleSample.x;
	// D191: dropped water is never a quiet crest, whatever pool it runs out of
	const float fallLip = max(FallShading::Lip(input.FallData.y, fallFlow.runTime), fallDropped);
	// down the drop in two steps, each column at its own height and pace (one height for all read as a band painted on): clear to
	// aerated (milk), then aerated to foam
	// (only water pouring out of a river or pool: a sheet fed by the one above arrives aerated)
	const float fallCrestPace = lerp(0.7, 1.3, FallShading::RibbonNoise(float2(fallFlow.column * FallShading::STREAK_FREQUENCY.x, 8.3), 1e4));
	const float fallCrestStart = lerp(FallGround::AerationLook.x, fallStreakStart, 0.5);
	const bool fallFromSource = input.FallData.y < FallShading::NONE_ABOVE;
	const float fallMilkLip = fallLip * (fallFromSource ? smoothstep(fallCrestStart * 0.3, fallCrestStart + FallShading::CREST_MILK_IN * fallCrestPace, fallFlow.plunge.z) : 1.0);
	const float fallFoamLip = fallLip * (fallFromSource ? smoothstep(fallCrestStart + FallShading::CREST_FOAM_AFTER * fallCrestPace,
															  fallCrestStart + (FallShading::CREST_FOAM_AFTER + FallShading::CREST_FOAM_IN) * fallCrestPace, fallFlow.plunge.z) :
														  1.0);
	const float fallBehind = max(SharedData::GetScreenDepth(DepthTex.Load(int3(input.HPosition.xy, 0)).x) - fallDepth, 0.0);
	const float fallAboveWater = FallShading::AboveWater(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz, input.FallFade.z, input.FallFade.y, input.FallData.y);
	// the foam's holes drawn out along the flow as its grain is; narrowed across, not slowed along, so they keep the water's pace (round holes read as spots)
	const float2 fallHoleUv = fallFlow.uvA * float2(FallShading::FOAM_STREAK_LENGTH, 1.0);  // (narrowed across: slowing the along slid them against the water)
	const float fallFoamStretch = fallFlow.stretch;                                         // D182: no shear (FlowFoamPattern wraps the coordinate)
	// D189: the bake's own foam (strikes, squeezes, rejoins) and the ribbons'
	const float fallBakeFoam = saturate(fallFlow.foam * FallShading::BAKE_FOAM) * fallFoamLip;
	const float fallFoamSource = max(fallBakeFoam, fallWhite * FallShading::PLUNGE_WHITE_FOAM * fallFoamLip);
	// The foam in mounds: billows boiling in place (their slope for its relief), finer lumps rounding its edges
	float fallBillow, fallLumpEdge;
	float2 fallBillowSlope;
	{
		const float boil = FallGround::Time * FallShading::BILLOW_BOIL;
		const float raw = FallShading::Morphing(fallFlow.uvA, FallShading::BILLOW_CELLS, boil);
		const float2 step = 0.25 / FallShading::BILLOW_CELLS;
		fallBillowSlope = float2(FallShading::Morphing(fallFlow.uvA + float2(step.x, 0.0), FallShading::BILLOW_CELLS, boil) - raw,
							  FallShading::Morphing(fallFlow.uvA + float2(0.0, step.y), FallShading::BILLOW_CELLS, boil) - raw) /
		                  step;
		fallBillow = smoothstep(0.2, 0.8, raw);  // rounded mounds, flat-bottomed hollows
		// the lumps: only well below where the foam starts (by the aeration, which only grows down the sheet), coming and going with
		// the streaks' own intervals (their column's on/off timer)
		const float lumps = smoothstep(FallShading::LUMP_FROM.x, FallShading::LUMP_FROM.y, fallFlow.fields.w) * lerp(FallShading::LUMP_STREAK_OFF, 1.0, fallStreakOn);
		const float fine = lerp(1.0 - FallShading::LUMP_EDGE, 1.0 + 0.5 * FallShading::LUMP_EDGE, FallShading::Morphing(fallHoleUv, FallShading::LUMP_EDGE_CELLS, boil * FallShading::LUMP_EDGE_RATE + 3.1));
		// and bigger ones over them (the lumps from fine up to LUMP_BIG_CELLS in size)
		const float big = lerp(1.0 - FallShading::LUMP_BIG, 1.0 + 0.4 * FallShading::LUMP_BIG, FallShading::Morphing(fallHoleUv, FallShading::LUMP_BIG_CELLS, boil + 8.3));
		fallLumpEdge = lerp(1.0, fine * big, lumps);
	}
	// The foam's breakup, shared by its kinds
	const float fallBreakup = FallShading::FoamBreakup(FallFoamTex, Normals01Sampler, fallFlow.uvA, SharedData::MipBias, 1.0 / FallShading::FOAM_STREAK_LENGTH);
	const float fallFoamDrive = fallFoamSource * fallLumpEdge * lerp(fallBreakup, 1.0, smoothstep(0.45, 0.85, fallFoamSource));
	// D190: read where the view meets the raised lumps (parallax)
	const float2 fallFoamUv = FallShading::FoamParallax(FallFoamTex, Normals01Sampler, fallFlow.uvA, fallViewShift, SharedData::MipBias, fallFoamStretch);
	// All the foam on one grain, churning in place (two layouts crossfaded), on the flow coordinates, which already bend round rock
	float fallFoamPattern;
	{
		float2 jumpA, jumpB;
		const float churnWeight = FallShading::FoamChurn(FallGround::Time, jumpA, jumpB);
		fallFoamPattern = FallShading::ChurnBlend(FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFoamUv + jumpB, SharedData::MipBias, fallFoamStretch),
			FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFoamUv + jumpA, SharedData::MipBias, fallFoamStretch), churnWeight);
	}
	// D189: the foam's relief: a height, its normal and the height toward the sun (self-shadowing)
	const float3 fallFoamNormal = FallShading::FoamReliefNormal(FallUp, input.WPosition.xyz, fallFoamPattern);
	const float2 fallToSun = float2(dot(SharedData::DirLightDirection.xyz, fallAcross), dot(SharedData::DirLightDirection.xyz, fallAlong)) / FallShading::FALLS_UV_WORLD;
	const float fallFoamSunHeight = FallShading::FlowFoamPattern(FallFoamTex, Normals01Sampler, fallFoamUv + fallToSun * FallShading::FOAM_SHADOW_REACH, SharedData::MipBias, fallFoamStretch);
	// Aeration as layers under the surface (the deeper dimmer), straight from the baked aeration: gapped by brief clear patches opening
	// and shutting in place, clumped by a slow noise, its grain churning (not sliding)
	float3 fallAerationVolume;
	float fallUnifiedAerationDrive;  // the aeration foam's drive, joined with the streaks' below
	float fallAerationGap;           // its brief clear patches (their milk cut too)
	{
		// the most aerated water nearly covered: driven past whole, its gaps, clumps and breakup easing off
		const float heavy = smoothstep(FallShading::AERATION_HEAVY.x, FallShading::AERATION_HEAVY.y, fallFlow.fields.w);
		const float gap = FallShading::AerationGaps(fallHoleUv, FallGround::Time) * (1.0 - 0.7 * heavy);
		fallAerationGap = gap;
		const float clump = lerp(lerp(1.0 - FallShading::AERATION_CLUMP, 1.0, FallShading::Morphing(fallHoleUv, FallShading::AERATION_CLUMP_CELLS, FallGround::Time + 5.1)), 1.0, heavy);
		// in the gaps between the dropped water's ribbons it thins (its own gaps fell elsewhere, and the two together covered it all)
		const float betweenRibbons = lerp(1.0, FallShading::AERATION_BETWEEN_RIBBONS, (1.0 - smoothstep(0.0, 0.5, fallRibbons.x)) * fallDropped * (1.0 - heavy) * (1.0 - smoothstep(FallShading::LOBE_FLAT.x, FallShading::LOBE_FLAT.y, abs(FallUp.z))));  // (curtains only: lobes have no ribbons)
		const float drive = smoothstep(FallShading::UNIFIED_AERATION_DRIVE.x, FallShading::UNIFIED_AERATION_DRIVE.y, fallFlow.fields.w) * lerp(1.0, FallShading::AERATION_HEAVY_DRIVE, heavy) *
		                    fallFoamLip * (1.0 - gap) * clump * fallLumpEdge * lerp(fallBreakup, 1.0, heavy) * betweenRibbons;
		fallUnifiedAerationDrive = drive;
		float2 jumpA, jumpB;
		const float churnWeight = FallShading::FoamChurn(FallGround::Time, jumpA, jumpB);
		fallAerationVolume = lerp(
			FallShading::AerationVolume(FallFoamTex, Normals01Sampler, fallFlow.uvA + jumpB, fallViewShift, drive, SharedData::MipBias, fallFoamStretch, FallShading::UNIFIED_AERATION_LAYERS, 1.0, FallShading::AERATION_LAYER_OPACITY),
			FallShading::AerationVolume(FallFoamTex, Normals01Sampler, fallFlow.uvA + jumpA, fallViewShift, drive, SharedData::MipBias, fallFoamStretch, FallShading::UNIFIED_AERATION_LAYERS, 1.0, FallShading::AERATION_LAYER_OPACITY),
			churnWeight);
	}
	// The streaks' foam clumped, then joined with the aeration's into one foam (reinforcing where both are): one cover cut from them on
	// the one grain, so they run into each other rather than lying one over the other
	const float fallStreakClump = lerp(1.0 - FallShading::STREAK_CLUMP, 1.0, FallShading::Morphing(fallHoleUv, FallShading::AERATION_CLUMP_CELLS, FallGround::Time + 2.3));
	float fallFoam;
	float fallSurfaceFoam;
	float fallFoamForming;  // the foam's joined drive: no milk wherever foam forms (its openings stay clear)
	{
		const float streakDrive = fallFoamDrive * FallShading::STREAK_FOAM_DRIVE * fallStreakClump;
		// and its contact foam round rock, land and the pool: baked distance to the contact, reaching out down the flow by a noise riding
		// it (never inside the baseline: the swept distance is never more than the contact's), eaten in from its outer edge
		const float reachOut = FallShading::Morphing(fallFlow.uvA, FallShading::CONTACT_REACH_CELLS, FallGround::Time * FallShading::CONTACT_REACH_RATE + 12.9);
		// its head (the ring at the contact) only a thin line; its tails (where the swept distance runs well inside the contact's: downstream
		// of it) the wide band, reaching out with the noise
		const float tailDistance = lerp(fallFlow.contact.x, min(fallFlow.contact.x, fallFlow.contact.y), lerp(FallShading::CONTACT_TAIL_LEAST, 1.0, reachOut));
		const float downstream = smoothstep(0.0, FallShading::CONTACT_TAIL_FROM, fallFlow.contact.x - tailDistance);
		const float eaten = FallShading::RibbonNoise(fallFlow.uvA * FallShading::TURB_BREAK_CELLS_CONTACT + float2(0.0, FallGround::Time * 0.4), FallShading::TURB_BREAK_CELLS_CONTACT.y) * FallShading::CONTACT_EDGE_NOISE;
		const float tail = (1.0 - smoothstep(FallShading::CONTACT_BAND.x, max(FallShading::CONTACT_BAND.y - eaten, FallShading::CONTACT_BAND.x + 1.0), tailDistance)) * downstream;
		const float head = 1.0 - smoothstep(FallShading::CONTACT_HEAD.x, FallShading::CONTACT_HEAD.y, fallFlow.contact.x);
		const float band = max(head, tail);
		const float solid = 1.0 - smoothstep(0.0, FallShading::CONTACT_SOLID, tailDistance);
		const float contactBreakup = fallBreakup * (1.0 - fallAerationGap);
		const float contactFoam = band * lerp(contactBreakup, 1.0, solid * downstream) * FallShading::CONTACT_FOAM_DRIVE * fallLumpEdge * fallLip;
		const float other = max(fallUnifiedAerationDrive, contactFoam);
		const float joined = max(streakDrive, other) + FallShading::FOAM_JOIN * min(streakDrive, other);
		fallFoamForming = joined;
		fallFoam = joined > 0.02 ? saturate(FallShading::AerationCover(joined, fallFoamPattern) * FallShading::STREAK_OPACITY / FallShading::FOAM_OPACITY) : 0.0;
		fallSurfaceFoam = fallFoam;
	}
	// D189: surface foam fully lit, the volume dimmer with depth
	const float fallSurfaceShare = saturate(fallSurfaceFoam / max(fallFoam, 1e-4));
	const float fallFoamShade = lerp(fallAerationVolume.y, 1.0, fallSurfaceShare);
	// D176: under the water below only its bubbles show, a plume seen through the water (the plane is flat, so the height interpolates
	// exactly): soft and fading with depth, never foam laid on the surface
	const float fallUnderVis = exp(-max(-fallAboveWater, 0.0) / FallShading::UNDER_SCATTER);
	const float fallAlpha = FallShading::Alpha(input.FallFade.x, fallBehind, fallFoam, input.FallClip) *
	                        (fallAboveWater < 0.0 ? lerp(fallFoam, saturate(fallFlow.fields.w), FallShading::UNDER_SOFT) * fallUnderVis * FallShading::UNDER_MOST : 1.0);
	if (fallAlpha <= 0.002)
		discard;
	const float3 waterUp = FallUp;
#			else
	const float3 waterUp = float3(0, 0, 1);
#			endif

	float distanceFactor = saturate(lerp(FrameBuffer::FrameParams.w, 1, (length(input.WPosition.xyz) - 8192) / (WaterParams.x - 8192)));
	float4 distanceMul = saturate(lerp(VarAmounts.z, 1, -(distanceFactor - 1))).xxxx;
	float distanceBlendFactor = distanceFactor;
#			if defined(UNIFIED_WATER)
	distanceBlendFactor = 1.0f;
#			endif

	bool isSpecular = false;

	float depth = 0;

#			if defined(DEPTH)
#				if defined(VERTEX_ALPHA_DEPTH)
#					if defined(VC)
	distanceMul = saturate(input.TexCoord3.z);
#					endif
#				else
	distanceMul = 0;

	depth = GetScreenDepthWater(screenPosition);
	float2 depthOffset =
		FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;
	float depthMul = length(float3((depthOffset * 2 - 1) * depth / ProjData.xy, depth));
	float3 depthAdjustedViewDirection = -viewDirection * depthMul;
	float viewSurfaceAngle = dot(depthAdjustedViewDirection, GetWaterPlane().xyz);

	float planeMul = (1 - GetWaterPlane().w / viewSurfaceAngle);
	distanceMul = saturate(
		planeMul * float4(length(depthAdjustedViewDirection).xx, abs(viewSurfaceAngle).xx) /
		FogParam.z);

#					if defined(HORIZON_FIX)
	if (DepthTex.Load(float3(screenPosition, 0)).x >= HorizonFix::EmptyDepthThreshold)
		distanceMul = 1.0.xxxx;
#					endif
#				endif
#			endif

#			if defined(UNDERWATER)
	float4 depthControl = float4(0, 1, 1, 0);
#			elif defined(LOD)
	float4 depthControl = float4(1, 0, 0, 1);
#			elif defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float4 depthControl = float4(0, 0, 1, 0);
#			else
	float4 depthControl = DepthControl * (distanceMul - 1) + 1;
#			endif
	float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);

#			if defined(SKYLIGHTING)
	float wetnessOcclusion = 1.0;

	float3 positionMSSkylight = input.WPosition.xyz;

#				if defined(PBR_WATER)
	// The waves are in Skylighting's occlusion maps (PBR Water): read the sky just above this point of the surface,
	// where a trough sees less of it past the crests around than a crest does.
	positionMSSkylight.z += Skylighting::CELL_SIZE.z;
#				endif
	// The water surface itself is never lifted out of the water (Skylighting::SampleNoBias).
	sh2 skylightingSH = Skylighting::SampleNoBias(positionMSSkylight, false);
	float skylighting = SphericalHarmonics::Unproject(skylightingSH, waterUp);

	float skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, waterUp, Skylighting::GetFadeOutFactor(input.WPosition.xyz));

	wetnessOcclusion = inWorld ? pow(saturate(skylighting), 2) : 0;
#			endif

#			if defined(SKYLIGHTING)
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, wetnessOcclusion);
#			else
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, inWorld);
#			endif

	float3 normal = waterData.normal;
#			if defined(FLUID_FIELD_FALLS)
	{
		// The falls' own normal map alone, without the river's
		if (FallHasNormal)
			normal = float3(0.0, 0.0, 1.0);
		// D159: normals in the sheet's frame (along its flow, over its normal); the piece's own normal map runs on the flow too (D127)
		[branch] if (FallHasNormal)
		{
			// D188: stretch evened out, a sharper mip and a finer layer near the camera
			float3 own = FallShading::FlowNormalEven(FallNormalTex, Normals01Sampler, fallFlow.uvA, fallFlow.stretch, 1.0, SharedData::MipBias + FallShading::FLOW_NORMAL_MIP_BIAS);
			const float detailNear = 1.0 - smoothstep(FallShading::FLOW_DETAIL_NEAR, FallShading::FLOW_DETAIL_FAR, length(input.WPosition.xyz));
			[branch] if (detailNear > 0.0)
			{
				const float3 fine = FallShading::FlowNormalEven(FallNormalTex, Normals01Sampler, fallFlow.uvA + float2(0.17, 0.31), fallFlow.stretch, FallShading::FLOW_DETAIL_SCALE,
					SharedData::MipBias + FallShading::FLOW_NORMAL_MIP_BIAS);
				own.xy += fine.xy * FallShading::FLOW_DETAIL_STRENGTH * detailNear;
			}
			own.xy *= FallShading::NORMAL_STRENGTH * lerp(FallShading::DROP_CREST_NORMAL, 1.0, fallChurn);  // D189, D192: the river's look at the crest, not on rapids
			normal = FallShading::SafeNormalize(float3(normal.xy + own.xy, normal.z * max(own.z, 0.1)), normal);
		}
		// D186: on the bake's flow frame; the mesh's tangent only where the bake has none
		const float3 fallFrame = dot(fallFlow.frame, fallFlow.frame) > 0.01 ? fallFlow.frame : input.FallTangent;
		normal = FallShading::SheetNormal(FallUp, fallFrame, normal);  // D174: not PerturbNormal's per-triangle frame
		// D188: tilted by the wobble's slope, so the light follows the moving vertices
		normal = FallShading::WobbleNormal(normal, FallUp, fallFrame, input.FallWobble, input.FallWobbleSizes, fallFlow.stretch);
	}
#			endif

#			if defined(PBR_WATER)
	PBRWater::SurfaceShading pbrSurface = PBRWater::ShadeSurface(input.WaveParam, input.FlowVelocity, input.ShoreState, input.SurfaceNoise.y, normal, waterData.detailVariance);
	normal = pbrSurface.normal;
	// A curling lip can turn its far side to the camera (seen from behind the wave, or where the lip passes through the
	// water behind it). Shaded about a normal facing away, its reflection, sun and foam lighting turned odd along the crest:
	// mirrored across the view direction, the normal keeps its lean along the surface and faces the side that is seen.
	float pbrFacing = dot(normal, viewDirection);
	if (pbrFacing > 0.0)
		normal = normalize(normal - 2.0 * pbrFacing * viewDirection);
	pbrSurface.normal = normal;  // the normals debug view shows what is shaded
	float pbrRoughness = pbrSurface.roughness;
#			else
	float pbrRoughness = 0.0;
#			endif

#			if defined(SKYLIGHTING)
#				if defined(PBR_WATER)
	// The sky each wave facet sees: tilted towards a cliff or a hull it sees less of it.
	skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, normal, Skylighting::GetFadeOutFactor(input.WPosition.xyz));
#				endif
	sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normal, -viewDirection, 0.0);
	float skylightingSpecular = Skylighting::EvaluateSpecular(skylightingSH, specularLobe, Skylighting::GetFadeOutFactor(input.WPosition.xyz));
#			endif

	float fresnel = GetFresnelValue(normal, viewDirection);
#			if defined(FLUID_FIELD_FALLS)
	// D190: white water is matte
	fresnel *= 1.0 - fallWhite * FallShading::WHITE_REFLECTION_LOSS;
#			endif
#			if defined(PBR_WATER) && !defined(UNDERWATER)
	{
		// Roughness-aware environment Fresnel for F0 = 0.02 (air -> water).
		float2 envBRDF = BRDF::EnvBRDF(pbrRoughness, saturate(dot(-viewDirection, normal)));
		float pbrFresnel = saturate(0.02 * envBRDF.x + envBRDF.y);
		fresnel = lerp(pbrFresnel, fresnel, PBRWater::Light0.z);
	}
#			endif

#			if defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float3 finalColor = 0.0.xxx;

#				if !defined(LIGHT_LIMIT_FIX)
	[unroll] for (int lightIndex = 0; lightIndex < NUM_SPECULAR_LIGHTS; ++lightIndex)
	{
		float3 lightVector = LightPos[lightIndex].xyz - (PosAdjust.xyz + input.WPosition.xyz);
		float3 lightDirection = normalize(normalize(lightVector) - viewDirection);
		float lightFade = saturate(length(lightVector) / LightPos[lightIndex].w);
		float lightColorMul = (1 - lightFade * lightFade);
#					if defined(ENABLE_LL)
		lightColorMul = pow(lightColorMul, TransferFunctions::GAME_GAMMA);
#					endif
#					if defined(PBR_WATER)
		float pbrLightDistance = length(lightVector);
		float3 lightColor = Color::PointLight(LightColor[lightIndex].xyz) * lightColorMul *
		                    PBRWater::SpecularGGX(normal, -viewDirection, PBRWater::SafeNormalize(lightVector, normal), pbrRoughness, 1.0 / PBRWater::WaterIOR,
								PBRWater::EmitterAngularRadius(PBRWater::PointLightEmitterRadius(), pbrLightDistance)) *
		                    PBRWater::Light1.w;
#					else
		float LdotN = saturate(dot(lightDirection, normal));
		float3 lightColor = (Color::PointLight(LightColor[lightIndex].xyz) * pow(LdotN, FresnelRI.z)) * lightColorMul;
#					endif
		finalColor += lightColor;
	}
#				endif

#				if !defined(PBR_WATER)
	finalColor *= fresnel;
#				endif
#				if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override specular color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorSpecular(waterData.rippleInfo, 2.5, 4.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#				endif

	isSpecular = true;
#			else

#				if defined(SKYLIGHTING)
	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceBlendFactor, skylightingSpecular, pbrRoughness);
#				else
	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceBlendFactor, 1.0, pbrRoughness);
#				endif

	DiffuseOutput diffuseOutput = GetWaterDiffuseColor(input, normal, viewDirection, distanceMul, depthControl.y, fresnel, viewPosition, depth);

	float surfaceShadow;
	float dirShadow = ShadowSampling::Get3DFilteredShadow(input.WPosition.xyz, diffuseOutput.refractedViewDirection, input.HPosition.xy, surfaceShadow);
#				if defined(FLUID_FIELD_FALLS)
	// D190: soften the sun's shadow on the falls: the volumetric cascades resolve cliff shadows only near, blotching the falls
	surfaceShadow = lerp(1.0, surfaceShadow, FallShading::SUN_SHADOW);
	dirShadow = lerp(1.0, dirShadow, FallShading::SUN_SHADOW);
#				endif
#				if defined(PBR_WATER) && !defined(UNDERWATER)
	// The waves' own shadows: the water the sunlight crosses to get here (the engine's shadow map holds no water).
	PBRWater::SunThroughWaves pbrWaveSun = PBRWater::SunPathThroughWaves(input.WPosition.xyz, SunDir.xyz, input.ShoreState.w, Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount));
#				endif

	float3 dirColor;
	float3 ambientColor;
	ShadowSampling::ExtractLighting(diffuseOutput.refractionDiffuseColor, dirColor, ambientColor);

	dirColor *= dirShadow;

#				if defined(SKYLIGHTING)
	ambientColor = ColorManagement::SceneColor::ScaleByLinear(ambientColor, skylightingDiffuse);
#				endif

	diffuseOutput.refractionDiffuseColor = dirColor + ambientColor;
#				if defined(FLUID_FIELD)
	// Fluid Field's foam colour: the water's own lit colour (dirColor + ambientColor) lifted toward the
	// scene light, so foam takes the water's hue without blowing out the exposure
	float3 fluidAmbientLight = ShadowSampling::GetAmbientLighting();
#					if defined(SKYLIGHTING)
	fluidAmbientLight = ColorManagement::SceneColor::ScaleByLinear(fluidAmbientLight, skylightingDiffuse);
#					endif
	// Foam lies on the surface, so it takes the surface's shadow, not the refracted point's under the
	// water: that one moves with every ripple and makes the foam flicker in sunlight
	const float3 fluidLight = ShadowSampling::GetDirectionalLighting() * surfaceShadow + fluidAmbientLight;
	const float fluidFoamFade = FluidField::FoamDaylight();
	float3 fluidFoamLight = FluidField::FoamTint(FluidField::FoamColor(dirColor + ambientColor, fluidLight), fluidLight);
#					if defined(FLUID_FIELD_FALLS)
	fluidFoamLight *= lerp(FallShading::FOAM_NIGHT_LIGHT, 1.0, FluidField::Daylight());
#					endif
	// The river's foam lies flat: the sun or moon lights it by its height, or a low moon over-lights it against the land
	const float3 fluidRiverFoamSun = ShadowSampling::GetDirectionalLighting() * surfaceShadow * saturate(SharedData::DirLightDirection.z);
	const float3 fluidRiverFoamLight = FluidField::FoamTint(FluidField::RiverFoamColor(dirColor + ambientColor, fluidRiverFoamSun + fluidAmbientLight), fluidLight);
#					if defined(FLUID_FIELD_FALLS)
	// D189: the surface foam lit over its relief
	float3 fallFoamLight = FluidField::FoamTint(fluidLight * FallShading::FOAM_BASE_ALBEDO, fluidLight) * lerp(FallShading::FOAM_NIGHT_LIGHT, 1.0, FluidField::Daylight()) * fallFoamShade *
	                       lerp(1.0, FallShading::FoamRelief(FallUp, fallFoamNormal, fallFoamPattern, fallFoamSunHeight, SharedData::DirLightDirection.xyz, Color::RGBToLuminance(ShadowSampling::GetDirectionalLighting() * surfaceShadow), Color::RGBToLuminance(fluidAmbientLight)),
							   fallSurfaceShare * lerp(1.0, FallShading::LOBE_RELIEF, smoothstep(FallShading::LOBE_FLAT.x, FallShading::LOBE_FLAT.y, abs(FallUp.z)) * smoothstep(FallShading::AERATION_HEAVY.x, FallShading::AERATION_HEAVY.y, fallFlow.fields.w)));
	// D189: the body takes at least some ambient light (by drop and steepness), or falls out of the sun go black
	diffuseOutput.refractionDiffuseColor = max(diffuseOutput.refractionDiffuseColor,
		fluidAmbientLight * FallShading::BODY_AMBIENT * max(fallDropped, saturate((1.0 - abs(FallUp.z)) * 1.5)));
	// D159: the falls' own aeration, not the river simulation's
	{
		// D186: milky as the river's shallows, from the falls' own aeration and speed; little at the crest
		const float milk = saturate(fallAeratedLook * FallShading::MILK_AERATION * fallDropped) *
		                   FallShading::MILK_MOST * lerp(1.0 - FallShading::MILK_STREAK, 1.0 + FallShading::MILK_STREAK, smoothstep(0.3, 0.7, fallStreaks)) * fallGap * FallGround::AerationLook.z;  // The falls' own cap, in streaks
		// D191: and the haze round the ribbons (the ribbons themselves are foam)
		const float white = saturate(milk + fallRibbons.y * FallShading::RIBBON_HAZE_MILK);
		diffuseOutput.refractionDiffuseColor = lerp(diffuseOutput.refractionDiffuseColor, fluidFoamLight, white);
		diffuseOutput.refractionColor = lerp(diffuseOutput.refractionColor, fluidFoamLight, white * 0.5);
		// Milky under the surface by the baked aeration, only where the foam is not, so the two never double up
		{
			const float aerMilk = FallShading::UnifiedMilk(fallFlow.fields.w, fallFoamForming, fallMilkLip);
			const float3 milkColor = lerp(fluidFoamLight, diffuseOutput.refractionDiffuseColor, FallShading::UNIFIED_MILK_HUE);
			diffuseOutput.refractionDiffuseColor = lerp(diffuseOutput.refractionDiffuseColor, milkColor, aerMilk * FallShading::UNIFIED_MILK);
			diffuseOutput.refractionColor = lerp(diffuseOutput.refractionColor, milkColor, aerMilk * FallShading::UNIFIED_MILK_VEIL);
		}
	}
	// The white water in lumps, morphing in place, lit from the sun's side (brighter facing it, shadowed behind)
	{
		const float lump = FallShading::Morphing(fallFlow.uvA, FallShading::LUMP_FREQUENCY, FallGround::Time + 1.9);
		const float lumpToSun = FallShading::Morphing(fallFlow.uvA + fallToSun * FallShading::LUMP_SUN_STEP, FallShading::LUMP_FREQUENCY, FallGround::Time + 1.9);
		const float shade = saturate(FallShading::LUMP_BASE + (lump - lumpToSun) * FallShading::LUMP_CONTRAST + (lump - 0.5) * FallShading::LUMP_BODY);
		fallFoamLight *= lerp(1.0, shade, saturate(max(fallWhite, fallSurfaceShare)));
		// the streaks: each its own brightness, a lit core dimming to its edges, and grain running down with the water
		const float core = smoothstep(FallShading::STREAK_CORE.x, FallShading::STREAK_CORE.y, fallRibbons.x);
		const float grain = lerp(1.0 - FallShading::STREAK_GRAIN, 1.0 + FallShading::STREAK_GRAIN * 0.5, smoothstep(0.25, 0.75, fallStreaks));
		fallFoamLight *= lerp(1.0, fallStreakTone * grain * lerp(FallShading::STREAK_EDGE_LIGHT, FallShading::STREAK_CORE_LIGHT, core), fallWhite);
		// thin foam at the ribbons' edges lets the water through: it takes the water's colour there, the crowns stay white
		const float thin = 1.0 - abs(fallRibbons.x * 2.0 - 1.0);
		fallFoamLight = lerp(fallFoamLight, diffuseOutput.refractionDiffuseColor * FallShading::EDGE_WATER_LIGHT, thin * FallShading::EDGE_WATER);
	}
	// D190: the volume's deeper foam takes the water's colour
	fallFoamLight = lerp(fallFoamLight, diffuseOutput.refractionDiffuseColor, fallAerationVolume.z * FallShading::AERATION_DEEP_TINT * (1.0 - fallSurfaceShare));
	// D179: aerated water is milky (the bubbles scatter the light inside it), less see-through, and its bubbles glint
	diffuseOutput.refractionDiffuseColor = lerp(diffuseOutput.refractionDiffuseColor, fluidFoamLight, saturate(fallBubbles * 0.2));
#					elif !defined(PBR_WATER)  // On PBR Water our foam drew as an opaque beige layer; off until it moves into his foam
	{
		// Bubbles in fast, foamy water make the water body lighter and milkier
		const float3 fluidPosition = input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
		FluidField::ApplyAeration(diffuseOutput.refractionDiffuseColor, fluidPosition, fluidRiverFoamLight, fluidFoamFade);
		FluidField::ApplyAeration(diffuseOutput.refractionColor, fluidPosition, fluidRiverFoamLight, 0.5 * fluidFoamFade);
	}
#					endif
#					if defined(FLUID_FIELD_FALLS)
	float fallRiverMul = diffuseOutput.refractionMul;  // (without depth or refractions: the river's look untinted)
#					endif
#					if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH) && defined(REFRACTIONS) && !defined(UNDERWATER)
	// All water takes on its own colour with depth, as it looks from underwater (distanceMul.w is the
	// depth below the surface over FogParam.z)
#						if defined(FLUID_FIELD_FALLS)
	// Calm water is the river plane's own, tinted by depth below the surface (at least RIVER_LOOK_DEPTH, or the sheet over rock
	// reads as glass); the falls' look comes in with fallMotion
	fallRiverMul = FluidField::TintedRefractionWeight(diffuseOutput.refractionMul, max(distanceMul.w * FogParam.z, FallShading::RIVER_LOOK_DEPTH * FallFalling));
	// D181: the tint by the sheet's own thickness along the view, at least SHEET_TINT_DEPTH
	diffuseOutput.refractionMul = FluidField::TintedRefractionWeight(diffuseOutput.refractionMul, max(min(distanceMul.x * FogParam.z, FallShading::SHEET_DEPTH / max(abs(dot(viewDirection, FallUp)), 0.25)), FallShading::SHEET_TINT_DEPTH));
#						else
	diffuseOutput.refractionMul = FluidField::TintedRefractionWeight(diffuseOutput.refractionMul, distanceMul.w * FogParam.z);
#						endif
#					endif
#					if defined(FLUID_FIELD_FALLS)
	{
		// The river plane's look where the water is calm (slow, level, not dropped, no air), the falls' as it moves: a thin
		// sheet, mostly see-through, what lies behind tinted by the water's own hue, its colour over at most BAKED_CLEAR_OPACITY; the
		// ribbons' haze thickens it
		const float fallMotion = saturate(max(max(fallDropped, smoothstep(FallShading::MOTION_SPEED.x, FallShading::MOTION_SPEED.y, fallFlow.fields.x)), fallAeratedLook)) * FallFalling;
		const float3 shallow = ShallowColor.xyz;  // as the river's own (upstream d34e245 dropped Color::Water)
		const float3 hue = shallow / max(max(shallow.r, max(shallow.g, shallow.b)), 1e-3);
		diffuseOutput.refractionColor *= lerp(1.0, hue, FallShading::BAKED_CLEAR_TINT * fallMotion);
		// its opacity by what lies behind it: over rock close behind it is a thin film the rock shows through (tinted); over a gap,
		// air or deep water behind, more of its own body colour
		const float clearOpacity = lerp(FallShading::CLEAR_OVER_ROCK, FallShading::CLEAR_OVER_GAP, smoothstep(FallShading::CLEAR_KNEE.x, FallShading::CLEAR_KNEE.y, saturate((input.FallData.x - FallShading::CLEAR_GAP.x) / (FallShading::CLEAR_GAP.y - FallShading::CLEAR_GAP.x))));
		const float fallsMul = lerp(min(diffuseOutput.refractionMul, clearOpacity), 1.0, fallRibbons.y * FallShading::RIBBON_HAZE_OPACITY);
		diffuseOutput.refractionMul = lerp(fallRiverMul, fallsMul, fallMotion);
		// milky water is murkier: less of what lies behind shows through
		diffuseOutput.refractionMul = lerp(diffuseOutput.refractionMul, 1.0, FallShading::UnifiedMilk(fallFlow.fields.w, fallFoamForming, fallMilkLip) * FallShading::UNIFIED_MURK);
	}
#					endif
#				endif

#				if !defined(UNDERWATER)
	if (SharedData::enbSettings.EnableWater) {
		float3 dirScatter = saturate(dot(normal.xyz, SharedData::DirLightDirection.xyz) * 0.5 + 0.5) * saturate(dot(viewDirection.xyz, SharedData::DirLightDirection.xyz) * 0.5 + 0.5) * ShadowSampling::GetDirectionalLighting();
		diffuseOutput.refractionDiffuseColor += DeepColor.xyz * dirScatter * surfaceShadow * SharedData::enbSettings.WaterSunLightingMultiplier;
	}
#				endif

	float3 diffuseColor = lerp(diffuseOutput.refractionColor, diffuseOutput.refractionDiffuseColor, diffuseOutput.refractionMul);

	depthControl = DepthControl * (distanceMul - 1) + 1;

	float3 specularLighting = 0;

#				if defined(LIGHT_LIMIT_FIX)
	uint lightCount = 0;

	uint clusterIndex = 0;
	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
		[loop] for (uint i = 0; i < lightCount; i++)
		{
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + i];
			LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];
			// Water has no engine shadow mask, so only shadow casters with a cached local shadow can light it.
			const bool localShadow = light.lightFlags & LightLimitFix::LightFlags::LocalShadow;
			if (LightLimitFix::IsLightIgnored(light) || ((light.lightFlags & LightLimitFix::LightFlags::ShadowCaster) && !localShadow)) {
				continue;
			}

			float3 lightDirection = light.positionWS.xyz - input.WPosition.xyz;
			float lightDist = length(lightDirection);

			float intensityMultiplier = LightLimitFix::GetAttenuation(lightDist, light);

			float3 normalizedLightDirection = normalize(lightDirection);

			float lightShadow = 1.0;
			[branch] if (localShadow)
				lightShadow = LightLimitFix::GetLocalShadow(DepthSampler, light.localShadowIndex, input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, normalizedLightDirection, false, Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount));

#					if defined(PBR_WATER)
			float3 lightColor = Color::PointLight(light.color.xyz) * light.fade *
			                    PBRWater::SpecularGGX(normal, -viewDirection, PBRWater::SafeNormalize(lightDirection, normal), pbrRoughness, 1.0 / PBRWater::WaterIOR,
									PBRWater::EmitterAngularRadius(PBRWater::PointLightEmitterRadius(), lightDist)) *
			                    PBRWater::Light1.w;
#					else
			float3 H = normalize(normalizedLightDirection - viewDirection);
			float HdotN = saturate(dot(H, normal));

			float3 lightColor = Color::PointLight(light.color.xyz) * pow(HdotN, FresnelRI.z) * light.fade;
#					endif
			specularLighting += lightColor * intensityMultiplier * lightShadow;
		}
	}
	specularColor += specularLighting * 3;
#				endif

#				if defined(UNDERWATER)
	float3 finalSpecularColor = lerp(ShallowColor.xyz, specularColor, 0.5);
	float3 finalColor = saturate(1 - length(input.WPosition.xyz) * 0.002) * ((1 - fresnel) * (diffuseColor - finalSpecularColor)) + finalSpecularColor;
	// Add ripple and splash color effects for underwater
#					if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization (darker for underwater)
	float3 debugColor = WetnessEffects::GetDebugWetnessColorUnderwater(waterData.rippleInfo, 1.5, 2.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#					endif
#				else

	float3 sunColor = GetSunColor(normal, viewDirection, input.WPosition.xyz, pbrRoughness) * surfaceShadow;
#					if defined(FLUID_FIELD)
	{
		// Foam is matte: no sun glint and a dull reflection where it is dense
#						if defined(FLUID_FIELD_FALLS)
		const float fluidMatte = saturate(fallFoam * 1.5);  // D159: the falls' own foam
#						elif defined(PBR_WATER)
		const float fluidMatte = 0.0;  // Our foam is off on PBR Water (above)
#						else
		const float fluidMatte = FluidField::FoamMatte(input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz);
#						endif
		sunColor *= 1.0 - fluidMatte;  // even a tenth of a glint shows through the foam's gaps as white flecks
		specularColor *= 1.0 - 0.5 * fluidMatte;
	}
#					endif

	// Reflection and sun weights. On refracting water vanilla fades both with refractionMul, which only
	// reaches 1 once the water is deep enough to fog, so shallow water lost its reflections.
	float surfaceMul = diffuseOutput.refractionMul;
	float sunMul = depthControl.w;
	bool shoreFadedSurface = false;

	if (SharedData::enbSettings.EnableWater) {
		sunColor *= SharedData::enbSettings.WaterSunSpecularMultiplier;
#					if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH) && defined(REFRACTIONS)
		// distanceMul.w is the water depth below the surface over FogParam.z, saturated
		surfaceMul = saturate(distanceMul.w * FogParam.z / max(min(ShoreFadeDepth, FogParam.z), 1e-4));
		sunMul = max(sunMul, surfaceMul);
		shoreFadedSurface = true;
#					endif
	}

#					if defined(VC) && !defined(PBR_WATER)
	float specularFraction = lerp(1, fresnel * surfaceMul, distanceBlendFactor);
	float3 finalColorPreFog = lerp(diffuseColor, specularColor, specularFraction) + sunColor * sunMul;

#						if !defined(UNIFIED_WATER)
	float fogDistanceFactor = input.FogParam.w;
	float3 fogColor = input.FogParam.xyz;
#						else
	float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
	float3 fogColor = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogDistanceFactor);
#						endif

#						if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#						endif
#						if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, fogColor, float4(input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, input.HPosition.z, 1));
		if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
			fogColor = exponentialHeightFog.xyz;
			fogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, fogColor, exponentialHeightFog.w);
		} else {
			fogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
			float3 expFogColor = exponentialHeightFog.xyz * GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, expFogColor, exponentialHeightFog.w);
		}
	} else {
		fogColor *= GetWaterFogFade();
		finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
	}
#						else
	fogColor *= GetWaterFogFade();
	finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
#						endif

	float3 finalColor = finalColorPreFog;

#						if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#						endif

#					else
#						if defined(PBR_WATER)
	// Water column: Beer-Lambert transmittance of the refracted scene plus in-scattered body colour.
	float pbrPathLength = distanceMul.x * FogParam.z;
#							if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	{
		// Nothing rendered behind the water (sky / far plane): treat the column as infinitely deep.
		float pbrRawDepth = DepthTex.Load(float3(screenPosition, 0)).x;
#								if defined(HORIZON_FIX)
		bool pbrEmpty = pbrRawDepth >= HorizonFix::EmptyDepthThreshold;
#								else
		bool pbrEmpty = pbrRawDepth >= 0.999999;
#								endif
		pbrPathLength = pbrEmpty ? 1e7 : max(depthMul - length(input.WPosition.xyz), 0.0);
	}
#							endif
	// Vertical depth of what is seen through the water, and the path the refracted ray really takes to it:
	// Snell bends it towards the vertical, so at grazing angles light crosses far less water than the
	// straight view ray suggests.
	float pbrBottomDepth = pbrPathLength * abs(viewDirection.z);
	float3 pbrRefracted = refract(viewDirection, normal, 1.0 / PBRWater::WaterIOR);
	float pbrWaterPath = pbrBottomDepth / max(-pbrRefracted.z, 0.2);
	// A breaker's face is no slab: the ray seen through it runs into the crest nearly level and leaves through its back,
	// crossing only the crest's thickness at that height, about three times the height still above it (a crest's back is
	// about a third as steep as a breaking face). As a slab down to the bed far behind, every face shaded as the same deep
	// body colour, a wall; through the crest the face is thin and bright near the top and deepens towards the trough.
	float pbrSteepFace = smoothstep(0.6, 0.2, abs(normal.z)) * saturate(pbrSurface.crest / (0.1 * PBRWater::UnitsPerMetre));
	float pbrCrestPath = 3.0 * max(pbrSurface.crest - pbrSurface.height, 0.0) + 0.05 * PBRWater::UnitsPerMetre;
	pbrWaterPath = lerp(pbrWaterPath, min(pbrWaterPath, pbrCrestPath), pbrSteepFace);
	// What the depth buffer shows behind a wave that rises above the still water (icebergs, cliffs, the far shore) is no
	// part of the water's background: the ray refracted into the wave bends down into the sea and never reaches it, and
	// the scene behind showed through every crest standing above the horizon. Only the bed under the water is seen.
	// (One depth-tested layer of water: the nearest wave already hides the ones behind it.)
	float pbrReachable = 1.0;
#							if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	if (pbrPathLength < 1e6) {
		float pbrBehind = input.WPosition.z + viewDirection.z * pbrPathLength - input.WaveParam.z;
		// Nor is anything far behind: the scene seen through the water holds no water, so past the wave the ray would
		// cross the waves behind, not the horizon and the land beyond them (a line along the horizon through every crest).
		float pbrBeyond = pbrPathLength * length(viewDirection.xy);
		float pbrUnreachable = max(saturate(pbrBehind / (0.25 * PBRWater::UnitsPerMetre)), saturate((pbrBeyond - 5.0 * PBRWater::UnitsPerMetre) / (5.0 * PBRWater::UnitsPerMetre))) *
		                       saturate(pbrSurface.height / (0.05 * PBRWater::UnitsPerMetre));
		// Blended as a share of the light, not of the path: lerp(path, 1e7, w) is ~1e5 units for even w = 0.01, and
		// everything just under a raised surface (a body floating in it, a leg) turned opaque body colour.
		pbrReachable = 1.0 - pbrUnreachable;
	}
#							endif

	// Clarity: drifting sediment patches, wave-stirred shallows, river silt, wading.
	float pbrFlow = saturate((1.0 - input.WaveParam.w) / max(PBRWater::Foam1.w, 0.01));
	float pbrColumnDepth = min(pbrSurface.depth, pbrBottomDepth);
	PBRWater::Turbidity pbrTurbidity = PBRWater::GetTurbidity(input.WaveParam.xyz, input.SurfaceNoise.x, pbrColumnDepth, PBRWater::FetchHeightRatio(pbrSurface.fetchMetres), input.WaveParam.w, pbrSurface.breakStage, pbrFlow);
	pbrSurface.turbidity = PBRWater::ColumnTurbidity(pbrTurbidity, pbrColumnDepth);
	PBRWater::WaterOptics pbrOptics = PBRWater::GetWaterOptics(PBRWater::Extinction(ShallowColor.xyz, FogParam.z * PBRWater::Light1.x), pbrSurface.turbidity);

	// Body colour: the water form's, shifted towards the sediment colour as sediment takes over.
	float3 pbrBodyLinear = ColorManagement::SceneToLinear(diffuseOutput.refractionDiffuseColor);
	pbrBodyLinear = PBRWater::BodyColour(pbrBodyLinear, PBRWater::SedimentBodyColour(pbrBodyLinear), pbrOptics);

	// Transmission (Beer-Lambert along the refracted path, in linear light). What is seen through the water
	// was itself lit through the water above it, so light reaching the bottom is attenuated by K_d.
	float pbrSunShare = Color::RGBToLuminance(dirColor) / max(Color::RGBToLuminance(dirColor + ambientColor), 1e-5);
	// Sunlight that reached this water through a crest lost what that water took (K_d: what it scatters forwards still
	// arrives): the body colour's and the bed's share of it, and the foam's.
	float3 pbrWaveSunLight = exp(-pbrOptics.downwelling * pbrWaveSun.path);
	float3 pbrWaveSunShare = lerp(1.0, pbrWaveSunLight, pbrSunShare);
	pbrBodyLinear *= pbrWaveSunShare;
#							if defined(REFRACTIONS)
	float3 pbrTransmittance = exp(-pbrOptics.extinction * pbrWaterPath) * pbrReachable;
	float3 pbrBottomLight = PBRWater::DownwellingTransmittance(pbrOptics, pbrBottomDepth, PBRWater::RefractedSunDirection(SunDir.xyz).z, pbrSunShare);
#							else
	float3 pbrTransmittance = 0.0;
	float3 pbrBottomLight = 1.0;
#							endif
	pbrBottomLight *= pbrWaveSunShare;
	float3 pbrTransmitted = ColorManagement::LinearToScene(ColorManagement::SceneToLinear(diffuseOutput.refractionColor) * pbrTransmittance * pbrBottomLight +
														   pbrBodyLinear * (1.0 - pbrTransmittance));

	// Sunlight transmitted through thin wave crests, coloured by the water it crosses.
	float pbrCrest = saturate(input.ShoreState.z / max(PBRWater::Optics0.w * 0.5, 1.0));
	float3 pbrSunLight, pbrSkyLight;
	ShadowSampling::ExtractLighting(1.0.xxx, pbrSunLight, pbrSkyLight);
	float pbrCrestThickness = max(PBRWater::Optics0.w * 0.6, 0.4 * PBRWater::UnitsPerMetre);
	// Through the crest's real thickness towards the sun where the heightfield reaches.
	float pbrCrestSunPath = pbrWaveSun.valid ? pbrWaveSun.path : -1.0;
	pbrTransmitted += pbrSunLight * dirShadow * PBRWater::WaveTranslucency(normal, -viewDirection, SunDir.xyz, pbrCrest, pbrOptics, pbrCrestThickness, pbrCrestSunPath) * PBRWater::Light0.y;

	// The waterline per pixel: the water's surface here (the waves at this pixel, or the swash sheet over the ground)
	// against the ground seen through it, so the edge follows the true bed - and rocks, piles and legs standing in the
	// water - instead of the mesh's triangles cutting through the terrain. Over the last few centimetres the surface
	// (reflection, body colour) fades into the ground; foam lies on the water, so it stays on the thin sheet.
	float pbrBed = pbrSurface.elevation;
#							if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	if (pbrPathLength < 1e6) {
		// Only ground just behind the surface is the bed under it. Seen through a wave that rises in front of it, an
		// iceberg, a mountain or the horizon far behind stood near or above the crest and the pixel was taken for (nearly)
		// dry ground: the wave showed it. Further out, the terrain under the pixel.
		if (pbrPathLength * length(viewDirection.xy) < 3.0 * PBRWater::UnitsPerMetre)
			pbrBed = input.WPosition.z + viewDirection.z * pbrPathLength - input.WaveParam.z;
	}
#							endif
	float pbrFilm = max(pbrSurface.height, PBRWater::SwashSurface(pbrBed, pbrSurface.swashSurface, pbrSurface.swashDepth)) - pbrBed;
	// With refractions, dry pixels show the ground instead (pbrEdge = 0): the stencil pass put them in the water mask,
	// and ISWaterBlend composites whatever this target holds there - after a discard, an old frame's water.
	float pbrEdge = smoothstep(0.0, 0.06 * PBRWater::UnitsPerMetre, pbrFilm);

	// Foam (PBRWater.hlsli): the shore waves' and the swash's, drawn with the foam texture below; and the open sea's
	// (pbrSurface.fftFoam, the accumulated cascade foam of GarrettGunnell/Water), blended in raw as a coverage.
	// Nothing is drawn from what the depth buffer shows through the water:
	// that put foam in the shape of whatever stood in it (legs, rocks) and of the shoreline seen from below. It is drawn
	// with the foam texture (Crest's black-point fade) where the surface is, a diffuse scatterer
	// (Dupuy & Bruneton 2012) lit by the sun and the sky like any surface.
	// The swash sheet runs up the beach foaming: each bore's own foam, carried up with its water and thinning as the sheet
	// spreads and drains, and the white water where uprushes and backwash collide (SwashStepCS). The sheet is where the
	// swash covers the bed near or above the still water.
	float pbrTipGap = pbrSurface.swashDepth > PBRWater::SwashMinDepth ? pbrSurface.swashSurface - pbrBed : -PBRWater::UnitsPerMetre;
	// Antialiased over the pixel: a hard step drew the uprush's edge in stairs.
	float pbrSheet = saturate(pbrTipGap / max(fwidth(pbrTipGap), 0.01) + 0.5) * smoothstep(-0.15 * PBRWater::UnitsPerMetre, 0.0, pbrBed);
	// What the breakers left on the water rides the water under the waves (ShoreFoamCS); the swash's is its own.
	float2 pbrSimFoam = pbrSurface.simFoam;
	// Two foams. The breaking wave's own white water (pbrSurface.breakerFoam: the folding of its surface and the profile's
	// foam, BreakerFoam) is drawn the way the open sea's is, as a raw coverage with no texture, so there is nothing on the
	// face, crest, lip and tumbling white water to stretch. The foam texture is kept for the flat white water: the bore once
	// it has formed and lies near horizontal, what the surf left on the water where it lies flat (on a rising face that
	// joins the breaker's, drawn with the sea's foam), and the swash sheet's. The two cross-fade by
	// how flat the surface is, and for the wave itself by its stage (bore), so the pattern takes over as the wave rolls flat.
	// The pattern holds still where the surface is, projected from above: the foam moves over it, as its coverage.
	// SurfaceFoam takes screen derivatives and skips the pattern where it gets no foam, so it is called unconditionally.
	float3 pbrFoamPosition = input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
	// Near the waterline the mesh lies on the sand above the water (DisplaceSurface) and climbs every ledge in the bed;
	// the water seen there is further along the view ray, where it reaches the water's surface. Projected from the mesh,
	// the pattern ran down the steep faces over the ledges in vertical streaks, a step along each one.
	float pbrMeshAbove = (input.WPosition.z - input.WaveParam.z) - max(pbrSurface.height, PBRWater::SwashSurface(pbrBed, pbrSurface.swashSurface, pbrSurface.swashDepth));
	if (pbrMeshAbove > 0.0 && viewDirection.z < -0.01)
		pbrFoamPosition += viewDirection * min(pbrMeshAbove / -viewDirection.z, pbrPathLength);
	float pbrFlat = smoothstep(0.80, 0.95, pbrSurface.waveNormal.z);
	float pbrBoreFlat = pbrSurface.bore * pbrFlat;
	float pbrPatternCoverage = max(max(pbrSurface.foam * pbrBoreFlat, pbrSimFoam.x * pbrFlat * (1.0 - pbrSurface.clearFace)), pbrSimFoam.y * pbrSheet);
	// What the swash stranded on the sand as it drained (the swash mark at the top of each uprush, lace where the sheet
	// thinned): drawn on the ground where no water covers it, the pattern held where it lies.
	float pbrStrandedFoam = PBRWaterFoam::SurfaceFoam(pbrFoamPosition.xy, pbrSimFoam.y * (1.0 - pbrSheet), PBRWater::FoamTile(), PBRWater::Tune5.y, PBRWater::OceanSampler);
	// The surf's leftover foam on faces that are not flat is in pbrSurface.breakerFoam, drawn with the sea's (BreakerFoamDrawn).
	float pbrRawCoverage = pbrSurface.breakerFoam;
	float pbrFoamPattern = saturate(PBRWaterFoam::SurfaceFoam(pbrFoamPosition.xy, pbrPatternCoverage, PBRWater::FoamTile(), PBRWater::Tune5.y, PBRWater::OceanSampler) + pbrRawCoverage);
	// Whitewater is a layer of bubbles light scatters through, not a surface: it is lit about the normal bent halfway up,
	// with the sun wrapping around it. Lit about the water's own normal, the foam on the underside of a curling lip (facing
	// down, seeing neither the sun nor the sky) drew a dark line along every curling crest.
	float3 pbrFoamNormal = PBRWater::SafeNormalize(normal + float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 1.0));
	float3 pbrFoamIrradiance = ColorManagement::SceneToLinear(ShadowSampling::GetDirectionalLighting()) * (dirShadow * saturate((dot(pbrFoamNormal, SunDir.xyz) + 0.5) / 1.5)) * pbrWaveSunLight;
	float3 pbrSkyIrradiance = ColorManagement::SceneToLinear(ShadowSampling::GetAmbientLighting());
#							if defined(SKYLIGHTING)
	pbrSkyIrradiance *= Skylighting::EvaluateDiffuse(skylightingSH, pbrFoamNormal, Skylighting::GetFadeOutFactor(input.WPosition.xyz));
#							endif
	float3 pbrFoamColor = ColorManagement::LinearToScene(PBRWaterFoam::Albedo * (pbrFoamIrradiance + pbrSkyIrradiance));

	float specularFraction = lerp(1, fresnel, distanceBlendFactor);
	float3 finalColorPreFog = lerp(pbrTransmitted, specularColor, specularFraction);
	float3 pbrSunGlint = sunColor * (depthControl.w * pbrWaveSun.visible);
#							if !defined(REFRACTIONS)
	if (pbrFilm <= 0.0 && (uint)PBRWater::Draw0.z != PBRWater::DebugEdgeTest)
		discard;
#							endif
	pbrSunGlint *= pbrEdge;
	float pbrFilmFoam = smoothstep(0.0, 0.04 * PBRWater::UnitsPerMetre, pbrFilm);
	float pbrFoam = max(pbrFoamPattern * pbrFilmFoam, saturate(pbrSurface.fftFoam) * pbrFilmFoam);
	// Foam scatters the glint with the rest of the light that reaches it.
	finalColorPreFog = lerp(finalColorPreFog, pbrFoamColor, pbrFoam) + pbrSunGlint * (1.0 - pbrFoam);
#							if defined(REFRACTIONS)
	// The foam stranded on the dry sand lies on the ground the final blend shows where there is no water (pbrEdge).
	float pbrStrandedShown = (1.0 - pbrEdge) * pbrStrandedFoam;
	float pbrCover = pbrEdge + pbrStrandedShown;
	finalColorPreFog = (pbrEdge * finalColorPreFog + pbrStrandedShown * pbrFoamColor) / max(pbrCover, 1e-4);
	pbrEdge = pbrCover;
#							endif
#						else
	float specularFraction = lerp(1, fresnel, distanceBlendFactor);
	float waterOpacity = diffuseOutput.refractionMul;
	float3 finalColorPreFog;
	if (shoreFadedSurface) {
		// Reflection takes its share first; the rest splits between the water color and the see-through refraction.
		// The sun is a reflection too, so it fades in with the surface instead of rimming see-through water.
		// Divided by the opacity because the composite below lerps from the refraction by it; the floor keeps that
		// continuous and moves at most 0.1% of the weight off the refraction.
		float reflectionWeight = specularFraction * surfaceMul;
		waterOpacity = max(lerp(diffuseOutput.refractionMul, 1.0, reflectionWeight), 1e-3);
		finalColorPreFog = (diffuseOutput.refractionDiffuseColor * (diffuseOutput.refractionMul * (1.0 - reflectionWeight)) + specularColor * reflectionWeight + sunColor * (sunMul * surfaceMul)) / waterOpacity;
	} else {
		finalColorPreFog = lerp(diffuseOutput.refractionDiffuseColor, specularColor, specularFraction) + sunColor * depthControl.w;
	}
#							if defined(FLUID_FIELD)
	{
		// Foam is opaque: it covers the see-through refraction as well
#								if defined(FLUID_FIELD_FALLS)
		// D159: the falls' own foam, laid over the water as the river's is
		if (fallAboveWater < 0.0) {
			// D176: under the water below, the bubbles' colour, dimmed into the water's hue with depth
			const float3 hue = diffuseOutput.refractionDiffuseColor / max(Color::RGBToLuminance(diffuseOutput.refractionDiffuseColor), 1e-3);
			finalColorPreFog = fallFoamLight * lerp(hue * FallShading::UNDER_DEEP_LIGHT, 1.0, fallUnderVis);
			waterOpacity = 1.0;
		} else if (fallFoam > 0.0) {
			// wet (a little of the reflection by fresnel) and lit through (backlit into the sun, wrapped round its
			// shaded side, in the water's hue, most where it is thin)
			{
				const float3 toSun = SharedData::DirLightDirection.xyz;
				const float3 sunLight = ShadowSampling::GetDirectionalLighting() * surfaceShadow;
				const float3 hue = diffuseOutput.refractionDiffuseColor / max(Color::RGBToLuminance(diffuseOutput.refractionDiffuseColor), 1e-3);
				// its mounds: a height from how whole it is (thin foam a film) times the billows, their slope tilting its normal
				const float whole = saturate(fallFoam / max(FallShading::FOAM_OPACITY, 1e-3));
				const float3 tilt = (fallBillowSlope.x * fallAcross + fallBillowSlope.y * fallAlong) / FallShading::FALLS_UV_WORLD * FallShading::FOAM_MOUND * whole;
				const float3 mound = FallShading::SafeNormalize(normal - tilt, normal);
				const float through = pow(saturate(dot(viewDirection, toSun)), FallShading::FOAM_SSS_POWER) * FallShading::FOAM_SSS_BACK +
				                      saturate((dot(mound, toSun) + FallShading::FOAM_SSS_WRAP) / (1.0 + FallShading::FOAM_SSS_WRAP)) * (1.0 - saturate(dot(mound, toSun))) * FallShading::FOAM_SSS_WRAP_LIGHT;
				// thin foam, and the thin hollows between mounds, let more through
				const float thin = lerp(1.0, FallShading::FOAM_SSS_THICK, whole) * (1.0 + FallShading::RIM_GLOW * (1.0 - fallBillow));
				// diffuse, light scattered inside it: wrapped past the terminator, the shaded side down to the ambient's share, never black
				const float sunLum = Color::RGBToLuminance(sunLight), ambientLum = Color::RGBToLuminance(fluidAmbientLight);
				const float wrapped = saturate((dot(mound, toSun) + FallShading::FOAM_WRAP) / (1.0 + FallShading::FOAM_WRAP));
				const float diffuse = lerp(ambientLum / max(ambientLum + sunLum, 1e-4), 1.0, wrapped);
				fallFoamLight *= lerp(1.0, diffuse, FallShading::FOAM_DIFFUSE);
				// its hollows less lit, its undersides toward the water's hue (the tops the sky's light, as it is)
				fallFoamLight *= lerp(1.0, FallShading::HOLLOW_DARK, (1.0 - fallBillow) * whole);
				fallFoamLight *= lerp(lerp(1.0, hue, FallShading::TWO_TONE), 1.0, saturate(mound.z * 0.5 + 0.5));
				fallFoamLight = lerp(fallFoamLight, specularColor, saturate(fresnel * FallShading::FOAM_REFLECT)) +
				                sunLight * lerp(1.0, hue, FallShading::FOAM_SSS_TINT) * through * thin;
				// diffuse: never brighter than a white surface of FOAM_ALBEDO under this sun and sky, eased into it above FOAM_KNEE of that
				const float ceiling = FallShading::FOAM_ALBEDO * (sunLum + ambientLum);
				const float knee = ceiling * FallShading::FOAM_KNEE;
				const float lum = Color::RGBToLuminance(fallFoamLight);
				[flatten] if (lum > knee && ceiling > knee)
				{
					const float eased = knee + (ceiling - knee) * (1.0 - exp(-(lum - knee) / (ceiling - knee)));
					fallFoamLight *= eased / lum;
				}
				fallFoamLight = FluidField::FoamTint(fallFoamLight, fluidLight);
			}
			const float fallOpacity = lerp(waterOpacity, 1.0, fallFoam);
			finalColorPreFog = (finalColorPreFog * (waterOpacity * (1.0 - fallFoam)) + fallFoamLight * fallFoam) / max(fallOpacity, 1e-3);
			waterOpacity = fallOpacity;
		}
#								else
		// shortcut: the river's lobe foam (FoamLook) is off: its sparse peaks read as bright white specks; redo it before drawing it again
#								endif
	}
#							endif
#						endif

#						if !defined(UNIFIED_WATER)
	float fogDistanceFactor = input.FogParam.w;
	float3 preFogColor = input.FogParam.xyz;
#						else
	float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
	float3 preFogColor = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogDistanceFactor);
#						endif

#						if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		preFogColor = ImageBasedLighting::GetFogIBLColor(preFogColor);
	}
#						endif
#						if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, preFogColor, float4(input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, input.HPosition.z, 1));
		if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
			preFogColor = exponentialHeightFog.xyz;
			preFogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, preFogColor, exponentialHeightFog.w);
		} else {
			preFogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
			float3 expFogColor = exponentialHeightFog.xyz * GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, expFogColor, exponentialHeightFog.w);
		}
	} else {
		preFogColor *= GetWaterFogFade();
		finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
	}
#						else
	preFogColor *= GetWaterFogFade();

	finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
#						endif

#						if defined(PBR_WATER)
	float3 finalColor = finalColorPreFog;
	// Debug views go through the same per-pixel waterline as the water: drawn over the whole coverage, they showed the
	// coarser, triangle-shaped region the passes keep (where the ground shows) as hard corners that are never on screen.
	if (PBRWater::Draw0.z > 0.5)
		finalColor = PBRWater::DebugView((uint)PBRWater::Draw0.z, pbrSurface, pbrFoam, input.ShoreState.z);
	// The waves' sun shadows: red the sun's glint hidden, green the water the sunlight crosses (2 m), blue off the heightfield.
	if ((uint)PBRWater::Draw0.z == PBRWater::DebugWaveSun)
		finalColor = float3(1.0 - pbrWaveSun.visible, saturate(pbrWaveSun.path / (2.0 * PBRWater::UnitsPerMetre)), pbrWaveSun.valid ? 0.0 : 0.3);
#							if defined(FLUID_FIELD)
	{
		// Heights in metres above the still water. The surface = waves (his) + the sim's surface in its area (or his ripples
		// outside it) + the lift that holds the mesh above the bed near the waterline (DisplaceSurface).
		float fluidHeight;
		float2 fluidSlope;
		PBRWater::FluidSurface(input.WaveParam.xyz, fluidHeight, fluidSlope);
		const float fluidMetres = fluidHeight * PBRWater::FluidShare(input.WPosition.xyz) * PBRWater::MetresPerUnit;
		const float waveMetres = input.ShoreState.z * PBRWater::MetresPerUnit - fluidMetres;
		const float liftMetres = (input.WPosition.z - input.ShoreState.w - input.ShoreState.z) * PBRWater::MetresPerUnit;
		// Height layers: red his waves and ripples, green the sim, blue the lift; each |m|, full at 1 m
		if ((uint)PBRWater::Draw0.z == PBRWater::DebugHeightLayers)
			finalColor = saturate(abs(float3(waveMetres, fluidMetres, liftMetres)));
		// The sim's surface against this draw's still plane: red above, blue below (full at 1 m), grey contour lines every
		// 0.25 m, white beyond 1 m, dark outside the simulation. A flat colour over a whole draw is an offset between them.
		if ((uint)PBRWater::Draw0.z == PBRWater::DebugFluidHeight) {
			float4 fluidSurface;
			const bool simulated = FluidField::SampleSurface(input.WaveParam.xyz + FrameBuffer::CameraPosAdjust.xyz, fluidSurface);
			const float raw = fluidHeight * PBRWater::MetresPerUnit;
			const float contour = step(0.9, frac(abs(raw) * 4.0)) * 0.3;
			finalColor = !simulated ? 0.05 : abs(raw) >= 1.0 ? 1.0 :
			                                                   float3(saturate(raw), fluidSurface.w * 0.3, saturate(-raw)) + contour;
		}
		// Waterline test: magenta where his mesh test (AboveWaterline, his terrain only) cuts the water; otherwise red how
		// much the per-pixel edge (pbrEdge, the sim's surface counted) fades it into the ground behind
		if ((uint)PBRWater::Draw0.z == PBRWater::DebugEdgeTest)
			finalColor = PBRWater::AboveWaterline(input.WPosition.xyz, input.ShoreState) ? float3(1.0, 0.0, 1.0) : float3(1.0 - pbrEdge, 0.0, 0.0);
		// Sim bed vs the ground behind the pixel (depth buffer, within 3 m), both above the still water: green within 5 cm,
		// red the sim's bed higher (it has rock where there is none: gaps), blue lower (it misses rock: lips, shards), 1 at
		// 0.5 m; black where the sim has no water, grey where the ground behind is out of reach
		if ((uint)PBRWater::Draw0.z == PBRWater::DebugSimBed) {
			const float3 absPos = input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
			const float simDepth = FluidField::SampleFlowOutput(absPos.xy).w;
			const float simBed = fluidHeight - simDepth;
			const bool groundSeen = pbrPathLength < 1e6 && pbrPathLength * length(viewDirection.xy) < 3.0 * PBRWater::UnitsPerMetre;
			const float diff = (simBed - pbrBed) * PBRWater::MetresPerUnit;
			finalColor = simDepth <= 0.0 ? 0.0 : !groundSeen  ? 0.3 :
			                                 abs(diff) < 0.05 ? float3(0.0, 0.8, 0.0) :
			                                                    float3(saturate(diff * 2.0), 0.0, saturate(-diff * 2.0));
		}
		// The sim's water depth (its bed: terrain, rays and mesh cuts): blue deep (2 m), red shallow, white contours every
		// 0.25 m, black where the sim has no water (a rock's top it counts as dry); dark grey outside the simulation
		if ((uint)PBRWater::Draw0.z == PBRWater::DebugFluidDepth) {
			const float3 absPos = input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
			const FluidField::Level nearLevel = FluidField::NearLevel();
			float2 nearCell;
			// Point read of the near level: the bilinear blend would average dry (0) cells into the depth
			const bool inNear = FluidField::ToCell(nearLevel, absPos.xy, nearCell);
			const float depthUnits = inNear ? FluidField::WaterFlowTex.Load(int3(int2(round(nearCell)) & (nearLevel.GridRes - 1), 0)).w : -2.0;
			const float metres = depthUnits * PBRWater::MetresPerUnit;
			const float t = saturate(metres / 2.0);
			const float contour = step(0.9, frac(metres * 4.0)) * 0.6;
			finalColor = !inNear ? 0.05 : depthUnits <= 0.0 ? 0.0 :
			                                                  float3(1.0 - t, 0.2, t) + contour;
		}
	}
#							endif
#							if defined(REFRACTIONS)
	// The thinnest water shows the ground it covers (as it is, already lit and fogged).
	float3 pbrGround = RefractionTex.Sample(RefractionSampler, FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw)).xyz;
	finalColor = lerp(pbrGround, finalColor, pbrEdge);
#							endif
#						else
	float3 refractionColor = diffuseOutput.refractionColor;

	float fogFactor = min(FogParam.w, pow(saturate(-diffuseOutput.depth * FogParam.y - FogParam.x), FogParam.z));
	float3 fogColor = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogFactor);
#							if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled && ExponentialHeightFog::ShouldDisableVanillaFog()) {
		fogFactor = 0;
	}
#							endif
#							if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#							endif
	refractionColor = lerp(refractionColor, fogColor, fogFactor);

	float3 finalColor = lerp(refractionColor, finalColorPreFog, waterOpacity);
#						endif
#						if defined(FLUID_FIELD_FALLS)
	// D180: bubbles over the result, or the water's colour hides them
	{
		// Air spheres facing the camera: silvery rims, sun highlight, lensed background
		const float3 toEye = -viewDirection;
		const float3 right = FallShading::SafeNormalize(cross(float3(0.0, 0.0, 1.0), toEye), float3(1.0, 0.0, 0.0));
		const float3 up = cross(toEye, right);
		const float3 bubble = FallShading::BubbleColor(fallBubbleSample.yz, right, up, toEye, SharedData::DirLightDirection.xyz,
			ShadowSampling::GetDirectionalLighting() * surfaceShadow, fluidFoamLight, refractionColor);
		// under the foam: hidden where it is dense, showing at its thin edges, where it forms and in its gaps
		const float underFoam = 1.0 - smoothstep(FallShading::BUBBLE_UNDER_FOAM.x, FallShading::BUBBLE_UNDER_FOAM.y, fallFoam / max(FallShading::FOAM_OPACITY, 1e-3));
		finalColor = lerp(finalColor, bubble, saturate(fallBubbles * FallShading::BUBBLE_GLINT) * underFoam);
	}

#						endif
#						if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#						endif
#					endif

#				endif
#			endif
#			if defined(PBR_WATER)
	if (PBRWater::Draw0.y > 0.5) {
		float wire = PBRWater::WireframeMask(input.Barycentric);
		float3 background = PBRWater::Draw0.y > 1.5 ? 0.0 : finalColor;
		finalColor = lerp(background, float3(0.1, 1.0, 0.6), wire);
	}
#			endif
	psout.Lighting = float4(finalColor, isSpecular);
#			if defined(FLUID_FIELD_FALLS)
	// D159: premultiplied for FallRefraction's blend (one, one minus source alpha); alpha only fades the edges
	finalColor = clamp(finalColor, 0.0, FallShading::MAX_OUTPUT);  // D3D's min and max return the other value for NaN
	psout.Lighting = float4(finalColor * fallAlpha, fallAlpha);
#			endif
#		endif

#		if defined(STENCIL)
	float3 viewDirection = normalize(input.WorldPosition.xyz);
	float3 normal =
		normalize(cross(ddx_coarse(input.WorldPosition.xyz), ddy_coarse(input.WorldPosition.xyz)));
	float VdotN = dot(viewDirection, normal);
#			if defined(PBR_WATER)
	// The mask also marks the water for ISWaterBlend and the upscaler: on a wave face seen edge-on or from behind
	// (the coarse derivatives span a crest) it rounded to 0 in the UNORM target and punched 2x2 holes in the water.
	VdotN = max(abs(VdotN), 2.0 / 255.0);
#			endif
	psout.WaterMask = float4(0, 0, VdotN, 0);

	float4 previousPosition = input.PreviousWorldPosition;
#			if defined(PBR_WATER)
	// The surf's foam, read at the displaced position (this pass has no undisplaced one); the foam grid's texels are
	// coarser than the difference almost everywhere.
	// The foam's pattern holds still where the surface is, so where it covers the water it does not move sideways with the
	// mesh: temporal AA and the upscalers reprojected it from where the mesh was and smeared it.
	float pbrMotionFoam = PBRWater::ShoreFoamAt(input.WorldPosition.xy + FrameBuffer::CameraPosAdjust.xy);
	// Last frame's position is relative to last frame's camera.
	float2 pbrFoamPrevious = input.WorldPosition.xy + (FrameBuffer::CameraPosAdjust.xy - FrameBuffer::CameraPreviousPosAdjust.xy);
	previousPosition.xy = lerp(previousPosition.xy, pbrFoamPrevious, saturate(2.0 * pbrMotionFoam));
#			endif
	psout.MotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, previousPosition);
#		endif

#		if defined(FLUID_FIELD_FALLS) && !defined(STENCIL)
	// The foam also slides down the flow: where it covers, its motion this frame too, or the anti-aliasing smeared it
	float3 fallPrevious = input.FallPrevious;
	{
		const float phaseStep = frac(FallGround::Scroll.x - FallGround::PreviousPhase + 0.5) - 0.5;  // (across the scroll's wrap)
		const float frameSeconds = phaseStep / max(FallGround::Scroll.y, 1e-4);
		fallPrevious -= fallFlow.velocity * frameSeconds * saturate(fallFoam / max(FallShading::FOAM_OPACITY, 1e-3));
	}
	psout.MotionVector = MotionBlur::GetSSMotionVector(float4(input.WPosition.xyz, 1.0), float4(fallPrevious, 1.0));
#		endif
	return psout;
}

#	endif

#endif
