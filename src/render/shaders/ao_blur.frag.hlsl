// Ambient occlusion's blur (not in the original game): the average over the 4 by 4 pixels its sample
// pattern repeats in, of those at about the same depth, so it stays on its own side of an edge.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float> occlusionTexture : register(t0, space2);
COMBINED SamplerState occlusionSampler : register(s0, space2);
COMBINED Texture2D<float> depthTexture : register(t1, space2);
COMBINED SamplerState depthSampler : register(s1, space2);

float main(float4 position : SV_Position) : SV_Target {
    const int2 pixel = int2(position.xy);
    uint width = 0, height = 0;
    depthTexture.GetDimensions(width, height);
    const int2 last = int2(width, height) - 1;
    const float here = depthTexture.Load(int3(pixel, 0));
    float sum = 0, all = 0, count = 0;
    for (int y = -2; y < 2; ++y) {
        for (int x = -2; x < 2; ++x) {
            const int2 at = clamp(pixel + int2(x, y), int2(0, 0), last);
            const float depth = depthTexture.Load(int3(at, 0));
            const float open = occlusionTexture.Load(int3(at, 0));
            all += open;
            // Depth goes as one over the distance, so this compares distances by their ratio.
            if (abs(depth - here) <= 0.03 * here) {
                sum += open;
                count += 1;
            }
        }
    }
    // A pixel nearly alone at its depth looks through a crack between two meshes that the main view
    // covers with a layer, so it takes what is around it instead of what is behind the crack.
    return count > 2 ? sum / count : all / 16;
}
