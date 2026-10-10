// The sky seen in the water: the camera's ray through the pixel, mirrored in the water (its z
// turned around), looks up the sky cube map (its faces laid out with z up).

TextureCube<float4> sky : register(t0, space2);
SamplerState skySampler : register(s0, space2);

cbuffer Camera : register(b0, space3) {
    float4 right;    // xyz: the camera's right, times tan(half the horizontal field of view)
    float4 up;       // xyz: its up, times tan(half the vertical field of view)
    float4 forward;  // xyz: where it looks
};

struct Input {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

float4 main(Input input) : SV_Target {
    float3 d = normalize(forward.xyz + input.ndc.x * right.xyz + input.ndc.y * up.xyz);
    d.z = -d.z;
    return float4(sky.Sample(skySampler, d).rgb, 1);
}
