// Skinned scene vertices: each is first moved by its morphs, then follows up to four joints
// (weighted), then world placement and the camera, as the game's shaders morph before skinning.
// Joint matrices are 3x4, three rows each, starting at jointBase. Morph weights are four to a row,
// starting at row morphBase of the same buffer (0xFFFFFFFF: none).

cbuffer Frame : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
    row_major float4x4 model;           // the instance's placement
    float4 ambient;                     // w: 1 when lit as the game lights objects
    float4 lightDirections[8];          // toward each light, w: a point light's range squared (0: directional)
    float4 lightColors[8];
    float4 lightPositions[8];           // a point light's position, w: its falloff
    float4 lightCones[8];               // a spot light's cosine at the cone's edge and 1 / (inner - edge) cosines, z: 1 for a spot
    float4 thumbnail;                   // rgb: the material's diffuse (a lot skirt's colour scale), w: the thumbnail scale (0: not a thumbnail)
    float4 pixelLights;                 // whole numbers, x: how many lights are lit per pixel through the normal map, y, z, w: which
    float4 eye;                         // xyz: the camera's position
    float4 specular;                    // rgb: the material's specular scale, w: its power (0: none)
    uint jointBase;
    uint morphBase;
    uint2 padding;
    float4 eyes;  // a face's eye direction (x, y) and the left and right eye's stretch
};

StructuredBuffer<float4> joints : register(t0, space0);
// Per morphing vertex, eight rows: each stream's position delta and morph (w), then each stream's
// normal delta.
StructuredBuffer<float4> morphs : register(t1, space0);

#include "object_light.hlsli"
#include "sdl_gpu.hlsli"

struct Input {
    LOCATION(0) float3 position : TEXCOORD0;
    LOCATION(1) float3 normal : TEXCOORD1;
    LOCATION(2) float2 uv : TEXCOORD2;
    LOCATION(3) float alpha : TEXCOORD3;
    LOCATION(4) uint4 joint : TEXCOORD4;   // 255: none
    LOCATION(5) float4 weight : TEXCOORD5;
    LOCATION(6) float2 lightUv : TEXCOORD6;  // unused: skinned surfaces have no light map
    LOCATION(7) float3 color : TEXCOORD7;
    LOCATION(8) float3 tangent : TEXCOORD8;
    LOCATION(9) uint morphVertex : TEXCOORD9;  // its first row in morphs, 0xFFFFFFFF: none
    LOCATION(10) float2 eyeUv : TEXCOORD10;    // u: 1 on an eye, v: its stretch, below 1 the right eye
};

// The shift of a face vertex's texture coordinates that moves its eye, as the game's eye shader
// gives it: the direction, plus each eye's own stretch by how far the vertex is from the eye's middle.
float2 eyeShift(float2 eyeUv) {
    const float s = eyeUv.y - 1;
    const float right = s < 0 ? 1 : 0;
    const float x = eyes.x + (1 - right) * abs(s) * eyes.z + right * abs(s) * eyes.w;
    return float2(x, -eyes.y) * eyeUv.x;
}

float3x4 jointMatrix(uint j) {
    const uint row = (jointBase + j) * 3;
    return float3x4(joints[row], joints[row + 1], joints[row + 2]);
}

Output main(Input input) {
    if (input.morphVertex != 0xFFFFFFFF && morphBase != 0xFFFFFFFF) {
        const uint row = input.morphVertex * 8;
        [unroll] for (int s = 0; s < 4; ++s) {
            const float4 move = morphs[row + s];
            const uint morph = (uint)move.w;
            const float weight = joints[morphBase + morph / 4][morph % 4];
            input.position += weight * move.xyz;
            input.normal += weight * morphs[row + 4 + s].xyz;
        }
    }
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
    output.uv = input.uv + eyeShift(input.eyeUv);
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
