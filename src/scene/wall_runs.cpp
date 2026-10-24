#include "scene/wall_runs.hpp"

#include "content/lot.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace ts2::scene::wallruns {

namespace {

constexpr int kMaxCoordinate = 256;
constexpr int kMaxLevel = 64;
constexpr std::size_t kMaxFaces = 4096;  // one-tile wall faces, as the wall light allows segments

// One side of a one-tile wall, walked from (x0, y0) to (x1, y1). It faces (dy, -dx).
struct Face {
    int level, x0, y0, x1, y1;
    bool used = false;
};

using Point = std::tuple<int, int, int>;  // level, x, y

Key keyOf(const Face& f) {
    const int dx = f.x1 - f.x0, dy = f.y1 - f.y0;
    const int nx = dy, ny = -dx;
    Key k;
    k.level = f.level;
    if (dx != 0 && dy != 0) {
        k.x = std::min(f.x0, f.x1);
        k.y = std::min(f.y0, f.y1);
        if (dx * dy < 0) {
            k.side = 0x10;
            k.part = nx > 0 ? 4 : 2;
        } else {
            k.side = 0x20;
            k.part = nx > 0 ? 1 : 3;
        }
    } else if (dx == 0) {
        k.y = std::min(f.y0, f.y1);
        k.x = nx > 0 ? f.x0 : f.x0 - 1;
        k.side = nx > 0 ? 2 : 8;
    } else {
        k.x = std::min(f.x0, f.x1);
        k.y = ny > 0 ? f.y0 : f.y0 - 1;
        k.side = ny > 0 ? 1 : 4;
    }
    return k;
}

// The point a face's room is read at: a third of a tile from the key tile's middle toward the
// part of the tile the face looks into.
std::int16_t roomOf(const Key& k, const RoomAt& roomAt) {
    if (!roomAt) {
        return 0;
    }
    const int toward = quarters(k);
    float x = static_cast<float>(k.x) + 0.5f, y = static_cast<float>(k.y) + 0.5f;
    if (toward & 1) x -= 1.0f / 3;
    if (toward & 2) y += 1.0f / 3;
    if (toward & 4) x += 1.0f / 3;
    if (toward & 8) y -= 1.0f / 3;
    return roomAt(k.level, x, y);
}

// How far the walk turns from direction a to b, toward the side faces look at first: the next
// face round a room is the one turning most that way. Going back is the last choice.
double turn(int ax, int ay, int bx, int by) {
    const int cross = ax * by - ay * bx, dot = ax * bx + ay * by;
    return std::atan2(static_cast<double>(cross), static_cast<double>(dot));
}

// Every one-tile face of the walls, both sides, once each. False when some were left out.
bool facesOf(const std::vector<Wall>& walls, std::vector<Face>& faces) {
    std::set<std::tuple<int, int, int, int, int>> seen;
    bool complete = true;
    for (const auto& w : walls) {
        const int dx = w.x1 - w.x0, dy = w.y1 - w.y0;
        const int steps = std::max(std::abs(dx), std::abs(dy));
        const bool inRange = w.level >= -kMaxLevel && w.level <= kMaxLevel && std::min({w.x0, w.y0, w.x1, w.y1}) >= 0 &&
                             std::max({w.x0, w.y0, w.x1, w.y1}) <= kMaxCoordinate;
        if (!inRange || steps == 0 || (dx != 0 && dy != 0 && std::abs(dx) != std::abs(dy))) {
            if (!inRange) {
                diag::warnOnce("wall runs: a wall past 256 tiles or 64 levels is left out");
            }
            continue;
        }
        const int sx = (dx > 0) - (dx < 0), sy = (dy > 0) - (dy < 0);
        for (int i = 0; i < steps; ++i) {
            int ax = w.x0 + i * sx, ay = w.y0 + i * sy, bx = ax + sx, by = ay + sy;
            if (std::tie(bx, by) < std::tie(ax, ay)) {
                std::swap(ax, bx);
                std::swap(ay, by);
            }
            if (!seen.insert({w.level, ax, ay, bx, by}).second) {
                continue;
            }
            if (faces.size() + 2 > kMaxFaces) {
                complete = false;
                return complete;
            }
            faces.push_back({w.level, ax, ay, bx, by});
            faces.push_back({w.level, bx, by, ax, ay});
        }
    }
    return complete;
}

// The faces in order round each room, each loop starting where the walk changes direction.
std::vector<std::vector<std::size_t>> loopsOf(std::vector<Face>& faces) {
    std::map<Point, std::vector<std::size_t>> leaving;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        leaving[{faces[i].level, faces[i].x0, faces[i].y0}].push_back(i);
    }
    std::vector<std::vector<std::size_t>> loops;
    for (std::size_t start = 0; start < faces.size(); ++start) {
        if (faces[start].used) {
            continue;
        }
        std::vector<std::size_t> loop;
        std::size_t at = start;
        while (!faces[at].used && loop.size() < faces.size()) {
            faces[at].used = true;
            loop.push_back(at);
            const Face& f = faces[at];
            const int dx = f.x1 - f.x0, dy = f.y1 - f.y0;
            std::size_t next = at;
            double best = 0;
            const auto out = leaving.find({f.level, f.x1, f.y1});
            if (out == leaving.end()) {
                break;
            }
            for (std::size_t c : out->second) {
                const Face& g = faces[c];
                const double t = turn(dx, dy, g.x1 - g.x0, g.y1 - g.y0);
                if (next == at || t < best) {
                    next = c;
                    best = t;
                }
            }
            at = next;
        }
        // Start at a corner so no straight piece is cut in two.
        auto corner = [&](std::size_t i) {
            const Face& a = faces[loop[(i + loop.size() - 1) % loop.size()]];
            const Face& b = faces[loop[i]];
            return a.x1 - a.x0 != b.x1 - b.x0 || a.y1 - a.y0 != b.y1 - b.y0;
        };
        for (std::size_t i = 0; i < loop.size(); ++i) {
            if (corner(i)) {
                std::rotate(loop.begin(), loop.begin() + static_cast<std::ptrdiff_t>(i), loop.end());
                break;
            }
        }
        loops.push_back(std::move(loop));
    }
    return loops;
}

} // namespace

Key keyOf(int level, int x0, int y0, int x1, int y1) {
    return keyOf(Face{level, x0, y0, x1, y1});
}

std::int16_t roomInFront(const Key& key, const RoomAt& roomAt) {
    return roomOf(key, roomAt);
}

int quarters(const Key& key) {
    switch (key.side) {
    case 1: return 8;
    case 2: return 1;
    case 4: return 2;
    case 8: return 4;
    case 0x10: return key.part == 2 ? 9 : 6;
    case 0x20: return key.part == 1 ? 0xC : 3;
    default: return 0;
    }
}

FloorSolid floorSolidOf(const content::Lot& lot) {
    const auto found = lot.grids.find(content::kFloorGridInstance);
    if (found == lot.grids.end()) {
        return [](int level, const Key&) { return level == 0; };
    }
    const array3d::Array3D floors = found->second;
    const int ground = content::groundGridLevel(lot);
    return [floors, ground](int level, const Key& key) {
        if (level == 0) {
            return true;
        }
        const int mask = quarters(key);
        if (mask == 0 || !floors.contains(level + ground, key.x, key.y)) {
            return false;
        }
        for (std::size_t q = 0; q < 4; ++q) {
            if ((mask & (1 << q)) && floors.i16(level + ground, key.x, key.y, q) == 0) {
                return false;
            }
        }
        return true;
    };
}

Layout build(const std::vector<Wall>& walls, const RoomAt& roomAt, const FloorSolid& floorSolid) {
    Layout out;
    std::vector<Face> faces;
    out.complete = facesOf(walls, faces);
    for (const auto& loop : loopsOf(faces)) {
        const std::size_t firstRun = out.runs.size();
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const Face& f = faces[loop[i]];
            const bool turns = i == 0 || f.x1 - f.x0 != faces[loop[i - 1]].x1 - faces[loop[i - 1]].x0 ||
                               f.y1 - f.y0 != faces[loop[i - 1]].y1 - faces[loop[i - 1]].y0;
            const Key key = keyOf(f);
            if (turns) {
                walllight::Run run;
                run.level = f.level;
                run.room = roomOf(key, roomAt);
                run.diagonal = f.x1 != f.x0 && f.y1 != f.y0;
                out.runs.push_back(std::move(run));
                out.keys.emplace_back();
            }
            walllight::Segment s;
            s.x0 = static_cast<float>(f.x0);
            s.y0 = static_cast<float>(f.y0);
            s.x1 = static_cast<float>(f.x1);
            s.y1 = static_cast<float>(f.y1);
            out.runs.back().segments.push_back(s);
            out.keys.back().push_back(key);
        }
        // The runs of a loop follow each other round it, the last back to the first.
        const std::size_t count = out.runs.size() - firstRun;
        for (std::size_t i = 0; i < count; ++i) {
            auto& run = out.runs[firstRun + i];
            run.previous = static_cast<int>(firstRun + (i + count - 1) % count);
            run.next = static_cast<int>(firstRun + (i + 1) % count);
            const auto& s = run.segments.front();
            run.angle = static_cast<float>(std::atan2(static_cast<double>(s.y1) - s.y0, static_cast<double>(s.x1) - s.x0));
        }
    }
    // The same wall tile one level up or down, where no solid floor lies between.
    std::map<std::tuple<int, int, int, int, int>, walllight::Link> where;
    for (std::size_t r = 0; r < out.keys.size(); ++r) {
        for (std::size_t s = 0; s < out.keys[r].size(); ++s) {
            const Key& k = out.keys[r][s];
            where[{k.x, k.y, k.level, k.side, k.part}] = {static_cast<int>(r), static_cast<int>(s)};
        }
    }
    auto find = [&](Key k, int level) {
        const auto it = where.find({k.x, k.y, level, k.side, k.part});
        return it == where.end() ? walllight::Link{} : it->second;
    };
    for (std::size_t r = 0; r < out.keys.size(); ++r) {
        for (std::size_t s = 0; s < out.keys[r].size(); ++s) {
            Key k = out.keys[r][s];
            auto& seg = out.runs[r].segments[s];
            Key above = k;
            above.level = k.level + 1;
            if (const auto up = find(k, k.level + 1); up.run >= 0 && !(floorSolid && floorSolid(k.level + 1, above))) {
                seg.up = up;
            }
            if (const auto down = find(k, k.level - 1); down.run >= 0 && !(floorSolid && floorSolid(k.level, k))) {
                seg.down = down;
            }
        }
    }
    return out;
}

std::array<float, 3> portalSample(const std::vector<walllight::Run>& runs, const Layout& layout, const Key& key, bool dimOutside,
                                  const std::array<float, 3>& outsideColor, float outsideStrength) {
    for (std::size_t r = 0; r < layout.keys.size() && r < runs.size(); ++r) {
        for (std::size_t s = 0; s < layout.keys[r].size() && s < runs[r].segments.size(); ++s) {
            if (!(layout.keys[r][s] == key)) {
                continue;
            }
            const auto& texel = runs[r].segments[s].halves[1][3];
            const float scale = std::bit_cast<float>(0x3B808081u);  // 1/255
            std::array<float, 3> out{static_cast<float>(texel.r * static_cast<double>(scale)), static_cast<float>(texel.g * static_cast<double>(scale)),
                                     static_cast<float>(texel.b * static_cast<double>(scale))};
            if (dimOutside && runs[r].room == 0) {
                for (std::size_t c = 0; c < 3; ++c) {
                    out[c] = static_cast<float>(out[c] - static_cast<double>(outsideColor[c]) * outsideStrength);
                }
            }
            return out;
        }
    }
    return {0, 0, 0};
}

std::vector<Wall> wallsOf(const content::Lot& lot, const BlocksLight& blocksLight) {
    std::vector<Wall> out;
    if (!lot.roomGraph) {
        return out;
    }
    std::map<std::int32_t, std::uint32_t> coveringOf;
    for (const auto& c : lot.wallCoverings) {
        coveringOf[static_cast<std::int32_t>(c.wall)] = c.type;
    }
    auto whole = [](float v, int& to) {
        if (!(v >= 0.0f && v <= static_cast<float>(kMaxCoordinate)) || v != std::floor(v)) {
            return false;
        }
        to = static_cast<int>(v);
        return true;
    };
    // Point ids to points (the first of any repeated id, as the graph's own lookup does).
    std::unordered_map<std::int32_t, const wallgraph::Point*> points;
    for (const auto& p : lot.roomGraph->points) {
        points.emplace(p.id, &p);
    }
    auto point = [&](std::int32_t id) -> const wallgraph::Point* {
        const auto it = points.find(id);
        return it == points.end() ? nullptr : it->second;
    };
    for (const auto& w : lot.roomGraph->walls) {
        const auto* a = point(w.from);
        const auto* b = point(w.to);
        if (!a || !b || a->level != b->level) {
            continue;
        }
        const auto covering = coveringOf.find(w.id);
        if (covering == coveringOf.end() || (blocksLight && !blocksLight(covering->second))) {
            continue;
        }
        Wall wall;
        wall.level = a->level;
        wall.covering = covering->second;
        if (whole(a->x, wall.x0) && whole(a->y, wall.y0) && whole(b->x, wall.x1) && whole(b->y, wall.y1)) {
            out.push_back(wall);
        }
    }
    return out;
}

} // namespace ts2::scene::wallruns
