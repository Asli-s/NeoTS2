// The Sims' shadow pictures blurred, each within its own tile of the atlas: the texels around each
// one added up, weighed as the game weighs its blur's copies, 2 texels each way outdoors and 3 indoors. Past a tile's edge its edge texels
// stand in, as the game's own picture is clamped.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> pictures : register(t0, space2);
COMBINED SamplerState picturesSampler : register(s0, space2);

cbuffer Blur : register(b0, space3) {
    float4 weights[2][49];  // x: by (across + 3) * 7 + (up + 3), outdoors (reaching 2) then indoors (3)
    float4 indoor[16];      // x: 1 when the tile's caster is indoors
};

float4 main(float4 position : SV_Position) : SV_Target {
    const float tile = 128, atlas = 512;
    const float2 texel = floor(position.xy);
    const float2 corner = floor(texel / tile) * tile;
    const int k = indoor[(int)(corner.y / tile) * 4 + (int)(corner.x / tile)].x > 0.5 ? 1 : 0;
    float sum = 0;
    [unroll] for (int across = -3; across <= 3; ++across) {
        [unroll] for (int up = -3; up <= 3; ++up) {
            const float2 at = clamp(texel + float2(across, up), corner, corner + tile - 1);
            sum += pictures.SampleLevel(picturesSampler, (at + 0.5) / atlas, 0).a * weights[k][(across + 3) * 7 + up + 3].x;
        }
    }
    return float4(0, 0, 0, sum);
}
