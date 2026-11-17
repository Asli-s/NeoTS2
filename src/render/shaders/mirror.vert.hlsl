// A mirror's glass: placed as the scene's vertices are, so it lands on the same pixels and depth.

cbuffer Mirror : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
    row_major float4x4 model;           // the instance's placement
};

#include "sdl_gpu.hlsli"

struct Input {
    LOCATION(0) float3 position : TEXCOORD0;
};

float4 main(Input input) : SV_Position {
    const float4 world = mul(model, float4(input.position, 1));
    return mul(viewProjection, world);
}
