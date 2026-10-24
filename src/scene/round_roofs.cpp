#include "scene/lot_parts.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <cmath>

// Round roofs as the game builds them: cones (type 7), domes (8) and octagonal spires (9) are rings
// of points rising from the eaves, each ring one course of shingles. The first two rings hang under
// the eave line and make the edge board's two bands (trim, then edges). The rings above are the
// shingled top, and the underside is a fan from the top down to the lowest ring. Texture coordinates
// are in 511ths of the roof texture. Each course is a third of a texture cell high, the three
// courses taking turns, and runs one cell per tile around the roof.

namespace ts2::scene {

namespace {

constexpr float kCourse = 34 / 511.f;  // a course's height in the texture
constexpr float kTile = 102 / 511.f;   // a tile's width in the texture

// The texture row a ring's course takes: the edge bands the first, then the three in turn.
float courseTop(int ring) {
    return (510 - 34 * (ring < 2 ? 0 : ring % 3)) / 511.f;
}

// The vertices and faces one roof is made of. Faces name vertex, normal and texture entries.
struct RingMesh {
    std::vector<std::array<float, 3>> positions, normals;
    std::vector<std::array<float, 2>> uvs;

    // A face as the game passes it: three or four corners, split from the first.
    void face(Primitive& p, const std::array<int, 4>& v, const std::array<int, 4>& n, const std::array<int, 4>& t, int corners) const {
        for (int k = 1; k + 1 < corners; ++k) {
            const std::array<int, 3> c{0, k, k + 1};
            if (v[static_cast<std::size_t>(c[0])] == v[static_cast<std::size_t>(c[1])] ||
                v[static_cast<std::size_t>(c[1])] == v[static_cast<std::size_t>(c[2])] ||
                v[static_cast<std::size_t>(c[0])] == v[static_cast<std::size_t>(c[2])]) {
                continue;  // the top band's quads end in the one top point
            }
            for (const int i : c) {
                const auto u = static_cast<std::size_t>(i);
                addVertex(p, positions[static_cast<std::size_t>(v[u])], normals[static_cast<std::size_t>(n[u])], uvs[static_cast<std::size_t>(t[u])]);
            }
        }
    }
};

// A ring's height above the eave line: 0.2 per ring from the third, never above the top. The first
// two hang 0.25 and 0.0875 below it.
struct Rise {
    float height;
    float at = 0;  // where the shingled rings have got to
    float of(int ring) const { return at + (ring == 0 ? -0.25f : ring == 1 ? -0.0875f : 0.0f); }
    void next(int ring) {
        if (ring > 1) {
            at = std::clamp(at + 0.2f, 0.0f, height);
        }
    }
};

// A point's four texture entries: its ring's course from u0 to u1 tiles.
void addCourse(RingMesh& m, int ring, float u0, float u1) {
    const float top = courseTop(ring), bottom = top - kCourse;
    m.uvs.insert(m.uvs.end(), {{u0 * kTile, top}, {u1 * kTile, top}, {u1 * kTile, bottom}, {u0 * kTile, bottom}});
}

// The material a band of rings is drawn with.
int bandMaterial(const RoofLook& look, int band) {
    return band == 0 ? look.trim : band == 1 ? look.edges : look.top;
}

// A cone or a dome: `rings` rings of `seg` points around (cx, cy), the top one a single point. The
// course along each ring is cut where it would run past a tile, the last piece of a tile ending on
// it and the next starting afresh.
template <class RingFn>
void addRingRoof(Surfaces& surfaces, const RoofLook& look, int rings, int seg, float cx, float cy, RingFn ring) {
    RingMesh m;
    for (int k = 0; k < rings; ++k) {
        const auto [radius, z, normalOut, normalUp] = ring(k);
        const float arc = radius * 2 * kPi / static_cast<float>(seg);  // in tiles
        float u = 0, angle = 0;
        for (int j = 0; j < seg; ++j) {
            m.positions.push_back({cx + radius * std::cos(angle), cy + radius * std::sin(angle), z});
            m.normals.push_back({normalOut * std::cos(angle), normalOut * std::sin(angle), normalUp});
            if (u + arc > 1) {
                u = 1 - arc;
            }
            addCourse(m, k, u, u + arc);
            u = std::abs(u - (1 - arc)) < 1e-6f ? 0 : u + arc;
            angle += 2 * kPi / static_cast<float>(seg);
        }
    }
    for (int b = 0; b + 1 < rings; ++b) {
        const int material = bandMaterial(look, b);
        if (material < 0) {
            continue;
        }
        for (int j = 0; j < seg; ++j) {
            const int a = b * seg + j, c = b * seg + (j + 1) % seg;
            const std::array<int, 4> v{a, c, c + seg, a + seg};
            m.face(surfaces[material], v, v, {4 * a, 4 * a + 1, 4 * a + 2, 4 * a + 3}, 4);
        }
    }
    if (look.under >= 0) {
        const int top = (rings - 1) * seg;
        for (int j = 0; j < seg; ++j) {
            m.face(surfaces[look.under], {top, (j + 1) % seg, j, 0}, {top, top, top, 0}, {4 * top, 4 * top, 4 * top, 0}, 3);
        }
    }
}

// An octagonal spire: an octagon three tiles across (for a one-tile roof) grown by half a tile,
// shrinking ring by ring to a point, its sides cut at a tile from each corner. A narrow strip of the
// cap texture runs up each side of every corner.
void addSpire(Surfaces& surfaces, const RoofLook& look, float width, float height, float slope, float cx, float cy, float z0) {
    const float p = width - 1;
    const float a0 = (p - 2 * std::floor(p / 3)) * 0.5f, b0 = p * 0.5f, l = std::hypot(a0, b0);
    const float s = (0.5f + l) / l, a = s * a0, b = s * b0;
    const std::array<std::array<float, 2>, 8> corner{{{a, -b}, {b, -a}, {b, a}, {a, b}, {-a, b}, {-b, a}, {-b, -a}, {-a, -b}}};
    auto side = [&](int i) { return corner[static_cast<std::size_t>((i + 1) & 7)]; };
    std::array<int, 8> points{};
    std::array<int, 8> first{};
    int perRing = 0;
    for (int i = 0; i < 8; ++i) {
        const auto& c = corner[static_cast<std::size_t>(i)];
        const auto& d = side(i);
        points[static_cast<std::size_t>(i)] = static_cast<int>(std::ceil(std::hypot(d[0] - c[0], d[1] - c[1]))) + 1;
        first[static_cast<std::size_t>(i)] = perRing;
        perRing += points[static_cast<std::size_t>(i)];
    }
    const int steps = static_cast<int>(std::ceil(5 * height));
    const int rings = steps + 1;
    RingMesh m;
    for (int i = 0; i < 8; ++i) {  // each side's own normal, then one straight down
        const auto& c = corner[static_cast<std::size_t>(i)];
        const auto& d = side(i);
        const std::array<float, 3> n{height * (d[1] - c[1]), -height * (d[0] - c[0]), c[0] * d[1] - d[0] * c[1]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        m.normals.push_back({n[0] / len, n[1] / len, n[2] / len});
    }
    m.normals.push_back({0, 0, -1});
    Rise rise{height};
    for (int k = 0; k < rings; ++k) {
        const float f = k < 2 ? 1 : 1 - static_cast<float>(k - 2) / static_cast<float>(std::max(1, steps - 1));
        const float z = z0 + rise.of(k);
        for (int i = 0; i < 8; ++i) {
            const auto& c = corner[static_cast<std::size_t>(i)];
            const auto& d = side(i);
            const float len = f * std::hypot(d[0] - c[0], d[1] - c[1]);
            for (int j = 0; j < points[static_cast<std::size_t>(i)]; ++j) {
                const float t = len > 0 ? std::min(static_cast<float>(j), len) / len : 0;
                m.positions.push_back({cx + f * (c[0] + (d[0] - c[0]) * t), cy + f * (c[1] + (d[1] - c[1]) * t), z});
                addCourse(m, k, 0, std::min(len, 1.0f));
            }
        }
        rise.next(k);
    }
    const int apex = static_cast<int>(m.positions.size());
    m.positions.push_back({cx, cy, z0 + height});
    m.positions.push_back({cx, cy, z0 - 0.25f});  // the underside's center
    m.uvs.push_back({0, 0});
    // The corner strips: a tenth of a tile wide on each side of a corner, from the eave line up
    // toward a point over the center at the shingles' last course, stopping a quarter short.
    const std::array<float, 3> top{0, 0, rise.at};
    const int strip = static_cast<int>(m.uvs.size());
    m.uvs.insert(m.uvs.end(), {{0, kTile}, {51 / 511.f, kTile}, {153 / 511.f, 0}, {kTile, 0}});
    const int pieces = static_cast<int>(std::ceil(height * slope));
    std::vector<std::array<int, 3>> strips;  // first vertex, side, which way
    for (int i = 0; i < 8; ++i) {
        const auto& c = corner[static_cast<std::size_t>(i)];
        const auto& d = side(i);
        const float dl = std::hypot(d[0] - c[0], d[1] - c[1]);
        const std::array<float, 2> dir{(d[0] - c[0]) / dl * 0.1f, (d[1] - c[1]) / dl * 0.1f};
        const std::array<float, 3> v{top[0] - c[0], top[1] - c[1], top[2]}, w{top[0] - d[0], top[1] - d[1], top[2]};
        const float wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        const std::array<float, 3> e{top[0] - 0.1f * w[0] / wl - c[0] - dir[0], top[1] - 0.1f * w[1] / wl - c[1] - dir[1],
                                     top[2] - 0.1f * w[2] / wl};
        const float stop = (std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) - 0.25f) / wl;
        auto run = [&](const std::array<float, 2>& p0, const std::array<float, 2>& p1, const std::array<float, 3>& up, int way) {
            strips.push_back({static_cast<int>(m.positions.size()), i, way});
            for (int j = 0; j <= pieces; ++j) {
                const float t = std::min(static_cast<float>(j) / static_cast<float>(pieces), stop);
                m.positions.push_back({cx + p0[0] + up[0] * t, cy + p0[1] + up[1] * t, z0 + up[2] * t});
                m.positions.push_back({cx + p1[0] + up[0] * t, cy + p1[1] + up[1] * t, z0 + up[2] * t});
            }
        };
        run(c, {c[0] + dir[0], c[1] + dir[1]}, v, 0);
        run({d[0] - dir[0], d[1] - dir[1]}, d, w, 1);
    }

    for (int band = 0; band < rings; ++band) {
        const int material = bandMaterial(look, band);
        if (material < 0) {
            continue;
        }
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j + 1 < points[static_cast<std::size_t>(i)]; ++j) {
                const int lo = band * perRing + first[static_cast<std::size_t>(i)] + j;
                const int hi = band + 1 < rings ? lo + perRing : apex;
                const std::array<int, 4> v{lo, lo + 1, band + 1 < rings ? hi + 1 : apex, hi};
                m.face(surfaces[material], v, {i, i, i, i}, {4 * lo, 4 * lo + 1, 4 * lo + 2, 4 * lo + 3}, 4);
            }
        }
    }
    if (look.under >= 0) {
        for (int i = 0; i < 8; ++i) {
            const int f = first[static_cast<std::size_t>(i)];
            m.face(surfaces[look.under], {apex + 1, f, f + points[static_cast<std::size_t>(i)] - 1, 0}, {8, 8, 8, 0},
                   {strip + 3, strip + 3, strip + 3, 0}, 3);
        }
    }
    if (look.atlas >= 0) {
        for (const auto& [v0, i, way] : strips) {
            const std::array<int, 4> t = way == 0 ? std::array<int, 4>{strip, strip + 1, strip + 2, strip + 3}
                                                  : std::array<int, 4>{strip + 1, strip, strip + 3, strip + 2};
            for (int j = 0; j < pieces; ++j) {
                const int v = v0 + 2 * j;
                m.face(surfaces[look.atlas], {v, v + 1, v + 3, v + 2}, {i, i, i, i}, t, 4);
            }
        }
    }
}

}  // namespace

bool addRoundRoof(Surfaces& surfaces, const roofs::Roof& roof, float eave, const RoofLook& look, float lotWidth, float lotHeight) {
    if (roof.type < 7 || roof.type > 9) {
        return false;
    }
    float lx = std::min(roof.x0, roof.x1), hx = std::max(roof.x0, roof.x1);
    float ly = std::min(roof.y0, roof.y1), hy = std::max(roof.y0, roof.y1);
    if (roof.type == 9) {
        // A spire stands over a tile more on each side, within the lot.
        lx = std::max(0.0f, lx - 1);
        ly = std::max(0.0f, ly - 1);
        hx = std::min(lotWidth, hx + 1);
        hy = std::min(lotHeight, hy + 1);
    }
    lx -= 0.5f;
    ly -= 0.5f;
    hx += 0.5f;
    hy += 0.5f;
    const float cx = (lx + hx) / 2, cy = (ly + hy) / 2, z0 = eave - 0.42f;
    const float radius = std::hypot(hx - lx, hy - ly) * 0.5f;
    const float slope = roof.slope > 0 ? roof.slope : 0.5f;
    const float around = std::ceil(2 * kPi * radius);
    // A broken file's roof could ask for millions of rings. Such a roof is left out.
    constexpr float kMaxVertices = 1 << 20;
    const float rings = 5 * std::max(radius, radius * slope) + 3;
    if (!(rings * std::max(around, 48.0f) <= kMaxVertices && radius * slope * slope <= kMaxVertices)) {
        diag::warnOnce("round roofs: a round roof needing more than 1048576 vertices is left out");
        return true;
    }
    const int seg = around < 49 ? 48 : static_cast<int>(around);
    if (roof.type == 9) {
        addSpire(surfaces, look, hx - lx, radius * slope, slope, cx, cy, z0);
    } else if (roof.type == 7) {
        const float height = radius * slope, len = std::hypot(height, radius);
        Rise rise{height};
        addRingRoof(surfaces, look, static_cast<int>(std::ceil(5 * height)) + 3, seg, cx, cy, [&](int k) {
            const float r = radius * (height - rise.at) / height, z = z0 + rise.of(k);
            rise.next(k);
            return std::array<float, 4>{r, z, height / len, radius / len};
        });
    } else {
        // A half sphere, rings every equal angle from the eaves to the top.
        const int steps = static_cast<int>(std::ceil(5 * radius));
        addRingRoof(surfaces, look, steps + 3, seg, cx, cy, [&](int k) {
            const float down = k < 2 ? kPi / 2 : kPi / 2 - static_cast<float>(k - 2) * (kPi / 2) / static_cast<float>(steps);
            const float z = z0 + radius * std::cos(down) + (k == 0 ? -0.25f : k == 1 ? -0.0875f : 0.0f);
            return std::array<float, 4>{radius * std::sin(down), z, std::sin(down), std::cos(down)};
        });
    }
    return true;
}

} // namespace ts2::scene
