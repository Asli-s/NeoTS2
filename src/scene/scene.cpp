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
    Transform r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = (j == 3 ? m[i * 4 + 3] : 0.0f);
            for (int k = 0; k < 3; ++k) {
                v += m[i * 4 + k] * b.m[k * 4 + j];
            }
            r.m[i * 4 + j] = v;
        }
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

std::vector<std::size_t> perPixelLights(const ObjectLighting& lighting, std::size_t usable) {
    const std::size_t n = std::min(usable, lighting.lights.size());
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < n && out.size() < 3; ++i) {
        if (!lighting.lights[i].point) {
            out.push_back(i);
        }
    }
    if (out.size() < 3) {
        for (const bool spot : {false, true}) {
            const auto& lights = lighting.lights;
            const auto it = std::find_if(lights.begin(), lights.begin() + static_cast<std::ptrdiff_t>(n),
                                         [&](const ObjectLighting::Light& l) { return l.point && l.spot == spot; });
            if (it != lights.begin() + static_cast<std::ptrdiff_t>(n)) {
                out.push_back(static_cast<std::size_t>(it - lights.begin()));
                break;
            }
        }
    }
    return out;
}

} // namespace ts2::scene
