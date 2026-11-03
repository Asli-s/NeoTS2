// The water in a lot's ponds as the game's waterOnTerrain material draws it: the sky cube seen along
// the view reflected in the flat water, and two ripple images sliding against each other.

cbuffer Pond : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
    row_major float4x4 model;
    float4 eye;     // xyz: the camera's position
    float4 light;   // rgb: the outdoors' texture light
    float4 ripple;  // x: seconds, y: the slide's turn rate (radians a second), z: repeats per tile
    float4 padding;
};

#include "sdl_gpu.hlsli"

struct Input {
    LOCATION(0) float3 position : TEXCOORD0;
    LOCATION(1) float3 normal : TEXCOORD1;
    LOCATION(2) float2 uv : TEXCOORD2;
    LOCATION(3) float alpha : TEXCOORD3;
};

struct Output {
    float4 position : SV_Position;
    float4 ripples : TEXCOORD0;     // xy: the first image's coordinates, zw: the second's
    float3 reflection : TEXCOORD1;  // the view reflected in the water, in the world
    float alpha : TEXCOORD2;
};

Output main(Input input) {
    Output output;
    const float3 world = mul(model, float4(input.position, 1)).xyz;
    output.position = mul(viewProjection, float4(world, 1));

    const float3 up = normalize(mul((float3x3)model, float3(0, 0, 1)));
    const float3 toEye = normalize(eye.xyz - world);
    output.reflection = 2 * up * dot(up, toEye) - toEye;

    // The first image slides along u from 0 to 1 and back in a sine, the second the other way.
    const float slide = 0.5 + 0.5 * sin(ripple.x * ripple.y);
    output.ripples = float4((input.uv + float2(slide, 0)) * ripple.z, (input.uv + float2(1 - slide, 0)) * ripple.z);
    output.alpha = input.alpha;
    return output;
}
