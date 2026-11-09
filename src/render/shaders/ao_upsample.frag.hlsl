// Ambient occlusion brought up to the view's size (not in the original game). Each half-size pixel stands
// for 2 by 2 of the view's, so a view pixel lies a quarter or three quarters of the way between the four
// nearest. It blends those at about its own depth by how near they are, so it stays on its own side of
// an edge. When none is, it takes the one nearest its depth.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float> occlusionTexture : register(t0, space2);
COMBINED SamplerState occlusionSampler : register(s0, space2);
COMBINED Texture2D<float> halfDepthTexture : register(t1, space2);
COMBINED SamplerState halfDepthSampler : register(s1, space2);
COMBINED Texture2D<float> depthTexture : register(t2, space2);
COMBINED SamplerState depthSampler : register(s2, space2);

float main(float4 position : SV_Position) : SV_Target {
    const int2 pixel = int2(position.xy);
    uint width = 0, height = 0;
    occlusionTexture.GetDimensions(width, height);
    const int2 last = int2(width, height) - 1;
    const float here = depthTexture.Load(int3(pixel, 0));
    const int2 first = (pixel - 1) >> 1;
    const float2 between = lerp(float2(0.75, 0.75), float2(0.25, 0.25), float2(pixel & 1));
    float sum = 0, weight = 0, nearest = 1, nearestGap = 1e30;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const int2 at = clamp(first + int2(x, y), int2(0, 0), last);
            const float open = occlusionTexture.Load(int3(at, 0));
            const float gap = abs(halfDepthTexture.Load(int3(at, 0)) - here);
            // Depth goes as one over the distance, so this compares distances by their ratio.
            if (gap <= 0.03 * here) {
                const float near = (x ? between.x : 1 - between.x) * (y ? between.y : 1 - between.y);
                sum += open * near;
                weight += near;
            }
            if (gap < nearestGap) {
                nearestGap = gap;
                nearest = open;
            }
        }
    }
    return weight > 0 ? sum / weight : nearest;
}
