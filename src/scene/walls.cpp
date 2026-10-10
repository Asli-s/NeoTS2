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

// Coverings run left to right as seen from their side, once along each segment (a tile, or a tile's
// diagonal) and once over a full storey down from the wall's top, so short walls such as a deck's
// skirt show the top of it.
// A cut-down wall keeps the texture where the full wall has it, so door and window holes line up.
void addThickWall(SceneBuilder& builder, Surfaces& surfaces, const ThickWall& w, int edge, float cutHeight) {
    WallQuad q = w.q;
    const float dx = q.x1 - q.x0, dy = q.y1 - q.y0, len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0) {
        return;
    }
    const float ux = dx / len, uy = dy / len;
    const float v0 = (q.top0 - q.bottom0) / kLevelHeight, v1 = (q.top1 - q.bottom1) / kLevelHeight;
    float t0 = 0, t1 = 0;  // texture coordinates at the (cut) top
    if (cutHeight > 0) {
        const float top0 = std::min(q.top0, q.bottom0 + cutHeight), top1 = std::min(q.top1, q.bottom1 + cutHeight);
        t0 = (q.top0 - top0) / kLevelHeight;
        t1 = (q.top1 - top1) / kLevelHeight;
        q.top0 = top0;
        q.top1 = top1;
    }
    auto along = [&](const std::array<float, 2>& c) { return (c[0] - q.x0) * ux + (c[1] - q.y0) * uy; };
    // The wall graph's left side faces (dy, -dx).
    const std::array<float, 3> leftNormal{uy, -ux, 0}, rightNormal{-uy, ux, 0};
    // Past a segment's ends (where corners are mitred) a mask would wrap around to its other edge
    // and could open a slit there; masked sides keep to the segment's own part of the texture.
    auto u = [](float v, bool masked) { return masked ? std::clamp(v, 0.0f, 1.0f) : v; };
    if (const int m = w.left.empty() ? -1 : builder.material(w.left); m >= 0) {
        const bool masked = !w.leftMask.empty();
        const float u0 = u(along(w.leftA) / len, masked), u1 = u(along(w.leftB) / len, masked);
        addQuad(surfaces[m], {{{w.leftA[0], w.leftA[1], q.bottom0}, {w.leftB[0], w.leftB[1], q.bottom1},
                               {w.leftB[0], w.leftB[1], q.top1}, {w.leftA[0], w.leftA[1], q.top0}}},
                leftNormal, {{{u0, v0}, {u1, v1}, {u1, t1}, {u0, t0}}});
    }
    if (const int m = w.right.empty() ? -1 : builder.material(w.right); m >= 0) {
        const bool masked = !w.rightMask.empty();
        const float u0 = u(1 - along(w.rightA) / len, masked), u1 = u(1 - along(w.rightB) / len, masked);
        addQuad(surfaces[m], {{{w.rightA[0], w.rightA[1], q.bottom0}, {w.rightB[0], w.rightB[1], q.bottom1},
                               {w.rightB[0], w.rightB[1], q.top1}, {w.rightA[0], w.rightA[1], q.top0}}},
                rightNormal, {{{u0, v0}, {u1, v1}, {u1, t1}, {u0, t0}}});
    }
    if (!w.thick) {
        return;
    }
    const std::array<std::array<float, 2>, 4> noUv{};
    // A cut-down wall stops at doorways: its top in pieces between them, closed at their edges.
    std::vector<std::array<float, 2>> gaps;
    if (cutHeight > 0) {
        const float top = (t0 + t1) / 2, bottom = (v0 + v1) / 2;
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
    const float h = kWallThickness / 2;
    auto heightAt = [&](float a, float z0, float z1) { return z0 + (z1 - z0) * a / len; };
    auto across = [&](float a, float side) {
        return std::array<float, 2>{q.x0 + ux * a + side * uy * h, q.y0 + uy * a - side * ux * h};
    };
    // Each piece's top and its ends: the wall's own ends (where walls join they are inside the
    // walls, and show when the neighbor is cut down) or a doorway's sides.
    for (const auto& [s, e] : pieces) {
        const bool atA = s <= kTouch, atB = e >= len - kTouch;
        const auto l0 = atA ? w.leftA : across(s, 1), r0 = atA ? w.rightA : across(s, -1);
        const auto l1 = atB ? w.leftB : across(e, 1), r1 = atB ? w.rightB : across(e, -1);
        const float z0 = heightAt(s, q.top0, q.top1), z1 = heightAt(e, q.top0, q.top1);
        const float b0 = heightAt(s, q.bottom0, q.bottom1), b1 = heightAt(e, q.bottom0, q.bottom1);
        addQuad(surfaces[edge], {{{l0[0], l0[1], z0}, {l1[0], l1[1], z1}, {r1[0], r1[1], z1}, {r0[0], r0[1], z0}}}, {0, 0, 1}, noUv);
        addQuad(surfaces[edge], {{{l0[0], l0[1], b0}, {r0[0], r0[1], b0}, {r0[0], r0[1], z0}, {l0[0], l0[1], z0}}}, {-ux, -uy, 0}, noUv);
        addQuad(surfaces[edge], {{{l1[0], l1[1], b1}, {r1[0], r1[1], b1}, {r1[0], r1[1], z1}, {l1[0], l1[1], z1}}}, {ux, uy, 0}, noUv);
    }
}

} // namespace ts2::scene
