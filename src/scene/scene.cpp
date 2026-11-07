#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::scene {

Transform Transform::of(const std::array<float, 3>& t, const std::array<float, 4>& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    Transform r;
    r.m = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),     t[0],
           2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),     t[1],
           2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y), t[2]};
    return r;
}

Transform Transform::at(float x, float y, float z, float angle) {
    return of({x, y, z}, {0, 0, std::sin(angle / 2), std::cos(angle / 2)});
}

Transform Transform::rigidInverse() const {
    Transform r;
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) {
            r.m[i * 4 + k] = m[k * 4 + i];
        }
        r.m[i * 4 + 3] = -(m[3] * m[i] + m[7] * m[4 + i] + m[11] * m[8 + i]);
    }
    return r;
}

Transform Transform::operator*(const Transform& b) const {
    // Sims' skeletons do thousands of these a frame, so this skips the array's bounds checks. Starting from 0 keeps a sum of negative zeros at plain zero.
    Transform r;
    const float* const row = m.data();
    const float* const c = b.m.data();
    float* const out = r.m.data();
    for (int i = 0; i < 12; i += 4) {
        const float x = row[i], y = row[i + 1], z = row[i + 2];
        out[i] = 0.0f + x * c[0] + y * c[4] + z * c[8];
        out[i + 1] = 0.0f + x * c[1] + y * c[5] + z * c[9];
        out[i + 2] = 0.0f + x * c[2] + y * c[6] + z * c[10];
        out[i + 3] = row[i + 3] + x * c[3] + y * c[7] + z * c[11];
    }
    return r;
}

void Transform::apply(float* v, bool point) const {
    const float x = v[0], y = v[1], z = v[2];
    for (int i = 0; i < 3; ++i) {
        v[i] = m[i * 4] * x + m[i * 4 + 1] * y + m[i * 4 + 2] * z + (point ? m[i * 4 + 3] : 0.0f);
    }
}

ObjectLighting withSimLights(const ObjectLighting& lighting) {
    ObjectLighting out = lighting;
    out.lights.insert(out.lights.end(), lighting.simLights.begin(), lighting.simLights.end());
    out.simLights.clear();
    auto score = [](const ObjectLighting::Light& l) { return l.color[0] * l.color[0] + l.color[1] * l.color[1] + l.color[2] * l.color[2]; };
    std::stable_sort(out.lights.begin(), out.lights.end(), [&](const auto& a, const auto& b) { return score(a) > score(b); });
    constexpr std::size_t kMaxLights = 8;
    if (out.lights.size() > kMaxLights) {
        out.lights.resize(kMaxLights);
    }
    return out;
}

PixelLights perPixelLights(const ObjectLighting& lighting, std::size_t usable) {
    const std::size_t n = std::min(usable, lighting.lights.size());
    PixelLights out;
    for (std::size_t i = 0; i < n && out.count < 3; ++i) {
        if (!lighting.lights[i].point) {
            out.index[out.count++] = i;
        }
    }
    if (out.count < 3) {
        for (const bool spot : {false, true}) {
            const auto& lights = lighting.lights;
            const auto it = std::find_if(lights.begin(), lights.begin() + static_cast<std::ptrdiff_t>(n),
                                         [&](const ObjectLighting::Light& l) { return l.point && l.spot == spot; });
            if (it != lights.begin() + static_cast<std::ptrdiff_t>(n)) {
                out.index[out.count++] = static_cast<std::size_t>(it - lights.begin());
                break;
            }
        }
    }
    return out;
}

} // namespace ts2::scene
