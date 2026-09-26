cbuffer ShadowCB : register(b0)
{
    float4x4 gWorldViewProj;
};

struct VertexInput
{
    float3 position : POSITION;
};

float4 VSShadow(VertexInput input) : SV_POSITION
{
    return mul(float4(input.position, 1.0f), gWorldViewProj);
}
