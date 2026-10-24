// Neighborhood terrain vertices: already in world space. The grid position (column, row) is the
// texture coordinate the paints and their alpha maps use.

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
};

struct Input {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 grid : TEXCOORD2;
};

struct Output {
    float4 position : SV_Position;
    float2 grid : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float height : TEXCOORD2;
};

Output main(Input input) {
    Output output;
    output.position = mul(viewProjection, float4(input.position, 1));
    output.grid = input.grid;
    output.normal = input.normal;
    output.height = input.position.z;
    return output;
}
