// A pool's water surface: the lot's mirror image shifted by the ripples, a tint added, and see-through
// by how straight the view looks down on it, as the game's PoolWaterSurfaceMaterial draws it.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> reflection : register(t0, space2);
COMBINED SamplerState reflectionSampler : register(s0, space2);
COMBINED Texture2D<float4> bump : register(t1, space2);
COMBINED SamplerState bumpSampler : register(s1, space2);

struct Input {
    float4 position : SV_Position;
    float4 screen : TEXCOORD0;
    float4 waves01 : TEXCOORD1;
    float4 waves23 : TEXCOORD2;
    float3 eye : TEXCOORD3;
    float4 specular : TEXCOORD4;
    float4 tint : TEXCOORD5;  // rgb: added to the water, a: its murk (0 for pools)
};

float4 main(Input input) : SV_Target {
    const float3 eye = normalize(input.eye);
    const float3 sum = bump.Sample(bumpSampler, input.waves01.xy).xyz + bump.Sample(bumpSampler, input.waves01.zw).xyz +
                       bump.Sample(bumpSampler, input.waves23.xy).xyz + bump.Sample(bumpSampler, input.waves23.zw).xyz;
    const float3 ripple = normalize(2 * sum - 4) * float3(0.1, 0.1, 1);
    const float4 at = input.screen + float4(ripple.xy, 0, 0);
    float4 color = reflection.Sample(reflectionSampler, at.xy / at.w);
    const float facing = 1 - max(dot(eye, ripple), 0);
    color.a = max(0.2 + 0.8 * pow(facing, 5), 0);
    // Murky water (ponds drawn like pools, not in the original game) blurs its mirror image, more up
    // and down than across, turns it toward its own colour and hides more of what is under it.
    const float murk = input.tint.a;
    if (murk > 0) {
        static const float2 kDisk[12] = {float2(-0.33, 0.12), float2(0.21, -0.49), float2(0.62, 0.18), float2(-0.08, 0.71),
                                         float2(-0.71, -0.33), float2(0.12, 0.29), float2(0.44, 0.78), float2(-0.52, 0.58),
                                         float2(0.83, -0.37), float2(-0.27, -0.86), float2(-0.95, 0.09), float2(0.37, -0.02)};
        float3 blurred = 0;
        [unroll] for (int i = 0; i < 12; ++i) {
            blurred += reflection.Sample(reflectionSampler, at.xy / at.w + kDisk[i] * float2(0.006, 0.016) * murk).rgb;
        }
        color.rgb = blurred / 12;
    }
    color.rgb = lerp(color.rgb, input.tint.rgb * 2, murk);
    color.a = lerp(color.a, 1, murk * 1.2);
    return saturate(color + float4(input.tint.rgb, 0) * (1 - murk) + input.specular);
}
