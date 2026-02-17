cbuffer SceneCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gViewProj;
    float3 gEyePos;
    float  _pad0;
    float3 gLightDir;   // направление к поверхности (луч света = -dir)
    float  _pad1;
    float3 gLightColor;
    float  _pad2;
};

struct VSIn
{
    float3 Pos    : POSITION;
    float3 Normal : NORMAL;
};

struct VSOut
{
    float4 PosH   : SV_POSITION;
    float3 PosW   : POSITION0;
    float3 NrmW   : NORMAL0;
};

VSOut VSMain(VSIn v)
{
    VSOut o;

    float4 posW = mul(float4(v.Pos, 1.0f), gWorld);
    o.PosW = posW.xyz;

    // нормаль
    o.NrmW = mul(v.Normal, (float3x3)gWorld);

    o.PosH = mul(posW, gViewProj);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 N = normalize(i.NrmW);
    float3 L = normalize(-gLightDir); // вектор от точки к источнику
    float3 V = normalize(gEyePos - i.PosW);
    float3 H = normalize(L + V);

    float3 ambient = 0.10f;

    float ndotl = saturate(dot(N, L));
    float3 diffuse = ndotl * gLightColor;

    // Phong/Blinn-Phong (здесь Blinn для стабильности).
    float specPow = 64.0f;
    float spec = pow(saturate(dot(N, H)), specPow);
    float3 specular = spec * gLightColor;

    float3 base = float3(0.85f, 0.75f, 0.55f); // 
    float3 color = base * (ambient + diffuse) + specular * 0.35f;

    return float4(color, 1.0f);
}
