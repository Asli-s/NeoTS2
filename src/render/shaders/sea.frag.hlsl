// A beach lot's sea: its mirror image shifted by the ripples, a tint added, and see-through by how
// straight the view looks down on it plus the water's depth, as the game's lotSkirtSea draws it.

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
    float4 tint : TEXCOORD4;  // rgb: added to the water, a: added to its opacity
    float fade : TEXCOORD5;   // scales its opacity, clamped to 0..1: 1, but less for a sea fading at the shore (not in the original game)
    float4 scale : TEXCOORD6; // rgb: what the mirror image is multiplied by, a: opacity added (not in the original game)
    float blur : TEXCOORD7;   // how much the ripples shift and blur the mirror image across the screen (not in the original game)
};

float4 main(Input input) : SV_Target {
    const float3 eye = normalize(input.eye);
    const float3 sum = bump.Sample(bumpSampler, input.waves01.xy).xyz + bump.Sample(bumpSampler, input.waves01.zw).xyz +
                       bump.Sample(bumpSampler, input.waves23.xy).xyz + bump.Sample(bumpSampler, input.waves23.zw).xyz;
    const float3 ripple = normalize(2 * sum - 4);
    const float4 at = input.screen + float4(ripple.xy, 0, 0);
    float4 color = reflection.Sample(reflectionSampler, at.xy / at.w);
    // A neighborhood's pool-style sea is seen from far off, where the shift above comes to almost
    // nothing. Its ripples shift its mirror image by a share of the screen instead and blur it, as
    // murky ponds drawn like pools blur theirs.
    if (input.blur > 0) {
        static const float2 kDisk[12] = {float2(-0.33, 0.12), float2(0.21, -0.49), float2(0.62, 0.18), float2(-0.08, 0.71),
                                         float2(-0.71, -0.33), float2(0.12, 0.29), float2(0.44, 0.78), float2(-0.52, 0.58),
                                         float2(0.83, -0.37), float2(-0.27, -0.86), float2(-0.95, 0.09), float2(0.37, -0.02)};
        const float2 shifted = at.xy / at.w + ripple.xy * 0.02 * input.blur;
        float3 blurred = 0;
        [unroll] for (int i = 0; i < 12; ++i) {
            blurred += reflection.Sample(reflectionSampler, shifted + kDisk[i] * float2(0.006, 0.016) * input.blur).rgb;
        }
        color.rgb = blurred / 12;
    }
    color.rgb *= input.scale.rgb;
    const float facing = 1 - max(dot(eye, ripple), 0);
    color.a = max(0.1 + 0.9 * pow(facing, 0.5), 0) * 0.5;
    color = saturate(color + input.tint);
    color.a = saturate(color.a + input.scale.a) * saturate(input.fade);
    return color;
}
