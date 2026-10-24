// The sky: one triangle covering the screen. The fragment shader works out each pixel's direction.

#include "sdl_gpu.hlsli"

struct Output {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

#ifdef GL2
// OpenGL 2 has no vertex number to work from: the corners (0,0) (2,0) (0,2) come in a buffer.
Output main(LOCATION(0) float2 corner : TEXCOORD0) {
#else
Output main(uint id : SV_VertexID) {
    const float2 corner = float2((id << 1) & 2, id & 2);  // (0,0) (2,0) (0,2)
#endif
    Output output;
    output.ndc = corner * 2 - 1;
    output.position = float4(output.ndc, 1, 1);
    return output;
}
