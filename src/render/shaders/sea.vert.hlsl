// A beach lot's sea as the game's lotSkirtSea material waves it: two sine waves across x and y lift
// each vertex, four scrolling copies of the bump map ripple it, and where it lies on the screen is
// where its mirror image is read from.

cbuffer Sea : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
    row_major float4x4 model;
    float4 eye;   // xyz: the camera's position
    float4 time;  // x: real seconds the lot has run unpaused
    float4 tint;  // rgb: the colour added to the water
};

#include "sdl_gpu.hlsli"

struct Input {
    LOCATION(0) float3 position : TEXCOORD0;
    LOCATION(1) float3 normal : TEXCOORD1;
    LOCATION(2) float2 depth : TEXCOORD2;  // x: how see-through the water's depth makes it
    LOCATION(3) float alpha : TEXCOORD3;   // the surface's opacity scale (1 but for a sea fading at the shore)
};

struct Output {
    float4 position : SV_Position;
    float4 screen : TEXCOORD0;  // the reflection's texture coordinates before the divide by w
    float4 waves01 : TEXCOORD1;
    float4 waves23 : TEXCOORD2;
    float3 eye : TEXCOORD3;     // toward the camera, in the surface's tangent space
    float4 tint : TEXCOORD4;
    float fade : TEXCOORD5;
};

// The material's waves: repeats along the axis, height and speed (wmXRepeat 5, wmYRepeat 6,
// wave heights 0.001, speeds 3). As in the game, the height also weighs z in the phase.
static const float4 kWaveX = float4(5, 0, 0.001, 3);
static const float4 kWaveY = float4(0, 6, 0.001, 3);

Output main(Input input) {
    Output output;
    // The waves and ripples run on the lot's own coordinates, as the game's do.
    const float4 posAndTime = float4(input.position, time.x);
    const float lift = sin(dot(posAndTime, kWaveX)) * kWaveX.z;
    const float phaseY = dot(posAndTime, kWaveY);
    const float3 local = float3(input.position.xy, input.position.z + lift + sin(phaseY) * kWaveY.z);
    const float3 world = mul(model, float4(local, 1)).xyz;

    output.position = mul(viewProjection, float4(world, 1));
    const float4 clip = output.position;
    output.screen = float4(0.5 * (clip.ww - clip.xy), clip.ww);

    // A lot is only turned and moved into place, so its turn's transpose brings directions back.
    const float3x3 toLocal = transpose((float3x3)model);
    const float3 toEye = mul(toLocal, normalize(eye.xyz - world));
    // As the game has it, the y wave's slope tilts the normal both ways.
    const float3 normal = normalize(input.normal + float3(cos(phaseY) * -float2(kWaveX.x * kWaveX.z, kWaveY.y * kWaveY.z), 0));

    const float2 slide = fmod(time.x, 100) * 0.005;
    const float2 base = local.xy * 0.05;
    output.waves01 = float4(base * 0.01 + slide * 2, base + slide * 2.5);
    output.waves23 = float4(base - slide * 2.3, base * 0.1 + slide * 1.5);

    const float3 binormal = normalize(cross(normal, float3(1, 0, 0)));
    const float3 tangent = normalize(cross(binormal, normal));
    output.eye = float3(dot(tangent, toEye), dot(binormal, toEye), dot(normal, toEye));
    // The game passes the tint as a vertex colour, which its shader model clamps to 0..1, so the
    // material's negative parts add nothing.
    output.tint = float4(saturate(tint.rgb), input.depth.x);
    output.fade = input.alpha;
    return output;
}
