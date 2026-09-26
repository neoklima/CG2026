struct Particle
{
    float3 position;
    float age;
    float3 velocity;
    float lifetime;
};

cbuffer ParticleFrameCB : register(b0)
{
    float4x4 gViewProj;
    float4 gCameraRight;
    float4 gCameraUp;
    float4 gEmitterAndDelta;
};

ConsumeStructuredBuffer<Particle> gInputParticles : register(u0);
AppendStructuredBuffer<Particle> gOutputParticles : register(u1);
StructuredBuffer<Particle> gDrawParticles : register(t0);

float Random01(float2 seed)
{
    return frac(sin(dot(seed, float2(12.9898f, 78.233f))) * 43758.5453f);
}

[numthreads(64, 1, 1)]
void CSUpdate(uint3 dispatchId : SV_DispatchThreadID)
{
    if(dispatchId.x >= 128)
        return;

    Particle particle = gInputParticles.Consume();
    const float delta = min(gEmitterAndDelta.w, 0.05f);
    particle.age += delta;
    if(particle.age >= particle.lifetime)
    {
        const float first = Random01(float2(dispatchId.x, particle.position.x + particle.age));
        const float second = Random01(float2(dispatchId.x + 17.0f, particle.position.z));
        const float angle = first * 6.2831853f;
        const float radius = 0.12f + second * 0.38f;
        particle.position = gEmitterAndDelta.xyz +
            float3(cos(angle) * radius, 0.0f, sin(angle) * radius);
        particle.velocity = float3(
            cos(angle) * (0.25f + second * 0.4f),
            2.0f + first * 1.2f,
            sin(angle) * (0.25f + second * 0.4f));
        particle.age = 0.0f;
        particle.lifetime = 1.7f + second * 0.7f;
    }
    else
    {
        particle.velocity.y -= 0.8f * delta;
        particle.position += particle.velocity * delta;
    }
    gOutputParticles.Append(particle);
}

struct ParticlePoint
{
    float3 position : POSITION;
    float age : TEXCOORD0;
    float lifetime : TEXCOORD1;
};

ParticlePoint VSParticle(uint vertexId : SV_VertexID)
{
    const Particle particle = gDrawParticles[vertexId];
    ParticlePoint output;
    output.position = particle.position;
    output.age = particle.age;
    output.lifetime = particle.lifetime;
    return output;
}

struct BillboardVertex
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 color : COLOR0;
};

[maxvertexcount(4)]
void GSParticle(point ParticlePoint input[1], inout TriangleStream<BillboardVertex> stream)
{
    const float life = saturate(input[0].age / input[0].lifetime);
    const float size = lerp(0.10f, 0.035f, life);
    const float2 corners[4] = {
        float2(-1.0f, -1.0f), float2(-1.0f, 1.0f),
        float2(1.0f, -1.0f), float2(1.0f, 1.0f)
    };
    BillboardVertex output;
    output.color = lerp(float3(1.0f, 0.82f, 0.22f), float3(1.0f, 0.28f, 0.04f), life);
    for(uint index = 0; index < 4; ++index)
    {
        output.uv = corners[index];
        const float3 worldPosition = input[0].position +
            (gCameraRight.xyz * corners[index].x + gCameraUp.xyz * corners[index].y) * size;
        output.position = mul(float4(worldPosition, 1.0f), gViewProj);
        stream.Append(output);
    }
}

float4 PSParticle(BillboardVertex input) : SV_Target
{
    clip(1.0f - dot(input.uv, input.uv));
    return float4(input.color, 1.0f);
}
