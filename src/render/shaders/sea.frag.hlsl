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
    float fade : TEXCOORD5;   // scales its opacity: 1, but less for a sea fading at the shore (not in the original game)
};

float4 main(Input input) : SV_Target {
    const float3 eye = normalize(input.eye);
    const float3 sum = bump.Sample(bumpSampler, input.waves01.xy).xyz + bump.Sample(bumpSampler, input.waves01.zw).xyz +
                       bump.Sample(bumpSampler, input.waves23.xy).xyz + bump.Sample(bumpSampler, input.waves23.zw).xyz;
    const float3 ripple = normalize(2 * sum - 4);
    const float4 at = input.screen + float4(ripple.xy, 0, 0);
    float4 color = reflection.Sample(reflectionSampler, at.xy / at.w);
    const float facing = 1 - max(dot(eye, ripple), 0);
    color.a = max(0.1 + 0.9 * pow(facing, 0.5), 0) * 0.5;
    color = saturate(color + input.tint);
    color.a *= input.fade;
    return color;
}
