// The neighborhood water sheet: world positions; texture coordinates in tiles.

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
};

struct Input {
    float3 position : TEXCOORD0;
    float2 tiles : TEXCOORD1;
};

struct Output {
    float4 position : SV_Position;
    float2 tiles : TEXCOORD0;
};

Output main(Input input) {
    Output output;
    output.position = mul(viewProjection, float4(input.position, 1));
    output.tiles = input.tiles;
    return output;
}
