// The neighborhood water as the game's reflective water material draws it: the reflection where
// the pixel is on screen and again a little off, both tinted, mixed by the ripple map's alpha
// (scaled and sliding along u over time), blended over what is below at the reflection strength.
// The game's reflection image is mirrored left to right (ours is not), so its offset to the right
// is an offset to the left here.

Texture2D<float4> reflection : register(t0, space2);
SamplerState reflectionSampler : register(s0, space2);
Texture2D<float4> ripples : register(t1, space2);
SamplerState rippleSampler : register(s1, space2);

cbuffer Water : register(b0, space3) {
    float4 tint;       // rgb: the reflection colour, a: the reflection strength
    float2 screen;     // the target's size in pixels
    float offset;      // the second sample's shift
    float scale;       // ripple map repeats per tile
    float speed;       // ripple map slide, in repeats per second
    float time;        // seconds
    float2 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 tiles : TEXCOORD0;
};

float4 main(Input input) : SV_Target {
    const float2 at = input.position.xy / screen;
    const float3 first = reflection.Sample(reflectionSampler, at).rgb * tint.rgb;
    const float3 second = reflection.Sample(reflectionSampler, at + float2(-offset, offset)).rgb * tint.rgb;
    const float slide = frac(time * speed / scale) * scale;
    const float a = ripples.Sample(rippleSampler, input.tiles * scale + float2(slide, 0)).a;
    return float4(first * a + second * (1 - a), tint.a);
}
