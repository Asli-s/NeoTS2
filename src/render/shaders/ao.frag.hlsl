// Ambient occlusion (not in the original game): how open the space above each pixel's surface is, from
// the depth pass alone. 1 is fully open, and lower where nearby surfaces crowd it. It repeats a 4 by 4
// pattern of turned samples, which the blur pass smooths away. At half the view's size each pixel
// works out one of the 2 by 2 depth pass pixels it stands for, as ao_halfdepth.frag picks them.

#include "sdl_gpu.hlsli"

COMBINED Texture2D<float> depthTexture : register(t0, space2);
COMBINED SamplerState depthSampler : register(s0, space2);

cbuffer Occlusion : register(b0, space3) {
    row_major float4x4 clipToWorld;
    row_major float4x4 worldToClip;
    float4 eye;
    float4 size;      // the target's width and height, then the depth pass's
    float radius;     // how far around a point counts, in tiles
    float strength;   // the power the openness is raised to
    float2 unused;
};

static const int kSamples = 16;

float depthAt(int2 pixel) {
    return depthTexture.Load(int3(clamp(pixel, int2(0, 0), int2(size.zw) - 1), 0));
}

float3 worldAt(int2 pixel) {
    const float2 ndc = float2((pixel.x + 0.5) / size.z * 2 - 1, 1 - (pixel.y + 0.5) / size.w * 2);
    const float4 p = mul(clipToWorld, float4(ndc, depthAt(pixel), 1));
    return p.xyz / p.w;
}

// The surface's normal from the depths around it, using the nearer neighbour on each axis so that an
// edge's far side doesn't bend it.
float3 normalAt(int2 pixel, float3 here) {
    const float3 left = worldAt(pixel - int2(1, 0)), right = worldAt(pixel + int2(1, 0));
    const float3 up = worldAt(pixel - int2(0, 1)), down = worldAt(pixel + int2(0, 1));
    const float3 across = dot(here - left, here - left) < dot(right - here, right - here) ? here - left : right - here;
    const float3 along = dot(here - up, here - up) < dot(down - here, down - here) ? here - up : down - here;
    const float3 n = normalize(cross(across, along));
    return dot(n, eye.xyz - here) < 0 ? -n : n;
}

// The depth pass pixel a pixel of the target works out: itself at full size. At half size it is the
// nearest of its 2 by 2, or the farthest, in turn like a checkerboard.
int2 chosenAt(int2 target) {
    if (size.z <= size.x) {
        return target;
    }
    const bool nearest = ((target.x + target.y) & 1) != 0;
    int2 best = target * 2;
    float bestDepth = depthAt(best);
    for (int k = 1; k < 4; ++k) {
        const int2 at = target * 2 + int2(k & 1, k >> 1);
        const float depth = depthAt(at);
        // Depth grows toward the camera.
        if (nearest ? depth > bestDepth : depth < bestDepth) {
            best = at;
            bestDepth = depth;
        }
    }
    return best;
}

float main(float4 position : SV_Position) : SV_Target {
    const int2 target = int2(position.xy);
    const int2 pixel = chosenAt(target);
    // Depth 0 is the far plane: the sky has nothing to shade.
    if (depthAt(pixel) <= 0) {
        return 1;
    }
    const float3 here = worldAt(pixel);
    const float3 n = normalAt(pixel, here);
    const float3 side = normalize(abs(n.z) < 0.99 ? cross(n, float3(0, 0, 1)) : cross(n, float3(1, 0, 0)));
    const float3 other = cross(n, side);
    static const float kOrder[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const float turn = kOrder[(target.x & 3) + (target.y & 3) * 4] / 16 * 6.2831853;
    float crowded = 0;
    for (int i = 0; i < kSamples; ++i) {
        const float t = (i + 0.5) / kSamples;
        const float angle = 2.3999632 * i + turn;
        const float flat = sqrt(t);
        const float3 direction = side * (flat * cos(angle)) + other * (flat * sin(angle)) + n * sqrt(1 - t);
        // More samples close by, where corners and contacts are.
        const float3 point_ = here + n * (radius * 0.02) + direction * (radius * lerp(0.1, 1, t * t));
        const float4 clip = mul(worldToClip, float4(point_, 1));
        if (clip.w <= 0) {
            continue;
        }
        const float2 ndc = clip.xy / clip.w;
        const int2 at = int2(floor(float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * size.zw));
        // Depth grows toward the camera.
        if (depthAt(at) > clip.z / clip.w) {
            // Something far in front of this point is not what crowds it.
            const float away = length(worldAt(at) - here);
            crowded += saturate(2 - away / radius);
        }
    }
    return pow(saturate(1 - crowded / kSamples), strength);
}
