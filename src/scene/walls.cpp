#include "scene/lot_parts.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>

namespace ts2::scene {

namespace {

// Where the side line of one wall (offset `h` along normal n, running along d) meets that of
// another (normal m, along e), both starting from the same point; square when they are parallel.
std::array<float, 2> meet(float px, float py, float dx, float dy, float nx, float ny, float ex, float ey, float mx,
                          float my, float h) {
    const float rx = h * (mx - nx), ry = h * (my - ny);
    const float cross = dx * ey - dy * ex;
    float s = 0;
    if (std::abs(cross) > 1e-4f) {
        s = std::clamp((rx * ey - ry * ex) / cross, -4 * h, 4 * h);
    }
    return {px + h * nx + s * dx, py + h * ny + s * dy};
}

// The spans of a face (as fractions of the segment across its texture) where its material's texture is a
// hole all the way from texture row v0 down to v1: a doorway through a cut-down wall.
std::vector<std::array<float, 2>> holes(SceneBuilder& builder, const std::string& material, float v0, float v1) {
    std::vector<std::array<float, 2>> spans;
    const int m = material.empty() ? -1 : builder.material(material);
    if (m < 0) {
        return spans;
    }
    const auto& mat = builder.scene().materials[static_cast<std::size_t>(m)];
    if (mat.alpha != AlphaMode::Mask || mat.image < 0) {
        return spans;
    }
    const Image& image = builder.scene().images[static_cast<std::size_t>(mat.image)];
    const auto w = static_cast<int>(image.width), h = static_cast<int>(image.height);
    const int r0 = std::clamp(static_cast<int>(v0 * static_cast<float>(h)), 0, h - 1);
    const int r1 = std::clamp(static_cast<int>(std::ceil(v1 * static_cast<float>(h))) - 1, r0, h - 1);
    int start = -1;
    for (int c = 0; c <= w; ++c) {
        bool hole = c < w;
        for (int r = r0; hole && r <= r1; ++r) {
            hole = image.rgba[(static_cast<std::size_t>(r) * image.width + static_cast<std::size_t>(c)) * 4 + 3] < 128;
        }
        if (hole && start < 0) {
            start = c;
        } else if (!hole && start >= 0) {
            spans.push_back({static_cast<float>(start) / static_cast<float>(w), static_cast<float>(c) / static_cast<float>(w)});
            start = -1;
        }
    }
    return spans;
}

} // namespace

std::map<std::string, std::vector<WallMask>> wallMasks(const std::vector<std::string>& materialNames) {
    static const std::regex pattern(R"(^(.+)_(0|\d+[ew])_(0|\d+[ns])_(\d+)_(n|s|e|w|ne|nw|se|sw)_wallmask_txmt$)",
                                    std::regex::icase);
    auto offset = [](const std::string& t, char positive) {
        const int n = std::stoi(t);
        return t.size() > 1 && std::tolower(static_cast<unsigned char>(t.back())) != positive ? -n : n;
    };
    std::map<std::string, std::vector<WallMask>> masks;
    for (const auto& name : materialNames) {
        std::smatch m;
        if (std::regex_match(name, m, pattern)) {
            const std::string material = name.substr(0, name.size() - 5);  // without "_txmt"
            masks[lower(m[1].str())].push_back(
                {offset(m[2].str(), 'e'), offset(m[3].str(), 's'), std::stoi(m[4].str()), lower(m[5].str()), material});
        }
    }
    return masks;
}

std::vector<WallHub> joinWalls(std::vector<ThickWall>& walls) {
    std::vector<WallHub> hubs;
    const float h = kWallThickness / 2;
    std::map<std::int32_t, std::vector<std::pair<std::size_t, int>>> atPoint;
    for (std::size_t i = 0; i < walls.size(); ++i) {
        auto& w = walls[i];
        if (!w.thick) {
            w.leftA = w.rightA = {w.q.x0, w.q.y0};
            w.leftB = w.rightB = {w.q.x1, w.q.y1};
            continue;
        }
        atPoint[w.points[0]].push_back({i, 0});
        atPoint[w.points[1]].push_back({i, 1});
    }
    struct Arm {
        std::size_t wall;
        int end;
        float dx, dy, angle;
    };
    for (const auto& [point, list] : atPoint) {
        std::vector<Arm> arms;
        for (const auto& [i, end] : list) {
            const auto& q = walls[i].q;
            float dx = q.x1 - q.x0, dy = q.y1 - q.y0;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len <= 0) {
                continue;
            }
            dx = (end == 0 ? dx : -dx) / len;
            dy = (end == 0 ? dy : -dy) / len;
            arms.push_back({i, end, dx, dy, std::atan2(dy, dx)});
        }
        std::sort(arms.begin(), arms.end(), [](const Arm& x, const Arm& y) { return x.angle < y.angle; });
        WallHub hub;
        for (std::size_t k = 0; k < arms.size(); ++k) {
            const Arm& a = arms[k];
            const Arm& next = arms[(k + 1) % arms.size()];
            const Arm& prev = arms[(k + arms.size() - 1) % arms.size()];
            ThickWall& w = walls[a.wall];
            const float px = a.end == 0 ? w.q.x0 : w.q.x1, py = a.end == 0 ? w.q.y0 : w.q.y1;
            // The counterclockwise side of an arm is the wall's right side at its start and its
            // left side at its end.
            const auto ccw = meet(px, py, a.dx, a.dy, -a.dy, a.dx, next.dx, next.dy, next.dy, -next.dx, h);
            const auto cw = meet(px, py, a.dx, a.dy, a.dy, -a.dx, prev.dx, prev.dy, -prev.dy, prev.dx, h);
            if (a.end == 0) {
                w.rightA = ccw;
                w.leftA = cw;
            } else {
                w.leftB = ccw;
                w.rightB = cw;
            }
            hub.x = px;
            hub.y = py;
            hub.top = a.end == 0 ? w.q.top0 : w.q.top1;
            hub.bottom = a.end == 0 ? w.q.bottom0 : w.q.bottom1;
            hub.walls.push_back(a.wall);
            hub.corners.push_back(cw);
            hub.corners.push_back(ccw);
        }
        if (arms.size() >= 3) {
            hubs.push_back(std::move(hub));
        }
    }
    return hubs;
}

float WallCeiling::at(float x, float y, float top) const {
    if (gables.empty()) {
        return top;
    }
    float z = 1e9f;
    for (const auto* g : gables) {
        z = std::min(z, g->height(x, y) - g->thickness);  // up to the roof's underside
    }
    // Near the eaves the underside is below the wall's own top, which then stays inside the slab.
    return std::max(top, z);
}

std::vector<float> WallCeiling::bends(const std::array<float, 2>& p, const std::array<float, 2>& q) const {
    std::vector<float> ts{0, 1};
    for (const auto* r : gables) {
        const auto more = r->bends(p, q);
        ts.insert(ts.end(), more.begin(), more.end());
    }
    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end(), [](float a, float b) { return b - a < 1e-4f; }), ts.end());
    return ts;
}

// Coverings run left to right as seen from their side, once along each segment (a tile, or a tile's
// diagonal) and once over a full storey down from the wall's own top, so short walls such as a
// deck's skirt show the top of it, and the covering carries on where the wall is cut down or raised.
void addThickWall(SceneBuilder& builder, Surfaces& surfaces, const ThickWall& w, int edge, float cutHeight) {
    const WallQuad& q = w.q;
    const float dx = q.x1 - q.x0, dy = q.y1 - q.y0, len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0) {
        return;
    }
    const float ux = dx / len, uy = dy / len;
    const float h = kWallThickness / 2;
    auto along = [&](const std::array<float, 2>& c) { return (c[0] - q.x0) * ux + (c[1] - q.y0) * uy; };
    auto across = [&](float a, float side) {
        return std::array<float, 2>{q.x0 + ux * a + side * uy * h, q.y0 + uy * a - side * ux * h};
    };
    auto lerp = [&](const std::array<float, 2>& c, float z0, float z1) { return z0 + (z1 - z0) * std::clamp(along(c) / len, 0.0f, 1.0f); };
    auto bottomAt = [&](const std::array<float, 2>& c) { return lerp(c, q.bottom0, q.bottom1); };
    auto ownTopAt = [&](const std::array<float, 2>& c) { return lerp(c, q.top0, q.top1); };
    const bool cut = cutHeight > 0;
    auto topAt = [&](const std::array<float, 2>& c) {
        const float own = ownTopAt(c);
        return cut ? std::min(own, bottomAt(c) + cutHeight) : std::max(bottomAt(c), w.ceiling.at(c[0], c[1], own));
    };
    auto v = [&](const std::array<float, 2>& c, float z) { return (ownTopAt(c) - z) / kLevelHeight; };
    // Positions along the wall where its top bends (as distances along it).
    std::vector<float> bends;
    if (!cut) {
        for (const float f : w.ceiling.bends({q.x0, q.y0}, {q.x1, q.y1})) {
            bends.push_back(f * len);
        }
    }
    // A line along the wall from a to b: there, and wherever the top bends in between.
    auto points = [&](const std::array<float, 2>& a, const std::array<float, 2>& b) {
        std::vector<std::array<float, 2>> pts{a};
        const float ta = along(a), tb = along(b);
        for (const float t : bends) {
            if (t > ta + 1e-4f && t < tb - 1e-4f) {
                const float k = (t - ta) / (tb - ta);
                pts.push_back({a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k});
            }
        }
        pts.push_back(b);
        return pts;
    };

    // The wall graph's left side faces (dy, -dx).
    const std::array<float, 3> leftNormal{uy, -ux, 0}, rightNormal{-uy, ux, 0};
    for (const bool left : {true, false}) {
        const std::string& name = left ? w.left : w.right;
        const int m = name.empty() ? -1 : builder.material(name);
        if (m < 0) {
            continue;
        }
        // Raised past its own top (closing a gable), a wall shows its covering without door and window holes.
        const std::string& plain = left ? w.leftCovering : w.rightCovering;
        const int above = plain.empty() ? m : builder.material(plain);
        // Past a segment's ends (where corners are mitred) a mask would wrap around to its other
        // edge and could open a slit there; masked sides keep to the segment's own part of the texture.
        const bool masked = !(left ? w.leftMask : w.rightMask).empty();
        auto u = [&](const std::array<float, 2>& c) {
            const float t = left ? along(c) / len : 1 - along(c) / len;
            return masked ? std::clamp(t, 0.0f, 1.0f) : t;
        };
        const auto pts = left ? points(w.leftA, w.leftB) : points(w.rightA, w.rightB);
        for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
            const auto& p = pts[k];
            const auto& n = pts[k + 1];
            const float bp = bottomAt(p), bn = bottomAt(n), tp = topAt(p), tn = topAt(n);
            const float op = ownTopAt(p), on = ownTopAt(n);
            // Up to the wall's own top, then any part above it.
            for (const auto& [z0p, z0n, z1p, z1n, material] :
                 {std::tuple{bp, bn, std::min(tp, op), std::min(tn, on), m}, std::tuple{op, on, std::max(tp, op), std::max(tn, on), above}}) {
                if (z1p <= z0p + 1e-4f && z1n <= z0n + 1e-4f) {
                    continue;
                }
                addQuad(surfaces[material], {{{p[0], p[1], z0p}, {n[0], n[1], z0n}, {n[0], n[1], z1n}, {p[0], p[1], z1p}}},
                        left ? leftNormal : rightNormal,
                        {{{u(p), v(p, z0p)}, {u(n), v(n, z0n)}, {u(n), v(n, z1n)}, {u(p), v(p, z1p)}}});
            }
        }
    }
    if (!w.thick) {
        return;
    }
    const std::array<std::array<float, 2>, 4> noUv{};
    // A cut-down wall stops at doorways: its top in pieces between them, closed at their edges.
    std::vector<std::array<float, 2>> gaps;
    if (cut) {
        const std::array<float, 2> a{q.x0, q.y0}, b{q.x1, q.y1};
        const float top = (v(a, topAt(a)) + v(b, topAt(b))) / 2, bottom = (v(a, q.bottom0) + v(b, q.bottom1)) / 2;
        for (const auto& g : holes(builder, w.left, top, bottom)) {
            gaps.push_back({g[0] * len, g[1] * len});
        }
        for (const auto& g : holes(builder, w.right, top, bottom)) {
            gaps.push_back({(1 - g[1]) * len, (1 - g[0]) * len});
        }
        std::sort(gaps.begin(), gaps.end());
    }
    // The pieces of wall left between the doorways (the whole wall when there are none).
    std::vector<std::array<float, 2>> pieces;
    float from = 0;
    constexpr float kTouch = 0.01f;  // a doorway this close to an end reaches it
    for (const auto& g : gaps) {
        if (g[0] > from + kTouch) {
            pieces.push_back({from, g[0]});
        }
        from = std::max(from, g[1]);
    }
    if (from < len - kTouch) {
        pieces.push_back({from, len});
    }
    // Each piece's top and its ends: the wall's own ends (where walls join they are inside the
    // walls, and show when the neighbor is cut down) or a doorway's sides.
    for (const auto& [s, e] : pieces) {
        const bool atA = s <= kTouch, atB = e >= len - kTouch;
        std::vector<std::array<float, 2>> l{atA ? w.leftA : across(s, 1)}, r{atA ? w.rightA : across(s, -1)};
        for (const float t : bends) {
            if (t > s + 1e-4f && t < e - 1e-4f) {
                l.push_back(across(t, 1));
                r.push_back(across(t, -1));
            }
        }
        l.push_back(atB ? w.leftB : across(e, 1));
        r.push_back(atB ? w.rightB : across(e, -1));
        for (std::size_t k = 0; k + 1 < l.size(); ++k) {
            addQuad(surfaces[edge], {{{l[k][0], l[k][1], topAt(l[k])}, {l[k + 1][0], l[k + 1][1], topAt(l[k + 1])},
                                      {r[k + 1][0], r[k + 1][1], topAt(r[k + 1])}, {r[k][0], r[k][1], topAt(r[k])}}},
                    {0, 0, 1}, noUv);
        }
        for (const auto& [lp, rp, out] : {std::tuple{l.front(), r.front(), -1.0f}, std::tuple{l.back(), r.back(), 1.0f}}) {
            addQuad(surfaces[edge], {{{lp[0], lp[1], bottomAt(lp)}, {rp[0], rp[1], bottomAt(rp)}, {rp[0], rp[1], topAt(rp)},
                                      {lp[0], lp[1], topAt(lp)}}},
                    {out * ux, out * uy, 0}, noUv);
        }
    }
}

} // namespace ts2::scene
