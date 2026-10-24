#include "scene/ground_decal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ts2::scene {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

using Corner = std::array<float, 3>;

// A triangle cut by up to four lines: each cut adds at most one corner, so 7 fit.
struct Polygon {
    std::array<Corner, 8> corners{};
    std::size_t size = 0;
    void add(const Corner& c) {
        if (size < corners.size()) {
            corners[size++] = c;
        }
    }
};

// The part of a polygon on the side of a line where nx x + ny y <= limit.
Polygon clipped(const Polygon& in, float nx, float ny, float limit) {
    Polygon out;
    for (std::size_t i = 0; i < in.size; ++i) {
        const Corner& a = in.corners[i];
        const Corner& b = in.corners[(i + 1) % in.size];
        const float da = nx * a[0] + ny * a[1] - limit, db = nx * b[0] + ny * b[1] - limit;
        if (da <= 0) {
            out.add(a);
        }
        if ((da < 0) != (db < 0) && da != db) {
            const float t = da / (da - db);
            out.add({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t});
        }
    }
    return out;
}

} // namespace

Primitive groundDecal(const PaintedTerrain& t, const std::array<float, 2>& centre, float width, float turns, int material) {
    Primitive p;
    p.material = material;
    // Nothing for a broken terrain, or a square that isn't a finite size within the terrain's reach
    // (the tile numbers below must fit their integers).
    const float tile = t.tileSize;
    const float extent = tile * static_cast<float>(std::max(t.columns, t.rows));
    if (t.columns < 2 || t.rows < 2 || t.heights.size() < static_cast<std::size_t>(t.columns) * t.rows || !(tile > 0) ||
        !std::isfinite(extent) || !(width >= kMinGroundDecalWidth) || width > kMaxGroundDecalWidth || !std::isfinite(turns) ||
        !(std::abs(centre[0]) < 2 * extent) || !(std::abs(centre[1]) < 2 * extent)) {
        return p;
    }
    const float half = width / 2;
    const float c = std::cos(turns * kTwoPi), s = std::sin(turns * kTwoPi);
    const std::array<float, 2> right{c, s}, up{-s, c};
    const float reach = half * 1.5f;  // past the corners of the turned square
    // A tile number, held within the terrain before it becomes an integer.
    const auto first = [&](float v) {
        return static_cast<std::int64_t>(std::clamp(std::floor(static_cast<double>(v) / tile), -1.0, static_cast<double>(t.columns) + t.rows));
    };
    const std::int64_t x0 = std::max<std::int64_t>(0, first(centre[0] - reach));
    const std::int64_t y0 = std::max<std::int64_t>(0, first(centre[1] - reach));
    const std::int64_t x1 = std::min<std::int64_t>(static_cast<std::int64_t>(t.columns) - 2, first(centre[0] + reach));
    const std::int64_t y1 = std::min<std::int64_t>(static_cast<std::int64_t>(t.rows) - 2, first(centre[1] + reach));
    const auto corner = [&](std::int64_t x, std::int64_t y) {
        return Corner{static_cast<float>(x) * tile, static_cast<float>(y) * tile,
                      t.heights[static_cast<std::size_t>(y) * t.columns + static_cast<std::size_t>(x)]};
    };
    // The square's four sides, as lines its inside is below.
    const float alongRight = right[0] * centre[0] + right[1] * centre[1];
    const float alongUp = up[0] * centre[0] + up[1] * centre[1];
    for (std::int64_t y = y0; y <= y1; ++y) {
        for (std::int64_t x = x0; x <= x1; ++x) {
            const Corner a = corner(x, y), b = corner(x + 1, y), e = corner(x + 1, y + 1), d = corner(x, y + 1);
            for (const auto& triangle : {Polygon{{a, b, e}, 3}, Polygon{{a, e, d}, 3}}) {
                auto poly = clipped(triangle, right[0], right[1], alongRight + half);
                poly = clipped(poly, -right[0], -right[1], half - alongRight);
                poly = clipped(poly, up[0], up[1], alongUp + half);
                poly = clipped(poly, -up[0], -up[1], half - alongUp);
                if (poly.size < 3) {
                    continue;
                }
                const auto uv = [&](const Corner& q) {
                    const float dx = q[0] - centre[0], dy = q[1] - centre[1];
                    return std::array<float, 2>{(dx * right[0] + dy * right[1]) / (2 * half) + 0.5f,
                                                0.5f + (dx * up[0] + dy * up[1]) / (2 * half)};
                };
                for (std::size_t k = 1; k + 1 < poly.size; ++k) {
                    for (const Corner& q : {poly.corners[0], poly.corners[k], poly.corners[k + 1]}) {
                        const auto texel = uv(q);
                        p.indices.push_back(static_cast<std::uint32_t>(p.positions.size() / 3));
                        p.positions.insert(p.positions.end(), q.begin(), q.end());
                        p.normals.insert(p.normals.end(), {0.0f, 0.0f, 1.0f});
                        p.uvs.insert(p.uvs.end(), texel.begin(), texel.end());
                    }
                }
            }
        }
    }
    return p;
}

} // namespace ts2::scene
