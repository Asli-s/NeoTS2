// A particle's corner: already in the world, with its texture coordinates and colour.

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
};

struct Input {
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
};

Output main(Input input) {
    Output output;
    output.position = mul(viewProjection, float4(input.position, 1));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
