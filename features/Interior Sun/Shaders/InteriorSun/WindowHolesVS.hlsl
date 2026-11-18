cbuffer WindowHolesCB : register(b0)
{
	row_major float4x4 ViewProj;
	float3 PosAdjust;
	float GlowThreshold;  // glass opens only where its glow map is at least this bright
	uint UseGlow;
	float3 pad0;
};

struct HoleVertex
{
	float3 Position;
	float2 UV;
};

// World-space triangle list of the cell's window glass and openings
StructuredBuffer<HoleVertex> WindowGlass : register(t0);

Texture2D<float4> GlowMap : register(t0);
SamplerState GlowSampler : register(s0);

struct VSOutput
{
	float4 Position: SV_POSITION;
	float2 UV: TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
	VSOutput output;
	float4 positionCS = mul(float4(WindowGlass[vertexId].Position - PosAdjust, 1), ViewProj);
	// Far depth: the pane opens the sun ray whatever its distance along it
	output.Position = float4(positionCS.xy, positionCS.w, positionCS.w);
	output.UV = WindowGlass[vertexId].UV;
	return output;
}

// Glow maps are white on the panes and black on the frame: the frame keeps the "shadowed" depth
void psmain(VSOutput input)
{
	if (UseGlow) {
		const float3 glow = GlowMap.Sample(GlowSampler, input.UV).rgb;
		clip(max(glow.r, max(glow.g, glow.b)) - GlowThreshold);
	}
}
