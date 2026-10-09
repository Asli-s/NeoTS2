#pragma once

#include <array>
#include <cmath>

namespace ts2::render {

using Vec3 = std::array<float, 3>;

inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 scale(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline Vec3 normalize(const Vec3& a) {
    const float l = std::sqrt(dot(a, a));
    return l > 0 ? scale(a, 1 / l) : a;
}

// A 4x4 matrix, row-major, applied to column vectors (p' = M p).
struct Mat4 {
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                float v = 0;
                for (int k = 0; k < 4; ++k) {
                    v += m[i * 4 + k] * b.m[k * 4 + j];
                }
                r.m[i * 4 + j] = v;
            }
        }
        return r;
    }

    // A camera at `eye` looking at `target`, with `up` as the world's up direction.
    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        const Vec3 f = normalize(sub(target, eye));
        const Vec3 s = normalize(cross(f, up));
        const Vec3 u = cross(s, f);
        Mat4 r;
        r.m = {s[0],  s[1],  s[2],  -dot(s, eye),
               u[0],  u[1],  u[2],  -dot(u, eye),
               -f[0], -f[1], -f[2], dot(f, eye),
               0,     0,     0,     1};
        return r;
    }

    // Perspective projection to depth 0 (near) .. 1 (far), right-handed view space looking down -z.
    static Mat4 perspective(float fovY, float aspect, float nearZ, float farZ) {
        const float t = 1 / std::tan(fovY / 2);
        Mat4 r;
        r.m = {t / aspect, 0, 0, 0,
               0, t, 0, 0,
               0, 0, farZ / (nearZ - farZ), nearZ * farZ / (nearZ - farZ),
               0, 0, -1, 0};
        return r;
    }
};

} // namespace ts2::render
