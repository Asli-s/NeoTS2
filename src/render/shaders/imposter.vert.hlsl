// Lot imposter parts: world positions and two sets of texture coordinates.

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
};

struct Input {
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float2 uv1 : TEXCOORD2;
};

struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

Output main(Input input) {
    Output output;
    output.position = mul(viewProjection, float4(input.position, 1));
    output.uv = input.uv;
    output.uv1 = input.uv1;
    return output;
}
