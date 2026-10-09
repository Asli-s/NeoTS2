#include "scene/scene.hpp"

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

} // namespace ts2::scene
