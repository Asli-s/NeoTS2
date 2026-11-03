// The pools' caustics tile: one frame of the causticsTiled flipbook over the whole tile, times the
// time of day's brightness, as the game's CausticsGeneratorMaterial draws it each frame.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> flipbook : register(t0, space2);
COMBINED SamplerState flipbookSampler : register(s0, space2);

cbuffer Caustics : register(b0, space3) {
    float4 frame;  // xy: the frame's corner in the flipbook, zw: one frame's size
    float4 color;
};

struct Input {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

float4 main(Input input) : SV_Target {
    const float2 uv = float2(input.ndc.x, -input.ndc.y) * 0.5 + 0.5;
    return flipbook.Sample(flipbookSampler, frame.xy + uv * frame.zw) * color;
}
