// Ambient occlusion's depth at half the view's size (not in the original game). Each pixel keeps the
// nearest of the 2 by 2 depths it stands for, or the farthest, in turn like a checkerboard. So both sides
// of an edge, and thin things such as fence posts, keep pixels at their own depth wherever they move.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float> depthTexture : register(t0, space2);
COMBINED SamplerState depthSampler : register(s0, space2);

float main(float4 position : SV_Position) : SV_Target {
    const int2 pixel = int2(position.xy);
    uint width = 0, height = 0;
    depthTexture.GetDimensions(width, height);
    const int2 last = int2(width, height) - 1;
    const int2 at = pixel * 2;
    const float a = depthTexture.Load(int3(min(at, last), 0));
    const float b = depthTexture.Load(int3(min(at + int2(1, 0), last), 0));
    const float c = depthTexture.Load(int3(min(at + int2(0, 1), last), 0));
    const float d = depthTexture.Load(int3(min(at + int2(1, 1), last), 0));
    // Depth grows toward the camera.
    return ((pixel.x + pixel.y) & 1) != 0 ? max(max(a, b), max(c, d)) : min(min(a, b), min(c, d));
}
