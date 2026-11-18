cbuffer WindowHolesCB : register(b0)
{
	row_major float4x4 ViewProj;
	float3 PosAdjust;
	float pad0;
};

// World-space triangle list of the cell's window glass
StructuredBuffer<float3> WindowGlass : register(t0);

float4 main(uint vertexId : SV_VertexID) : SV_POSITION
{
	float4 positionCS = mul(float4(WindowGlass[vertexId] - PosAdjust, 1), ViewProj);
	// Far depth: the pane opens the sun ray whatever its distance along it
	return float4(positionCS.xy, positionCS.w, positionCS.w);
}
