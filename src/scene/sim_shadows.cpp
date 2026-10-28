#include "scene/sim_shadows.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::scene::simshadows {

namespace {

using Vec = std::array<float, 3>;

float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

Vec unit(const Vec& v, const Vec& fallback) {
    const float l = std::sqrt(dot(v, v));
    return l > 1e-6f && std::isfinite(l) ? Vec{v[0] / l, v[1] / l, v[2] / l} : fallback;
}

// The Sim's extent around its root, as the game frames it: 0.7 to either side at 1 below and at 2
// above.
constexpr float kWidth = 0.7f, kBelow = -1, kAbove = 2;
constexpr float kNear = -8, kFar = 1;

} // namespace

std::array<float, 16> camera(const Vec& toSun, const Vec& root) {
    const Vec d = unit(toSun, {0, 0, 1});
    // The picture looks the way the light travels, its up as near the world's up as that allows.
    const Vec forward{-d[0], -d[1], -d[2]};
    const float s = std::sqrt(std::max(1 - d[2] * d[2], 0.0f));
    const Vec up = s > 1e-4f ? unit({d[2] * forward[0], d[2] * forward[1], 1 + d[2] * forward[2]}, {0, 1, 0}) : Vec{0, 1, 0};
    const Vec right = cross(forward, up);
    // A point at height h above the root and t along the sun's way across the ground lies at
    // s h - d.z t on the picture's up axis.
    const float bottom = std::min(s * kBelow - d[2] * kWidth, s * kAbove - d[2] * kWidth);
    const float top = std::max(s * kBelow + d[2] * kWidth, s * kAbove + d[2] * kWidth);
    const float height = std::max(top - bottom, 1e-3f);
    const float ax = 1 / kWidth, ay = 2 / height, az = 1 / (kFar - kNear);
    const float by = -(top + bottom) / height, bz = -kNear * az;
    auto row = [&](const Vec& axis, float scale, float offset) {
        return std::array<float, 4>{axis[0] * scale, axis[1] * scale, axis[2] * scale, offset - dot(axis, root) * scale};
    };
    const auto x = row(right, ax, 0), y = row(up, ay, by), z = row(forward, az, bz);
    return {x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3], z[0], z[1], z[2], z[3], 0, 0, 0, 1};
}

int blurWeight(int across, int up, int radius) {
    radius = std::clamp(radius, 0, 8);
    if (std::abs(across) > radius || std::abs(up) > radius) {
        return 0;
    }
    const int side = 2 * radius + 1;
    const int index = (across + radius) * side + (up + radius);
    int left = 255, taps = side * side, w = 0;
    for (int i = 0; i <= index; ++i) {
        w = left / taps;
        left -= w;
        --taps;
    }
    return w;
}

std::array<float, 3> shade(const windowlight::Daylight& day, float strength) {
    constexpr Vec kUp{0, 0, 1};
    windowlight::Daylight whole = day;
    whole.outdoorStrength = 1;
    windowlight::Daylight dimmed = whole;
    if (!dimmed.outdoorLights.empty()) {
        dimmed.outdoorLights[0].strength *= std::max(1 - (std::isfinite(strength) ? strength : 1.0f), 0.0f);
    }
    const Vec all = windowlight::skyLight(whole, kUp), less = windowlight::skyLight(dimmed, kUp);
    Vec out;
    for (std::size_t c = 0; c < 3; ++c) {
        const float a = std::clamp(all[c], 0.0f, 1.0f), b = std::clamp(less[c], 0.0f, 1.0f);
        const float ratio = a > 0 ? b / a : 1.0f;
        out[c] = std::min(255.0f, std::round(ratio * 256)) / 255;
    }
    return out;
}

} // namespace ts2::scene::simshadows
