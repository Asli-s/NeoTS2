// A pool's water surface as the game's PoolWaterSurfaceMaterial waves it: two sine waves across x
// and y lift each vertex, four scrolling copies of the bump map ripple it, and where it lies on the
// screen is where its mirror image is read from.

cbuffer Pool : register(b0, space1) {
    row_major float4x4 viewProjection;  // applied to column vectors
    row_major float4x4 model;
    float4 eye;           // xyz: the camera's position
    float4 waveX;         // repeats along x, 0, height, speed
    float4 waveY;         // 0, repeats along y, height, speed
    float4 waveNormal;    // xy: how far the waves tilt the normal along x and y
    float4 light;         // xyz: toward the sun
    float4 lightColor;    // the sun's colour, its specular colour too
    float4 night;         // the highlight's scale for the time of day
    float4 time;          // x: real seconds the lot has run unpaused, y: the water's height in the game's own frame minus ours
    float4 tint;          // rgb: the colour added to the water, a: its murk (see scene::Material::poolSurface)
};

#include "sdl_gpu.hlsli"

struct Input {
    LOCATION(0) float3 position : TEXCOORD0;
    LOCATION(1) float3 normal : TEXCOORD1;
};

struct Output {
    float4 position : SV_Position;
    float4 screen : TEXCOORD0;  // the reflection's texture coordinates before the divide by w
    float4 waves01 : TEXCOORD1;
    float4 waves23 : TEXCOORD2;
    float3 eye : TEXCOORD3;     // toward the camera, in the surface's tangent space
    float4 specular : TEXCOORD4;
    float4 tint : TEXCOORD5;
};

Output main(Input input) {
    Output output;
    // The waves and ripples run on the pool's own lot coordinates, as the game's do. The game's waves
    // take the height in its own frame, which differs from ours by `time.y`.
    const float4 posAndTime = float4(input.position.xy, input.position.z + time.y, time.x);
    const float lift = sin(dot(posAndTime, waveX)) * waveX.z;
    const float phaseY = dot(posAndTime, waveY);
    const float3 local = float3(input.position.xy, input.position.z + lift + sin(phaseY) * waveY.z);
    const float3 world = mul(model, float4(local, 1)).xyz;

    output.position = mul(viewProjection, float4(world, 1));
    const float4 clip = output.position;
    output.screen = float4(0.5 * (clip.ww - clip.xy), clip.ww);

    // A lot is only turned and moved into place, so its turn's transpose brings directions back.
    const float3x3 toLocal = transpose((float3x3)model);
    const float3 toEye = mul(toLocal, normalize(eye.xyz - world));
    // As the game has it, the y wave's slope tilts the normal both ways. Murky water (ponds drawn like
    // pools, not in the original game) leaves the tilt out: across a pond's breadth it shows as stripes.
    const float2 tilt = tint.a > 0 ? float2(0, 0) : waveNormal.xy;
    const float3 normal = normalize(input.normal + float3(cos(phaseY) * tilt, 0));
    const float3 half_ = normalize(toEye + mul(toLocal, light.xyz));
    // The size of the base, as the game's vertex shader model raises it.
    output.specular = saturate(lightColor * pow(abs(dot(normal, half_)), 50) * night * lightColor * 0.5);

    const float2 slide = fmod(time.x, 100) * 0.005;
    const float2 base = local.xy * 0.05;
    output.waves01 = float4(base + slide * 2, base * 2 + slide * 4);
    output.waves23 = float4(base * 7 + slide * 2, base * 12 + slide);

    const float3 binormal = normalize(cross(normal, float3(1, 0, 0)));
    const float3 tangent = normalize(cross(binormal, normal));
    output.eye = float3(dot(tangent, toEye), dot(binormal, toEye), dot(normal, toEye));
    output.tint = tint;
    return output;
}
