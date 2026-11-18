// A mirror's glass shows its mirror image as it is, unlit, as the game's MirrorReflectionMaterial
// does. The image was drawn flipped left to right at the screen's size, so each pixel reads the one
// across the screen from it.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> reflection : register(t0, space2);
COMBINED SamplerState reflectionSampler : register(s0, space2);

float4 main(float4 position : SV_Position) : SV_Target {
    uint width, height;
    reflection.GetDimensions(width, height);
    const int2 pixel = int2(position.xy);
    return float4(reflection.Load(int3(int(width) - 1 - pixel.x, pixel.y, 0)).rgb, 1);
}
