// A neighborhood road, as the game's road material draws it: the road texture (its alpha blending
// it over the terrain), times the terrain's light map at the same spot.

#include "sdl_gpu.hlsli"

#ifdef GL2
COMBINED Texture2D<float4> roads : register(t0, space2);  // OpenGL 2 has no texture arrays: one road texture per draw
#else
COMBINED Texture2DArray<float4> roads : register(t0, space2);
#endif
COMBINED SamplerState roadSampler : register(s0, space2);
COMBINED Texture2D<float4> lightMap : register(t1, space2);
COMBINED SamplerState lightSampler : register(s1, space2);

cbuffer Road : register(b0, space3) {
    float lightMapSize;  // world units the light map covers
    float3 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float textureLayer : TEXCOORD1;
    float2 ground : TEXCOORD2;
};

float4 main(Input input) : SV_Target {
#ifdef GL2
    float4 color = roads.Sample(roadSampler, input.uv);
#else
    float4 color = roads.Sample(roadSampler, float3(input.uv, round(input.textureLayer)));
#endif
    color.rgb *= lightMap.Sample(lightSampler, input.ground / lightMapSize).rgb;
    return color;
}
