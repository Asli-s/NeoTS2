// Skinned scene vertices: each follows up to four joints (weighted), then world placement and the
// camera. Joint matrices are 3x4, three rows each, starting at jointBase.

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
    row_major float4x4 model;           // the instance's placement
    float4 ambient;                     // w: 1 when lit as the game lights objects
    float4 lightDirections[8];          // toward each light, w: a point light's range squared (0: directional)
    float4 lightColors[8];
    float4 lightPositions[8];           // a point light's position, w: its falloff
    float4 lightCones[8];               // a spot light's cosine at the cone's edge and 1 / (inner - edge) cosines, z: 1 for a spot
    float4 thumbnail;                   // rgb: the material's diffuse (a lot skirt's colour scale), w: the thumbnail scale (0: not a thumbnail)
    int4 pixelLights;                   // x: how many lights are lit per pixel through the normal map, y, z, w: which
    float4 eye;                         // xyz: the camera's position
    float4 specular;                    // rgb: the material's specular scale, w: its power (0: none)
    uint jointBase;
    uint3 padding;
};

StructuredBuffer<float4> joints : register(t0, space0);

#include "object_light.hlsli"

struct Input {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float alpha : TEXCOORD3;
    uint4 joint : TEXCOORD4;   // 255: none
    float4 weight : TEXCOORD5;
    float2 lightUv : TEXCOORD6;  // unused: skinned surfaces have no light map
    float3 color : TEXCOORD7;
    float3 tangent : TEXCOORD8;
};

float3x4 jointMatrix(uint j) {
    const uint row = (jointBase + j) * 3;
    return float3x4(joints[row], joints[row + 1], joints[row + 2]);
}

Output main(Input input) {
    float3 position = 0, normal = 0, tangent = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        if (input.joint[k] != 255 && input.weight[k] > 0) {
            const float3x4 m = jointMatrix(input.joint[k]);
            position += input.weight[k] * mul(m, float4(input.position, 1));
            normal += input.weight[k] * mul((float3x3)m, input.normal);
            tangent += input.weight[k] * mul((float3x3)m, input.tangent);
        }
    }
    Output output;
    const float4 world = mul(model, float4(position, 1));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)model, normalize(normal));
    output.uv = input.uv;
    output.world = world.xyz;
    output.alpha = input.alpha;
    output.color = input.color;
    output.light = gameLight(output.normal, world.xyz);
    output.reflection = reflectedView(output.normal, world.xyz);
    output.specular = gameSpecular(output.normal, output.reflection, output.light.w);
    output.lightUv = float2(0, 0);
    pixelLightVectors(output.normal, mul((float3x3)model, tangent), world.xyz, output);
    return output;
}
