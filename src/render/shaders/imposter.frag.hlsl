// A lot imposter part, as the game's "Shaders for imposters" script draws it. Its pipeline does the
// blending; this picks the texture's color and alpha and drops what the alpha test rejects.

Texture2D<float4> image : register(t0, space2);
SamplerState imageSampler : register(s0, space2);

cbuffer Part : register(b0, space3) {
    float3 tint;   // the terrain light map's tint
    float cutoff;  // drawn only where alpha is above this
    float mode;    // 0 terrain light map (second coordinates, tinted), 1 texture as is, 2 roof (opaque)
    float3 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

float4 main(Input input) : SV_Target {
    float4 color;
    if (mode < 0.5) {
        color = image.Sample(imageSampler, input.uv1);
        color.rgb *= tint;
    } else {
        color = image.Sample(imageSampler, input.uv);
        if (mode > 1.5) {
            color.a = 1;
        }
    }
    if (color.a <= cutoff) {
        discard;
    }
    return color;
}
