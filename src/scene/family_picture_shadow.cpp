#include "scene/family_picture_shadow.hpp"

#include "scene/object_shadow_picture.hpp"
#include "scene/rig.hpp"
#include "scene/sim_shadows.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace ts2::scene::familyshadow {

namespace {

using Vec = std::array<float, 3>;

float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vec cross(const Vec& a, const Vec& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

bool normalize(Vec& v) {
    const float n = std::sqrt(dot(v, v));
    if (!(n > 1e-8f) || !std::isfinite(n)) {
        return false;
    }
    v = {v[0] / n, v[1] / n, v[2] / n};
    return true;
}

void grow(Box& box, const Vec& p) {
    for (std::size_t k = 0; k < 3; ++k) {
        box.low[k] = std::min(box.low[k], p[k]);
        box.high[k] = std::max(box.high[k], p[k]);
    }
}

// The inverse of a transform's 3 x 3 part and move, or nullopt when it has none.
std::optional<Transform> inverse(const Transform& t) {
    const auto& m = t.m;
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::abs(det) > 1e-12f) || !std::isfinite(det)) {
        return std::nullopt;
    }
    Transform r;
    r.m[0] = (e * i - f * h) / det;
    r.m[1] = (c * h - b * i) / det;
    r.m[2] = (b * f - c * e) / det;
    r.m[4] = (f * g - d * i) / det;
    r.m[5] = (a * i - c * g) / det;
    r.m[6] = (c * d - a * f) / det;
    r.m[8] = (d * h - e * g) / det;
    r.m[9] = (b * g - a * h) / det;
    r.m[10] = (a * e - b * d) / det;
    for (int row = 0; row < 3; ++row) {
        r.m[static_cast<std::size_t>(row * 4 + 3)] =
            -(r.m[static_cast<std::size_t>(row * 4)] * m[3] + r.m[static_cast<std::size_t>(row * 4 + 1)] * m[7] +
              r.m[static_cast<std::size_t>(row * 4 + 2)] * m[11]);
    }
    return r;
}

// Clip x and y of a point through the light's camera.
std::array<float, 2> clip(const std::array<float, 16>& c, const Vec& p) {
    return {c[0] * p[0] + c[1] * p[1] + c[2] * p[2] + c[3], c[4] * p[0] + c[5] * p[1] + c[6] * p[2] + c[7]};
}

} // namespace

void growByJoints(Box& box, const Mesh& mesh, const std::vector<Transform>& joints, const Transform& place) {
    Box own;
    for (const auto& p : mesh.primitives) {
        if (!isSkinned(p)) {
            continue;
        }
        // The rest vertices by their first joint, in that joint's frame.
        std::map<std::uint8_t, Box> byJoint;
        const std::size_t vertices = std::min(p.positions.size() / 3, p.joints.size() / 4);
        for (std::size_t v = 0; v < vertices; ++v) {
            const std::uint8_t j = p.joints[4 * v];
            if (!(p.weights[4 * v] > 0) || j >= mesh.skin.size() || j >= joints.size()) {
                continue;
            }
            Vec q{p.positions[3 * v], p.positions[3 * v + 1], p.positions[3 * v + 2]};
            if (!std::isfinite(q[0]) || !std::isfinite(q[1]) || !std::isfinite(q[2])) {
                continue;
            }
            mesh.skin[j].inverseBind.apply(q.data(), true);
            grow(byJoint[j], q);
        }
        for (const auto& [j, b] : byJoint) {
            const auto bind = inverse(mesh.skin[j].inverseBind);
            if (!bind) {
                continue;
            }
            const Transform posed = joints[j] * *bind;
            for (int c = 0; c < 8; ++c) {
                Vec corner{(c & 1) ? b.high[0] : b.low[0], (c & 2) ? b.high[1] : b.low[1], (c & 4) ? b.high[2] : b.low[2]};
                posed.apply(corner.data(), true);
                grow(own, corner);
            }
        }
    }
    if (own.empty()) {
        return;
    }
    const auto& m = place.m;
    Vec centre{(own.low[0] + own.high[0]) / 2, (own.low[1] + own.high[1]) / 2, (own.low[2] + own.high[2]) / 2};
    const Vec half{(own.high[0] - own.low[0]) / 2, (own.high[1] - own.low[1]) / 2, (own.high[2] - own.low[2]) / 2};
    place.apply(centre.data(), true);
    for (std::size_t k = 0; k < 3; ++k) {
        const float reach = std::abs(m[k * 4]) * half[0] + std::abs(m[k * 4 + 1]) * half[1] + std::abs(m[k * 4 + 2]) * half[2];
        box.low[k] = std::min(box.low[k], centre[k] - reach);
        box.high[k] = std::max(box.high[k], centre[k] + reach);
    }
}

std::optional<std::array<float, 16>> camera(const Box& box) {
    if (box.empty()) {
        return std::nullopt;
    }
    return objectshadow::boxCamera(box.low, box.high, kToLight);
}

std::vector<float> coverage(const std::vector<float>& triangles, const std::array<float, 16>& camera) {
    constexpr int n = kResolution;
    constexpr float half = n / 2.0f;
    std::vector<float> out(static_cast<std::size_t>(n * n), 0.0f);
    for (std::size_t t = 0; t + 8 < triangles.size(); t += 9) {
        std::array<std::array<float, 2>, 3> s{};
        bool finite = true;
        for (std::size_t k = 0; k < 3; ++k) {
            const auto c = clip(camera, {triangles[t + 3 * k], triangles[t + 3 * k + 1], triangles[t + 3 * k + 2]});
            s[k] = {(c[0] + 1) * half, (1 - c[1]) * half};
            finite = finite && std::isfinite(s[k][0]) && std::isfinite(s[k][1]);
        }
        const float area = (s[1][0] - s[0][0]) * (s[2][1] - s[0][1]) - (s[1][1] - s[0][1]) * (s[2][0] - s[0][0]);
        if (!finite || area == 0) {
            continue;
        }
        const float sign = area > 0 ? 1.0f : -1.0f;
        // Clamped first, so that a corner far off the picture can't overflow the texel numbers.
        const auto fit = [](float v) { return std::clamp(v, -1.0f, static_cast<float>(n)); };
        const float x0 = fit(std::min({s[0][0], s[1][0], s[2][0]})), x1 = fit(std::max({s[0][0], s[1][0], s[2][0]}));
        const float y0 = fit(std::min({s[0][1], s[1][1], s[2][1]})), y1 = fit(std::max({s[0][1], s[1][1], s[2][1]}));
        const int ix0 = std::max(0, static_cast<int>(std::ceil(x0))), ix1 = std::min(n - 1, static_cast<int>(std::floor(x1)));
        const int iy0 = std::max(0, static_cast<int>(std::ceil(y0))), iy1 = std::min(n - 1, static_cast<int>(std::floor(y1)));
        for (int y = iy0; y <= iy1; ++y) {
            for (int x = ix0; x <= ix1; ++x) {
                const float px = static_cast<float>(x), py = static_cast<float>(y);
                bool inside = true;
                for (std::size_t k = 0; k < 3 && inside; ++k) {
                    const auto& a = s[k];
                    const auto& b = s[(k + 1) % 3];
                    inside = sign * ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) >= 0;
                }
                if (inside) {
                    out[static_cast<std::size_t>(y * n + x)] = 1;
                }
            }
        }
    }
    return out;
}

std::vector<float> blurred(const std::vector<float>& picture) {
    constexpr int n = kResolution;
    std::vector<float> out(static_cast<std::size_t>(n * n), 0.0f);
    if (picture.size() != out.size()) {
        return out;
    }
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            float sum = 0;
            for (int across = -kBlur; across <= kBlur; ++across) {
                for (int up = -kBlur; up <= kBlur; ++up) {
                    const int sx = std::clamp(x + across, 0, n - 1), sy = std::clamp(y + up, 0, n - 1);
                    sum += picture[static_cast<std::size_t>(sy * n + sx)] * static_cast<float>(simshadows::blurWeight(across, up, kBlur)) / 255;
                }
            }
            out[static_cast<std::size_t>(y * n + x)] = sum;
        }
    }
    return out;
}

void lay(Image& picture, const Vec& eye, const Vec& target, const Vec& up, float fovY, const std::array<float, 16>& camera,
         const std::vector<float>& blurredPicture) {
    constexpr int n = kResolution;
    const std::size_t w = picture.width, h = picture.height;
    if (w == 0 || h == 0 || picture.rgba.size() != w * h * 4 || blurredPicture.size() != static_cast<std::size_t>(n * n) ||
        !(fovY > 0 && fovY < 3.1f)) {
        return;
    }
    Vec forward{target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    if (!normalize(forward)) {
        return;
    }
    Vec right = cross(forward, up);
    if (!normalize(right)) {
        return;
    }
    const Vec trueUp = cross(right, forward);
    const float reachY = std::tan(fovY / 2), reachX = reachY * static_cast<float>(w) / static_cast<float>(h);
    const auto texel = [&](int x, int y) {
        return blurredPicture[static_cast<std::size_t>(std::clamp(y, 0, n - 1) * n + std::clamp(x, 0, n - 1))];
    };
    for (std::size_t py = 0; py < h; ++py) {
        for (std::size_t px = 0; px < w; ++px) {
            std::uint8_t* pixel = &picture.rgba[(py * w + px) * 4];
            if (pixel[3] == 255) {
                continue;
            }
            const float sx = (2 * (static_cast<float>(px) + 0.5f) / static_cast<float>(w) - 1) * reachX;
            const float sy = (1 - 2 * (static_cast<float>(py) + 0.5f) / static_cast<float>(h)) * reachY;
            const Vec d{forward[0] + sx * right[0] + sy * trueUp[0], forward[1] + sx * right[1] + sy * trueUp[1],
                        forward[2] + sx * right[2] + sy * trueUp[2]};
            if (!(d[2] < -1e-6f)) {
                continue;
            }
            const float t = -eye[2] / d[2];
            if (!(t > 0)) {
                continue;
            }
            const Vec floor{eye[0] + t * d[0], eye[1] + t * d[1], 0};
            if (std::abs(floor[0]) > kFloorSize / 2 || std::abs(floor[1]) > kFloorSize / 2) {
                continue;
            }
            const auto c = clip(camera, floor);
            const float u = (c[0] + 1) / 2, v = (1 - c[1]) / 2;
            if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) {
                continue;
            }
            // Filtered between the four nearest texels.
            const float tx = u * n - 0.5f, ty = v * n - 0.5f;
            const int x0 = static_cast<int>(std::floor(tx)), y0 = static_cast<int>(std::floor(ty));
            const float fx = tx - static_cast<float>(x0), fy = ty - static_cast<float>(y0);
            const float a = (texel(x0, y0) * (1 - fx) + texel(x0 + 1, y0) * fx) * (1 - fy) +
                            (texel(x0, y0 + 1) * (1 - fx) + texel(x0 + 1, y0 + 1) * fx) * fy;
            const float s = std::clamp(a, 0.0f, 1.0f) * kStrength;
            if (s <= 0) {
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                pixel[k] = static_cast<std::uint8_t>(std::lround(pixel[k] * (1 - s)));
            }
            pixel[3] = static_cast<std::uint8_t>(std::lround(std::clamp(s * 255 + pixel[3] * (1 - s), 0.0f, 255.0f)));
        }
    }
}

} // namespace ts2::scene::familyshadow
