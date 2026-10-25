cbuffer ShaderConstants : register(b0)
{
    float u_Time;
    float u_BassIntensity;
    float u_BassHit;
    float u_Width;
    float u_Height;
    uint u_CelShading;
    uint u_ClickThrough;
    float u_Feedback;
    uint u_BeatIndex;
    uint u_BeatCount;
    float2 u_Padding;
    float4 u_BeatAges[2];
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
    VertexOutput output;
    float2 position = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = position;
    output.position = float4(position * float2(2.0, -2.0) + float2(-1.0, 1.0),
                              0.0, 1.0);
    return output;
}

float randomValue(float2 value)
{
    return frac(sin(dot(value, float2(12.9898, 78.233))) * 43758.5453);
}

float4 DrawBlocks(float2 pixel, float strength, float glowStrength, uint beatId)
{
    float activity = saturate(strength * 1.2);
    if (activity < 0.008)
        return 0.0;

    float4 result = 0.0;
    float beatSalt = (float)beatId * 17.17;
    int leftCount = 3 + (int)floor(randomValue(float2(beatSalt, 31.7)) * 9.0);
    int rightCount = 3 + (int)floor(randomValue(float2(beatSalt, 47.3)) * 9.0);

    [unroll]
    for (int i = 0; i < 11; ++i)
    {
        [unroll]
        for (int sideIndex = 0; sideIndex < 2; ++sideIndex)
        {
            int lineCount = (sideIndex == 0) ? leftCount : rightCount;
            if (i < lineCount)
            {
                float side = (sideIndex == 0) ? 1.0 : -1.0;
                float sideDistance = (side > 0.0) ? pixel.x : u_Width - pixel.x;
                float seed = (float)i * 13.71 + (float)sideIndex * 173.9 + 4.3;
                float anchor = lerp(0.08, 0.92,
                                    randomValue(float2(seed, beatSalt + 7.1)));
                float centerY = anchor * u_Height;
                float lengthFraction = lerp(0.10, 0.75,
                                            randomValue(float2(seed, beatSalt + 13.9)));
                float depth = u_Width * lengthFraction;
                bool thick = (sideIndex == 0 && i < 2) ||
                             (sideIndex == 1 && i == 0);
                float span = thick
                    ? lerp(8.0, 25.0, randomValue(float2(seed, beatSalt + 11.3)))
                    : 3.0;
                float alongDepth = saturate(sideDistance / max(depth, 1.0));
                float insideDepth = step(0.0, sideDistance) * step(sideDistance, depth);
                float taperedHeight = lerp(span, 3.0, alongDepth);
                float trimFromTop =
                    step(0.5, randomValue(float2(seed, beatSalt + 19.7)));
                float top = centerY + (1.0 - trimFromTop) * (span - taperedHeight);
                float bottom = top + taperedHeight;
                float inside = insideDepth * step(top, pixel.y) * step(pixel.y, bottom);
                float distanceX = max(max(-sideDistance, sideDistance - depth), 0.0);
                float distanceY = max(max(top - pixel.y, pixel.y - bottom), 0.0);
                float lineDistance = length(float2(distanceX, distanceY));
                float glow = (1.0 - smoothstep(6.0, 35.0, lineDistance)) *
                             glowStrength * 0.16;
                float outline = inside * (1.0 - smoothstep(
                    1.0, 3.0, min(depth - sideDistance,
                                  min(pixel.y - top, bottom - pixel.y))));
                float opacity = saturate((inside * 0.52 + outline * 0.2) * activity);

                float hue = randomValue(float2(seed, 5.3));
                float3 color = lerp(float3(0.58, 0.035, 0.96),
                                    float3(0.015, 0.28, 1.0), hue);
                if (u_CelShading != 0)
                {
                    color = floor(color * 4.0) / 4.0;
                    float segmented =
                        step(0.55, frac(sideDistance / 18.0 + seed)) *
                        step(0.45, frac(pixel.y / 7.0 + seed * 0.17));
                    opacity *= max(0.25, 0.48 + outline * 0.52 + segmented * 0.24);
                    color = lerp(color, float3(0.08, 0.82, 1.0), outline * 0.95);
                }

                float contribution = saturate(opacity);
                float combinedGlow = saturate(glow);
                result.rgb = color * combinedGlow +
                             result.rgb * (1.0 - combinedGlow);
                result.a = combinedGlow + result.a * (1.0 - combinedGlow);
                result.rgb = color * contribution +
                             result.rgb * (1.0 - contribution);
                result.a = contribution + result.a * (1.0 - contribution);
            }
        }
    }
    return result;
}

float4 PSMain(VertexOutput input) : SV_TARGET
{
    float2 pixel = input.uv * float2(u_Width, u_Height);
    float bass = smoothstep(0.012, 0.24, u_BassIntensity);
    float hit = saturate(u_BassHit);
    float4 blocks = 0.0;
    float currentHit = hit;
    [unroll]
    for (int beatSlot = 0; beatSlot < 8; ++beatSlot)
    {
    if ((uint)beatSlot < u_BeatCount)
    {
        float beatAge = u_BeatAges[beatSlot / 4][beatSlot % 4];
        float beatEnvelope = 1.0 - smoothstep(0.0, 0.15, beatAge);
        float lineStrength = beatEnvelope;
        float glowStrength = beatEnvelope;
        uint beatId = u_BeatIndex - (uint)beatSlot;
        float4 layer = DrawBlocks(pixel, lineStrength, glowStrength, beatId);
        blocks.rgb = layer.rgb + blocks.rgb * (1.0 - layer.a);
        blocks.a = layer.a + blocks.a * (1.0 - layer.a);
        if (beatSlot == 0)
            currentHit = max(currentHit, glowStrength);
    }
    }
    if (u_BeatCount == 0 && hit > 0.008)
    {
    blocks = DrawBlocks(pixel, hit, hit, u_BeatIndex);
    }

    float sideDistance = min(pixel.x, u_Width - pixel.x);
    float edgeStrength = 1.0 - smoothstep(u_Width * 0.1, u_Width * 0.5,
                                          sideDistance);
    float edgeOpacity = lerp(0.05, 0.35, edgeStrength);
    float blockAlpha = edgeOpacity * saturate(blocks.a * 2.5);
    float blockScale = blockAlpha / max(blocks.a, 0.0001);
    blocks.rgb *= blockScale;
    blocks.a = blockAlpha;
    float sideGlow = (1.0 - smoothstep(0.0, 26.0, sideDistance)) *
                     currentHit * 0.05;
    float3 color = blocks.rgb + float3(0.08, 0.72, 1.0) * sideGlow;
    float alpha = min(0.35, blocks.a + sideGlow);

    if (u_Feedback > 0.0 && pixel.x < 190.0 && pixel.y < 42.0)
    {
        float2 local = pixel - float2(16.0, 12.0);
        float control = step(0.0, local.x) * step(local.x, 150.0) *
                        step(0.0, local.y) * step(local.y, 22.0);
        float marker = step(0.0, local.x) *
                       (step(local.x, 5.0) + step(8.0, local.x) * step(local.x, 13.0));
        float3 statusColor = u_ClickThrough != 0
            ? float3(0.0, 0.85, 1.0)
            : float3(0.8, 0.15, 1.0);
        float statusAlpha = control * u_Feedback *
                            saturate(marker + 0.12 * step(17.0, local.y));
        color = statusColor * statusAlpha + color * (1.0 - statusAlpha);
        alpha = statusAlpha + alpha * (1.0 - statusAlpha);
    }

    return float4(color, alpha);
}
