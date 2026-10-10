// A lot skirt's boundary curtain: world positions only (it only writes depth).

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
};

float4 main(float3 position : TEXCOORD0) : SV_Position {
    return mul(viewProjection, float4(position, 1));
}
