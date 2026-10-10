// The sky: the camera's ray through the pixel looks up a sky cube map (its faces laid out with z up).
// Seen in the water the ray is mirrored (its z turned around: forward.w -1); a lot's sky, behind
// everything, is looked up directly (forward.w 1).

TextureCube<float4> sky : register(t0, space2);
SamplerState skySampler : register(s0, space2);

cbuffer Camera : register(b0, space3) {
    float4 right;    // xyz: the camera's right, times tan(half the horizontal field of view)
    float4 up;       // xyz: its up, times tan(half the vertical field of view)
    float4 forward;  // xyz: where it looks; w: 1, or -1 to mirror the ray in the water
};

struct Input {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

float4 main(Input input) : SV_Target {
    float3 d = normalize(forward.xyz + input.ndc.x * right.xyz + input.ndc.y * up.xyz);
    d.z *= forward.w;
    return float4(sky.Sample(skySampler, d).rgb, 1);
}
