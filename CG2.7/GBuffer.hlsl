cbuffer SceneCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gViewProj;
    float2 gTextureOffset;
    float2 gTextureTiling;
    float3 gEyePosition;
    uint   gColorizeTiles;
    uint   gEnableNormalMapping;
    uint   gEnableDisplacement;
    float2 _togglePadding;
};

cbuffer MaterialCB : register(b1)
{
    float4 gDiffuse;
    float3 gSpecular;
    float  gShininess;
    float  gDisplacementScale;
    float  gDisplacementBias;
    float  gMinTessFactor;
    float  gMaxTessFactor;
    float  gTessNearDistance;
    float  gTessFarDistance;
    float2 _materialPadding;
};

Texture2D gDiffuseMap      : register(t0);
Texture2D gNormalMap       : register(t1);
Texture2D gDisplacementMap : register(t2);
SamplerState gWrapSampler  : register(s0);

struct VSIn
{
    float3 Pos      : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Tangent  : TANGENT;
};

struct ControlPoint
{
    float3 PosW     : POSITION0;
    float3 NrmW     : NORMAL0;
    float2 TexCoord : TEXCOORD0;
    float4 TangentW : TANGENT0;
};

ControlPoint VSGeometry(VSIn vertex)
{
    ControlPoint output;
    output.PosW = mul(float4(vertex.Pos, 1.0f), gWorld).xyz;
    output.NrmW = normalize(mul(vertex.Normal, (float3x3)gWorld));
    output.TangentW = float4(
        normalize(mul(vertex.Tangent.xyz, (float3x3)gWorld)), vertex.Tangent.w);
    output.TexCoord = vertex.TexCoord * gTextureTiling + gTextureOffset;
    return output;
}

struct PatchConstants
{
    float Edge[3] : SV_TessFactor;
    float Inside  : SV_InsideTessFactor;
};

float DistanceTessellation(float3 positionWorld)
{
    const float distanceToCamera = distance(positionWorld, gEyePosition);
    const float range = max(gTessFarDistance - gTessNearDistance, 0.001f);
    const float blend = saturate((distanceToCamera - gTessNearDistance) / range);
    return lerp(gMaxTessFactor, gMinTessFactor, blend);
}

PatchConstants PatchConstantsHS(InputPatch<ControlPoint, 3> patch)
{
    PatchConstants output;

    output.Edge[0] = DistanceTessellation((patch[1].PosW + patch[2].PosW) * 0.5f);
    output.Edge[1] = DistanceTessellation((patch[2].PosW + patch[0].PosW) * 0.5f);
    output.Edge[2] = DistanceTessellation((patch[0].PosW + patch[1].PosW) * 0.5f);
    output.Inside = (output.Edge[0] + output.Edge[1] + output.Edge[2]) / 3.0f;
    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstantsHS")]
[maxtessfactor(16.0f)]
ControlPoint HSGeometry(InputPatch<ControlPoint, 3> patch,
    uint controlPointId : SV_OutputControlPointID)
{
    return patch[controlPointId];
}

struct DSOut
{
    float4 PosH     : SV_POSITION;
    float3 PosW     : POSITION0;
    float3 NrmW     : NORMAL0;
    float2 TexCoord : TEXCOORD0;
    float4 TangentW : TANGENT0;
};

[domain("tri")]
DSOut DSGeometry(PatchConstants constants,
    float3 barycentric : SV_DomainLocation,
    const OutputPatch<ControlPoint, 3> patch)
{
    DSOut output;

    float3 positionWorld =
        patch[0].PosW * barycentric.x +
        patch[1].PosW * barycentric.y +
        patch[2].PosW * barycentric.z;
    float3 normalWorld = normalize(
        patch[0].NrmW * barycentric.x +
        patch[1].NrmW * barycentric.y +
        patch[2].NrmW * barycentric.z);
    float4 tangentWorld =
        patch[0].TangentW * barycentric.x +
        patch[1].TangentW * barycentric.y +
        patch[2].TangentW * barycentric.z;
    float2 texCoord =
        patch[0].TexCoord * barycentric.x +
        patch[1].TexCoord * barycentric.y +
        patch[2].TexCoord * barycentric.z;

    if(gEnableDisplacement != 0)
    {
        const float height = gDisplacementMap.SampleLevel(gWrapSampler, texCoord, 0.0f).r;
        positionWorld += normalWorld * (height * gDisplacementScale + gDisplacementBias);
    }

    tangentWorld.xyz = normalize(tangentWorld.xyz -
        normalWorld * dot(normalWorld, tangentWorld.xyz));
    tangentWorld.w = tangentWorld.w < 0.0f ? -1.0f : 1.0f;

    output.PosW = positionWorld;
    output.NrmW = normalWorld;
    output.TangentW = tangentWorld;
    output.TexCoord = texCoord;
    output.PosH = mul(float4(positionWorld, 1.0f), gViewProj);
    return output;
}

struct GBufferOutput
{
    float4 AlbedoSpecular  : SV_Target0;
    float4 NormalShininess : SV_Target1;
    float4 WorldPosition   : SV_Target2;
};

[earlydepthstencil]
GBufferOutput PSGeometry(DSOut input)
{
    const float4 texel = gDiffuseMap.Sample(gWrapSampler, input.TexCoord);
    const float alpha = texel.a * gDiffuse.a;
    clip(alpha - 0.05f);

    const float3 normalGeometry = normalize(input.NrmW);
    float3 normalWorld = normalGeometry;
    if(gEnableNormalMapping != 0)
    {
        const float3 tangent = normalize(input.TangentW.xyz -
            normalGeometry * dot(normalGeometry, input.TangentW.xyz));
        const float3 bitangent = normalize(cross(normalGeometry, tangent)) * input.TangentW.w;
        const float3 normalTangent = normalize(
            gNormalMap.Sample(gWrapSampler, input.TexCoord).xyz * 2.0f - 1.0f);
        normalWorld = normalize(
            mul(normalTangent, float3x3(tangent, bitangent, normalGeometry)));
    }

    GBufferOutput output;
    const float specularStrength = max(gSpecular.r, max(gSpecular.g, gSpecular.b));
    float3 albedo = texel.rgb * gDiffuse.rgb;
    if(gColorizeTiles != 0)
    {
        const int2 tileCoord = int2(floor(input.TexCoord * 4.0f));
        const uint2 tileBits = asuint(tileCoord);
        uint hash = tileBits.x * 73856093u ^ tileBits.y * 19349663u;
        hash ^= hash >> 13;
        hash *= 1274126177u;
        hash ^= hash >> 16;
        const float3 tileColor = 0.25f + 0.75f *
            float3(hash & 255u, (hash >> 8) & 255u, (hash >> 16) & 255u) / 255.0f;
        const float tileShade = dot(texel.rgb, float3(0.299f, 0.587f, 0.114f));
        albedo = tileColor * (0.45f + 0.55f * tileShade);
    }
    output.AlbedoSpecular = float4(albedo, specularStrength);
    output.NormalShininess = float4(normalWorld, saturate(gShininess / 256.0f));
    output.WorldPosition = float4(input.PosW, 1.0f);
    return output;
}
