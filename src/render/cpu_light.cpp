#include "render/cpu_light.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::render {

namespace {

float saturate(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

// Whether light i is lit per pixel through the normal map instead of at the vertices.
bool perPixel(const VertexUniforms& u, int i) {
    const float* p = u.pixelLights;
    const auto fi = static_cast<float>(i);
    return (p[0] > 0 && fi == p[1]) || (p[0] > 1 && fi == p[2]) || (p[0] > 2 && fi == p[3]);
}

} // namespace

std::array<float, 4> gameLight(const VertexUniforms& u, const Vec3& normal, const Vec3& world, bool allLights) {
    if (u.ambient[3] < 0.5f) {
        return {0, 0, 0, 0};
    }
    const Vec3 n = normalize(normal);
    Vec3 sum{u.ambient[0], u.ambient[1], u.ambient[2]};
    for (std::size_t i = 0; i < kObjectLights; ++i) {
        if (!allLights && perPixel(u, static_cast<int>(i))) {
            continue;
        }
        const float* direction = u.lightDirections[i];
        Vec3 toward{direction[0], direction[1], direction[2]};
        float reach = 1;
        if (direction[3] > 0) {  // a point light: toward it from here, and how much of it gets here
            const Vec3 v = sub({u.lightPositions[i][0], u.lightPositions[i][1], u.lightPositions[i][2]}, world);
            const float d2 = dot(v, v);
            const Vec3 axis = toward;
            toward = scale(v, 1 / std::sqrt(std::max(d2, 1e-12f)));
            reach = d2 <= direction[3] ? 1 / (1 + u.lightPositions[i][3] * d2) : 0;
            if (u.lightCones[i][2] > 0.5f) {  // a spot light's cone
                reach *= saturate((dot(toward, axis) - u.lightCones[i][0]) * u.lightCones[i][1]);
            }
        }
        const float share = std::max(0.0f, dot(n, toward) * reach);
        for (std::size_t k = 0; k < 3; ++k) {
            sum[k] += share * u.lightColors[i][k];
        }
    }
    const float* thumbnail = u.thumbnail;
    // A lot skirt: the fixed-function vertex colour, clamped, times its material's colour scale.
    if (u.ambient[3] > 2.5f) {
        return {saturate(sum[0]) * thumbnail[0], saturate(sum[1]) * thumbnail[1], saturate(sum[2]) * thumbnail[2], 1};
    }
    // A family thumbnail: the game's vertex colour there, clamped, then doubled by its texture stage.
    if (thumbnail[3] > 0) {
        return {2 * saturate(sum[0] * thumbnail[0] * thumbnail[3]), 2 * saturate(sum[1] * thumbnail[1] * thumbnail[3]),
                2 * saturate(sum[2] * thumbnail[2] * thumbnail[3]), 2};
    }
    return {sum[0], sum[1], sum[2], 1};
}

} // namespace ts2::render
