cbuffer SceneCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gViewProj;
    float3 gEyePos;
    float  _pad0;
    float3 gLightDir;
    float  _pad1;
    float3 gLightColor;
    float  _pad2;
    float2 gTextureOffset;
    float2 gTextureTiling;
};

cbuffer MaterialCB : register(b1)
{
    float4 gDiffuse;
    float3 gSpecular;
    float  gShininess;
};

Texture2D gDiffuseMap : register(t0);
SamplerState gWrapSampler : register(s0);

struct VSIn
{
    float3 Pos      : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct VSOut
{
    float4 PosH     : SV_POSITION;
    float3 PosW     : POSITION0;
    float3 NrmW     : NORMAL0;
    float2 TexCoord : TEXCOORD0;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 posW = mul(float4(v.Pos, 1.0f), gWorld);
    o.PosW = posW.xyz;
    o.NrmW = mul(v.Normal, (float3x3)gWorld);
    o.PosH = mul(posW, gViewProj);
    o.TexCoord = v.TexCoord * gTextureTiling + gTextureOffset;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 texel = gDiffuseMap.Sample(gWrapSampler, i.TexCoord);
    float alpha = texel.a * gDiffuse.a;
    clip(alpha - 0.05f);

    float3 N = normalize(i.NrmW);
    float3 L = normalize(-gLightDir);
    float3 V = normalize(gEyePos - i.PosW);
    float3 H = normalize(L + V);

    float3 ambient = 0.12f;
    float ndotl = saturate(dot(N, L));
    float3 diffuseLight = ndotl * gLightColor;
    float spec = pow(saturate(dot(N, H)), max(gShininess, 1.0f));

    float3 base = texel.rgb * gDiffuse.rgb;
    float3 color = base * (ambient + diffuseLight) + spec * gSpecular * gLightColor;
    return float4(color, alpha);
}
