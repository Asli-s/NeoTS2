// Scene surfaces: the material's color times its texture, lit by a sky and a sun.

Texture2D<float4> baseTexture : register(t0, space2);
SamplerState baseSampler : register(s0, space2);

cbuffer Material : register(b0, space3) {
    float4 color;
    float4 sunDirection;  // toward the sun, xyz; w unused
    float hasTexture;
    float alphaCutoff;    // discard below this (0 for none)
    float2 padding;
};

struct Input {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 world : TEXCOORD2;
    float alpha : TEXCOORD3;
    bool front : SV_IsFrontFace;
};

float4 main(Input input) : SV_Target {
    float4 albedo = color;
    if (hasTexture > 0.5) {
        albedo *= baseTexture.Sample(baseSampler, input.uv);
    }
    albedo.a *= input.alpha;
    if (albedo.a < alphaCutoff) {
        discard;
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
