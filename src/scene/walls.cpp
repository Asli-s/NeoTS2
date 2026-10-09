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
                w.openA = arms.size() == 1;
            } else {
                w.leftB = ccw;
                w.rightB = cw;
                w.openB = arms.size() == 1;
            }
            hub.x = px;
            hub.y = py;
            hub.top = a.end == 0 ? w.q.top0 : w.q.top1;
            hub.corners.push_back(cw);
            hub.corners.push_back(ccw);
        }
        if (arms.size() >= 3) {
            hubs.push_back(std::move(hub));
        }
    }
    return hubs;
}

// Coverings run left to right as seen from their side, once per tile along the wall and once over
// a full storey down from the wall's top, so short walls such as a deck's skirt show the top of it.
void addThickWall(SceneBuilder& builder, Surfaces& surfaces, const ThickWall& w, int edge) {
    const auto& q = w.q;
    const float dx = q.x1 - q.x0, dy = q.y1 - q.y0, len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0) {
        return;
    }
    const float ux = dx / len, uy = dy / len;
    const float v0 = (q.top0 - q.bottom0) / kLevelHeight, v1 = (q.top1 - q.bottom1) / kLevelHeight;
    auto along = [&](const std::array<float, 2>& c) { return (c[0] - q.x0) * ux + (c[1] - q.y0) * uy; };
    // The wall graph's left side faces (dy, -dx).
    const std::array<float, 3> leftNormal{uy, -ux, 0}, rightNormal{-uy, ux, 0};
    if (const int m = w.left.empty() ? -1 : builder.material(w.left); m >= 0) {
        const float u0 = along(w.leftA), u1 = along(w.leftB);
        addQuad(surfaces[m], {{{w.leftA[0], w.leftA[1], q.bottom0}, {w.leftB[0], w.leftB[1], q.bottom1},
                               {w.leftB[0], w.leftB[1], q.top1}, {w.leftA[0], w.leftA[1], q.top0}}},
                leftNormal, {{{u0, v0}, {u1, v1}, {u1, 0}, {u0, 0}}});
    }
    if (const int m = w.right.empty() ? -1 : builder.material(w.right); m >= 0) {
        const float u0 = len - along(w.rightA), u1 = len - along(w.rightB);
        addQuad(surfaces[m], {{{w.rightA[0], w.rightA[1], q.bottom0}, {w.rightB[0], w.rightB[1], q.bottom1},
                               {w.rightB[0], w.rightB[1], q.top1}, {w.rightA[0], w.rightA[1], q.top0}}},
                rightNormal, {{{u0, v0}, {u1, v1}, {u1, 0}, {u0, 0}}});
    }
    if (!w.thick) {
        return;
    }
    const std::array<std::array<float, 2>, 4> noUv{};
    addQuad(surfaces[edge], {{{w.leftA[0], w.leftA[1], q.top0}, {w.leftB[0], w.leftB[1], q.top1},
                              {w.rightB[0], w.rightB[1], q.top1}, {w.rightA[0], w.rightA[1], q.top0}}},
            {0, 0, 1}, noUv);
    if (w.openA) {
        addQuad(surfaces[edge], {{{w.leftA[0], w.leftA[1], q.bottom0}, {w.rightA[0], w.rightA[1], q.bottom0},
                                  {w.rightA[0], w.rightA[1], q.top0}, {w.leftA[0], w.leftA[1], q.top0}}},
                {-ux, -uy, 0}, noUv);
    }
    if (w.openB) {
        addQuad(surfaces[edge], {{{w.leftB[0], w.leftB[1], q.bottom1}, {w.rightB[0], w.rightB[1], q.bottom1},
                                  {w.rightB[0], w.rightB[1], q.top1}, {w.leftB[0], w.leftB[1], q.top1}}},
                {ux, uy, 0}, noUv);
    }
}

} // namespace ts2::scene
