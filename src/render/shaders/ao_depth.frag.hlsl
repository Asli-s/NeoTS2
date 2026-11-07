// Ambient occlusion's depth pass (not in the original game): cut-out surfaces leave out what the scene
// shader's alpha test leaves out, so their holes don't darken what is behind them.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> baseTexture : register(t0, space2);
COMBINED SamplerState baseSampler : register(s0, space2);

// The start of the scene shader's Material, field for field.
cbuffer Material : register(b0, space3) {
    float4 color;
    float4 sunDirection;
    float hasTexture;
    float alphaCutoff;
    float unlit;
    float alphaReplicate;
    float4 lightMap;
    float4 pixelLightColors[3];
    float pixelLightCount;
    float normalMapped;
    float simLighting;
    float lightMapFade;
    float4 envCube;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float4 emissive;
    float4 uvTile;
};

// What the scene's vertex shaders give the scene shader.
struct Input {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 world : TEXCOORD2;
    float alpha : TEXCOORD3;
    float4 light : TEXCOORD4;
    float2 lightUv : TEXCOORD5;
    float3 color : TEXCOORD6;
    float3 lightVector0 : TEXCOORD7;
    float3 lightVector1 : TEXCOORD8;
    float3 lightVector2 : TEXCOORD9;
    float3 reflection : TEXCOORD10;
    float3 specular : TEXCOORD11;
};

void main(Input input) {
    if (alphaCutoff > 0) {
        const float texel = hasTexture > 0.5 ? baseTexture.Sample(baseSampler, input.uv * uvTile.xy + uvTile.zw).a : 1;
        if (color.a * texel * input.alpha < alphaCutoff) {
            discard;
        }
    }
}
