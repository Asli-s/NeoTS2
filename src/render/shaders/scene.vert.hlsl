// Scene vertices: world placement, then the camera.

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
};

#include "object_light.hlsli"
#include "sdl_gpu.hlsli"

struct Input {
    LOCATION(0) float3 position : TEXCOORD0;
    LOCATION(1) float3 normal : TEXCOORD1;
    LOCATION(2) float2 uv : TEXCOORD2;
    LOCATION(3) float alpha : TEXCOORD3;
    LOCATION(6) float2 lightUv : TEXCOORD6;
    LOCATION(7) float3 color : TEXCOORD7;
    LOCATION(8) float3 tangent : TEXCOORD8;
};

Output main(Input input) {
    Output output;
    const float4 world = mul(model, float4(input.position, 1));
    output.position = mul(viewProjection, world);
    output.normal = mul((float3x3)model, input.normal);
    output.uv = input.uv;
    output.world = world.xyz;
    output.alpha = input.alpha;
    output.color = input.color;
    output.light = gameLight(output.normal, world.xyz);
    output.reflection = reflectedView(output.normal, world.xyz);
    output.specular = gameSpecular(output.normal, output.reflection, output.light.w);
    output.lightUv = input.lightUv;
    pixelLightVectors(output.normal, mul((float3x3)model, input.tangent), world.xyz, output);
    return output;
}
