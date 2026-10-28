// A lot object's picture from the sun blurred into its tile of the atlas: the scratch picture it was
// drawn into, shrunk to the picture's size, with the texels around each point added up as the game
// weighs its blur's copies. Past the picture's edge its edge texels stand in.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> scratch : register(t0, space2);
COMBINED SamplerState scratchSampler : register(s0, space2);

cbuffer Blur : register(b0, space3) {
    float4 weights[25];  // x: by (across + 2) * 5 + (up + 2)
    float4 tile;         // xy: the tile's corner in the atlas, z: the picture's size, in texels
};

float4 main(float4 position : SV_Position) : SV_Target {
    const float side = 128;  // the scratch picture's size
    const float2 at = (position.xy - tile.xy) / tile.z;
    float sum = 0;
    [unroll] for (int across = -2; across <= 2; ++across) {
        [unroll] for (int up = -2; up <= 2; ++up) {
            const float2 uv = clamp(at + float2(across, up) / side, 0.5 / side, 1 - 0.5 / side);
            sum += scratch.SampleLevel(scratchSampler, uv, 0).a * weights[(across + 2) * 5 + up + 2].x;
        }
    }
    return float4(0, 0, 0, sum);
}
