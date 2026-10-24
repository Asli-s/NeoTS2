// Scene surfaces: the material's color times its texture, lit by their light map (a lot's floors and
// walls), as the game lights objects when the vertices bring that light, otherwise by a sky and a
// sun, unless unlit.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float4> baseTexture : register(t0, space2);
COMBINED SamplerState baseSampler : register(s0, space2);
COMBINED Texture2D<float4> lightMapTexture : register(t1, space2);
COMBINED SamplerState lightMapSampler : register(s1, space2);

cbuffer Material : register(b0, space3) {
    float4 color;
    float4 sunDirection;  // toward the sun, xyz; w unused
    float hasTexture;
    float alphaCutoff;    // discard below this (0 for none)
    float unlit;          // 1: color and texture as they are, without light
    float padding;
    float4 lightMap;      // x: the gain on the light map (0: none); y: 1 to leave the color out; z: 1 for terrain
};

struct Input {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 world : TEXCOORD2;
    float alpha : TEXCOORD3;
    float4 light : TEXCOORD4;
    float2 lightUv : TEXCOORD5;
    bool front : SV_IsFrontFace;
};

float4 main(Input input) : SV_Target {
    const float4 texel = hasTexture > 0.5 ? baseTexture.Sample(baseSampler, input.uv) : float4(1, 1, 1, 1);
    float4 albedo = color * texel;
    albedo.a *= input.alpha;
    if (albedo.a < alphaCutoff) {
        discard;
    }
    if (unlit > 0.5) {
        return albedo;
    }
    // A lot's floor or wall, as the game draws it: its texture (floors: times the material's diffuse,
    // walls: alone) times the light map times 2, and no other light.
    if (lightMap.x > 0) {
        const float3 base = lightMap.y > 0.5 && hasTexture > 0.5 ? texel.rgb : albedo.rgb;
        const float3 lit = lightMapTexture.Sample(lightMapSampler, input.lightUv).rgb;
        // The lot's terrain: darkened by its light map (a multiply pass in the game, so at most 1).
        if (lightMap.z > 0.5) {
            return float4(base * saturate(lit * lightMap.x), albedo.a);
        }
        return float4(saturate(base * lit * lightMap.x), albedo.a);
    }
    // A family thumbnail: the texture times the light, which already holds the material's diffuse.
    if (input.light.w > 1.5) {
        return float4((hasTexture > 0.5 ? texel.rgb : color.rgb) * input.light.rgb, albedo.a);
    }
    if (input.light.w > 0.5) {
        return float4(albedo.rgb * input.light.rgb, albedo.a);
    }
    float3 n = normalize(input.normal);
    if (!input.front) {
        n = -n;  // two-sided surfaces light their back as a front
    }
    // Sky from above, a little light from the ground, and the sun.
    const float sky = lerp(0.45, 0.85, saturate(n.z * 0.5 + 0.5));
    const float sun = saturate(dot(n, sunDirection.xyz)) * 0.75;
    return float4(albedo.rgb * (sky + sun), albedo.a);
}
