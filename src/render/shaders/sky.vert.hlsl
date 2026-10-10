// The sky: one triangle covering the screen; the fragment shader works out each pixel's direction.

struct Output {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

Output main(uint id : SV_VertexID) {
    const float2 corner = float2((id << 1) & 2, id & 2);  // (0,0) (2,0) (0,2)
    Output output;
    output.ndc = corner * 2 - 1;
    output.position = float4(output.ndc, 1, 1);
    return output;
}
