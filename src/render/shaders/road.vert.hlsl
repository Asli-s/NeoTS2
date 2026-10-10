// Neighborhood road quads: world positions, the road texture's coordinates and which road texture.

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
};

struct Input {
    float3 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float texture : TEXCOORD2;
};

struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float texture : TEXCOORD1;
    float2 ground : TEXCOORD2;  // x and y in world units
};

Output main(Input input) {
    Output output;
    output.position = mul(viewProjection, float4(input.position, 1));
    output.uv = input.uv;
    output.texture = input.texture;
    output.ground = input.position.xy;
    return output;
}
