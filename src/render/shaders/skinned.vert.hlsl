// Skinned scene vertices: each follows up to four joints (weighted), then world placement and the
// camera. Joint matrices are 3x4, three rows each, starting at jointBase.

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
    row_major float4x4 model;           // the instance's placement
    uint jointBase;
    uint3 padding;
};

StructuredBuffer<float4> joints : register(t0, space0);

struct Input {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float alpha : TEXCOORD3;
    uint4 joint : TEXCOORD4;   // 255: none
    float4 weight : TEXCOORD5;
};

struct Output {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 world : TEXCOORD2;
    float alpha : TEXCOORD3;
};

float3x4 jointMatrix(uint j) {
    const uint row = (jointBase + j) * 3;
    return float3x4(joints[row], joints[row + 1], joints[row + 2]);
}

Output main(Input input) {
    float3 position = 0, normal = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        if (input.joint[k] != 255 && input.weight[k] > 0) {
            const float3x4 m = jointMatrix(input.joint[k]);
            position += input.weight[k] * mul(m, float4(input.position, 1));
            normal += input.weight[k] * mul((float3x3)m, input.normal);
        }
    }
    Output output;
    const float4 world = mul(model, float4(position, 1));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)model, normalize(normal));
    output.uv = input.uv;
    output.world = world.xyz;
    output.alpha = input.alpha;
    return output;
}
