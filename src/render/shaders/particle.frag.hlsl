// A particle's pixel: its texture times its colour, as the game's StandardParticlesMaterial draws it.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> image : register(t0, space2);
COMBINED SamplerState imageSampler : register(s0, space2);

cbuffer Particle : register(b0, space3) {
    float alphaReference;  // pixels with less alpha aren't drawn
    float3 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

float4 main(Input input) : SV_Target {
    float4 color = image.Sample(imageSampler, input.uv) * input.color;
    if (color.a < alphaReference) {
        discard;
    }
    return color;
}
