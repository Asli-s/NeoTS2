// Ambient occlusion's depth pass for surfaces without holes (not in the original game): depth and the
// drawn normal for ao.frag. A normal worked out from depth would show a low-poly surface's faces as
// flat steps.

#include "sdl_gpu.hlsli"

// The first of what the scene's vertex shaders give the scene shader.
struct Input {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
};

float4 main(Input input) : SV_Target {
    const float length2 = dot(input.normal, input.normal);
    return length2 > 1e-12 ? float4(input.normal * rsqrt(length2) * 0.5 + 0.5, 1) : float4(0, 0, 0, 0);
}
