// The water in a lot's ponds: the game draws two passes over what is below. The first is the sky
// cube's reflection times the texture light, at the vertex's alpha. The second is the two ripple
// images multiplied, at 0.3. Both are given here as one colour, premultiplied, for a
// one / one-minus-source-alpha blend.

#include "sdl_gpu.hlsli"

COMBINED TextureCube<float4> sky : register(t0, space2);
COMBINED SamplerState skySampler : register(s0, space2);
COMBINED Texture2D<float4> ripple0 : register(t1, space2);
COMBINED SamplerState ripple0Sampler : register(s1, space2);
COMBINED Texture2D<float4> ripple1 : register(t2, space2);
COMBINED SamplerState ripple1Sampler : register(s2, space2);

cbuffer Pond : register(b0, space3) {
    float4 light;  // rgb: the outdoors' texture light
};

struct Input {
    float4 position : SV_Position;
    float4 ripples : TEXCOORD0;
    float3 reflection : TEXCOORD1;
    float alpha : TEXCOORD2;
};

float4 main(Input input) : SV_Target {
    const float3 reflected = sky.Sample(skySampler, input.reflection).rgb * light.rgb;
    const float3 rippled = ripple0.Sample(ripple0Sampler, input.ripples.xy).rgb * ripple1.Sample(ripple1Sampler, input.ripples.zw).rgb;
    const float a = saturate(input.alpha);
    const float kRipple = 0.3;
    return float4(kRipple * rippled + (1 - kRipple) * a * reflected, 1 - (1 - kRipple) * (1 - a));
}
