cbuffer WindowHolesCB : register(b0)
{
	row_major float4x4 ViewProj;
	float3 PosAdjust;
	float GlowThreshold;  // glass opens only where its glow map is at least this bright
	uint Mode;            // 1: glass, opens where its glow map is bright; 2: opening, opens where its texture is see-through
	float AlphaThreshold;
	float2 pad0;
	float3 ShellOffset;  // shell casters: moved away from the sun, so lit faces don't shadow themselves
	float pad1;
};

struct HoleVertex
{
	float3 Position;
	float2 UV;
};

// World-space triangle list of the cell's window glass and openings
StructuredBuffer<HoleVertex> WindowGlass : register(t0);

Texture2D<float4> HoleTexture : register(t0);
SamplerState HoleSampler : register(s0);

struct VSOutput
{
	float4 Position: SV_POSITION;
	float2 UV: TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
	VSOutput output;
	if (Mode == 3) {
		// The room's shell as a caster, at its own depth
		output.Position = mul(float4(WindowGlass[vertexId].Position + ShellOffset - PosAdjust, 1), ViewProj);
		output.UV = 0;
		return output;
	}
	float4 positionCS = mul(float4(WindowGlass[vertexId].Position - PosAdjust, 1), ViewProj);
	// Far depth: the pane opens the sun ray whatever its distance along it
	output.Position = float4(positionCS.xy, positionCS.w, positionCS.w);
	output.UV = WindowGlass[vertexId].UV;
	return output;
}

// Glass: glow maps are white on the panes and black on the frame. Openings: only their see-through pixels open.
// Anything clipped keeps the "shadowed" depth.
void psmain(VSOutput input)
{
	const float4 texel = HoleTexture.Sample(HoleSampler, input.UV);
	if (Mode == 1)
		clip(max(texel.r, max(texel.g, texel.b)) - GlowThreshold);
	else if (Mode == 2)
		clip(AlphaThreshold - texel.a);
}
