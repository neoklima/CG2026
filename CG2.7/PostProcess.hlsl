Texture2D<float4> gSceneColor : register(t0);
SamplerState gPointClamp : register(s0);

struct FullscreenVertex
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenVertex VSPostProcess(uint vertexId : SV_VertexID)
{
    FullscreenVertex output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(
        output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
        0.0f, 1.0f);
    return output;
}

float4 PSCopy(FullscreenVertex input) : SV_Target
{
    return gSceneColor.SampleLevel(gPointClamp, input.uv, 0);
}

float4 PSVignette(FullscreenVertex input) : SV_Target
{
    const float radius = 0.35f;
    const float softness = 0.35f;
    const float strength = 0.65f;
    const float distanceFromCenter = distance(input.uv, float2(0.5f, 0.5f));
    const float edge = smoothstep(radius, radius + softness, distanceFromCenter);
    const float3 color = gSceneColor.SampleLevel(gPointClamp, input.uv, 0).rgb;
    return float4(color * (1.0f - strength * edge), 1.0f);
}

float4 PSGaussianBlur3x3(FullscreenVertex input) : SV_Target
{
    uint width;
    uint height;
    gSceneColor.GetDimensions(width, height);
    const float2 texelSize = 1.0f / float2(width, height);
    const float kernel[3][3] = {
        {1.0f, 2.0f, 1.0f},
        {2.0f, 4.0f, 2.0f},
        {1.0f, 2.0f, 1.0f}
    };
    float3 sum = 0.0f;
    [unroll]
    for(int y = -1; y <= 1; ++y)
    {
        [unroll]
        for(int x = -1; x <= 1; ++x)
        {
            const float2 sampleUv = input.uv + float2(x, y) * texelSize;
            sum += gSceneColor.SampleLevel(gPointClamp, sampleUv, 0).rgb * kernel[y + 1][x + 1];
        }
    }
    return float4(sum / 16.0f, 1.0f);
}
