#define MAX_LIGHTS 32
#define SHADOW_CASCADES 3

#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

struct Light
{
    float3 Position;
    float  Range;
    float3 Direction;
    float  SpotCosOuter;
    float3 Color;
    float  Intensity;
    float  SpotCosInner;
    uint   Type;
    float2 Padding;
};

cbuffer LightingCB : register(b0)
{
    float3 gEyePosition;
    float  gAmbientIntensity;
    float3 gAmbientColor;
    uint   gLightCount;
    float3 gBackgroundColor;
    float  _padding;
    Light  gLights[MAX_LIGHTS];
    float4x4 gShadowViewProj[SHADOW_CASCADES];
    float4 gCascadeSplits;
    float4 gCameraForwardAndShadow;
};

Texture2D<float4> gAlbedoSpecular  : register(t0);
Texture2D<float4> gNormalShininess : register(t1);
Texture2D<float4> gWorldPosition   : register(t2);
Texture2DArray<float> gShadowMaps  : register(t3);
SamplerComparisonState gShadowSampler : register(s0);

struct FullscreenVertex
{
    float4 Position : SV_POSITION;
};

FullscreenVertex VSFullscreen(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float DistanceAttenuation(float distanceToLight, float range)
{
    const float normalizedDistance = saturate(distanceToLight / max(range, 0.001f));
    const float smoothFalloff = 1.0f - normalizedDistance * normalizedDistance;
    return smoothFalloff * smoothFalloff / (1.0f + 0.06f * distanceToLight);
}

float DirectionalShadow(float3 positionWorld)
{
    if(gCameraForwardAndShadow.w < 0.5f)
        return 1.0f;

    const float cameraDepth = dot(positionWorld - gEyePosition, gCameraForwardAndShadow.xyz);
    uint cascade = cameraDepth > gCascadeSplits.x ? 1u : 0u;
    cascade = cameraDepth > gCascadeSplits.y ? 2u : cascade;
    const float4 lightPosition = mul(float4(positionWorld, 1.0f), gShadowViewProj[cascade]);
    const float3 projected = lightPosition.xyz / lightPosition.w;
    const float2 uv = float2(projected.x * 0.5f + 0.5f, 0.5f - projected.y * 0.5f);
    if(any(uv < 0.0f) || any(uv > 1.0f) || projected.z < 0.0f || projected.z > 1.0f)
        return 1.0f;

    float visibility = 0.0f;
    [unroll]
    for(int y = -1; y <= 1; ++y)
    {
        [unroll]
        for(int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(x, y) / 1024.0f;
            visibility += gShadowMaps.SampleCmpLevelZero(
                gShadowSampler, float3(uv + offset, cascade), projected.z - 0.001f);
        }
    }
    return visibility / 9.0f;
}

float4 PSLighting(FullscreenVertex input) : SV_Target
{
    const int2 pixel = int2(input.Position.xy);
    const float4 worldData = gWorldPosition.Load(int3(pixel, 0));
    if(worldData.a < 0.5f)
        return float4(gBackgroundColor, 1.0f);

    const float4 albedoSpecular = gAlbedoSpecular.Load(int3(pixel, 0));
    const float4 normalShininess = gNormalShininess.Load(int3(pixel, 0));

    const float3 positionWorld = worldData.xyz;
    const float3 normalWorld = normalize(normalShininess.xyz);
    const float shininess = max(normalShininess.a * 256.0f, 1.0f);
    const float3 viewDirection = normalize(gEyePosition - positionWorld);

    float3 result = albedoSpecular.rgb * gAmbientColor * gAmbientIntensity;

    [loop]
    for(uint lightIndex = 0; lightIndex < min(gLightCount, (uint)MAX_LIGHTS); ++lightIndex)
    {
        const Light light = gLights[lightIndex];
        float3 lightDirection = 0.0f;
        float attenuation = 1.0f;

        if(light.Type == LIGHT_DIRECTIONAL)
        {
            lightDirection = normalize(-light.Direction);
        }
        else
        {
            const float3 toLight = light.Position - positionWorld;
            const float distanceToLight = length(toLight);
            if(distanceToLight >= light.Range)
                continue;

            lightDirection = toLight / max(distanceToLight, 0.0001f);
            attenuation = DistanceAttenuation(distanceToLight, light.Range);

            if(light.Type == LIGHT_SPOT)
            {
                const float3 lightToPixel = -lightDirection;
                const float coneCosine = dot(lightToPixel, normalize(light.Direction));
                attenuation *= smoothstep(light.SpotCosOuter, light.SpotCosInner, coneCosine);
            }
        }

        const float nDotL = saturate(dot(normalWorld, lightDirection));
        if(nDotL <= 0.0f || attenuation <= 0.0f)
            continue;

        const float3 halfVector = normalize(lightDirection + viewDirection);
        const float specular = pow(saturate(dot(normalWorld, halfVector)), shininess) * albedoSpecular.a;
        const float shadow = light.Type == LIGHT_DIRECTIONAL
            ? DirectionalShadow(positionWorld) : 1.0f;
        const float3 radiance = light.Color * light.Intensity * attenuation * shadow;
        result += (albedoSpecular.rgb * nDotL + specular) * radiance;
    }

    return float4(result, 1.0f);
}
