#include "sim/idle_spot.hpp"

#include "diag/log.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>

namespace ts2::sim {

namespace {

constexpr float kFixedPoint = 65536.0f;
constexpr float kOtherSimDistance = 0.6f;
constexpr float kWallDistance = 0.4f;
// A wall longer than this many tiles is not a lot's wall (a broken file). It is left out.
constexpr int kMaxWallTiles = 256;

float tileUnits(std::int32_t fixed) { return static_cast<float>(fixed) / kFixedPoint; }

} // namespace

WallTiles WallTiles::fromLot(const content::Lot& lot) {
    WallTiles walls;
    if (!lot.roomGraph) {
        return walls;
    }
    const auto& graph = *lot.roomGraph;
    const wallgraph::PointIndex pointById(graph);
    for (const auto& w : graph.walls) {
        const auto* a = pointById(w.from);
        const auto* b = pointById(w.to);
        if (a != nullptr && b != nullptr && a->level == b->level) {
            walls.add(a->level, a->y, a->x, b->y, b->x);
        }
    }
    return walls;
}

WallTiles WallTiles::builtWalls(const content::Lot& lot) {
    // Coverings below this are wall styles. Above, it is a fence's GUID.
    constexpr std::uint32_t kFirstFenceGuid = 0x10000;
    WallTiles walls;
    if (!lot.roomGraph) {
        return walls;
    }
    std::set<std::int32_t> built;
    for (const auto& c : lot.wallCoverings) {
        if (c.type < kFirstFenceGuid) {
            built.insert(static_cast<std::int32_t>(c.wall));
        }
    }
    const auto& graph = *lot.roomGraph;
    const wallgraph::PointIndex pointById(graph);
    for (const auto& w : graph.walls) {
        const auto* a = pointById(w.from);
        const auto* b = pointById(w.to);
        if (built.contains(w.id) && a != nullptr && b != nullptr && a->level == b->level) {
            walls.add(a->level, a->y, a->x, b->y, b->x);
        }
    }
    return walls;
}

void WallTiles::mark(int level, int x, int y, std::uint8_t side) { sides_[{level, x, y}] |= side; }

void WallTiles::add(int level, float x0, float y0, float x1, float y1) {
    for (const float v : {x0, y0, x1, y1}) {
        if (!(std::fabs(v) <= static_cast<float>(kMaxWallTiles))) {
            diag::warnOnce("idle spots: a wall past 256 tiles is left out");
            return;
        }
    }
    const float dx = x1 - x0, dy = y1 - y0;
    const int steps = static_cast<int>(std::lround(std::max(std::fabs(dx), std::fabs(dy))));
    if (steps <= 0 || steps > kMaxWallTiles) {
        if (steps > kMaxWallTiles) {
            diag::warnOnce("idle spots: a wall longer than 256 tiles is left out");
        }
        return;
    }
    const int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0, sy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
    const int ax = static_cast<int>(std::lround(x0)), ay = static_cast<int>(std::lround(y0));
    for (int i = 0; i < steps; ++i) {
        const int px = ax + sx * i, py = ay + sy * i;
        if (sy == 0) {
            const int tx = sx > 0 ? px : px - 1;
            mark(level, tx, py, kLowY);
            mark(level, tx, py - 1, kHighY);
        } else if (sx == 0) {
            const int ty = sy > 0 ? py : py - 1;
            mark(level, px, ty, kLowX);
            mark(level, px - 1, ty, kHighX);
        } else {
            mark(level, sx > 0 ? px : px - 1, sy > 0 ? py : py - 1, sx == sy ? kDiagonalRising : kDiagonalFalling);
        }
    }
}

std::uint8_t WallTiles::at(int level, int x, int y) const {
    const auto it = sides_.find({level, x, y});
    return it != sides_.end() ? it->second : 0;
}

bool routing(const SimObject& sim) {
    if (!sim.walking) {
        return false;
    }
    const auto& frames = sim.thread.frames;
    const Walking& walk = *sim.walking;
    return walk.frame < frames.size() && frames[walk.frame].group == walk.group && frames[walk.frame].instance == walk.tree &&
           frames[walk.frame].pc == walk.pc;
}

bool okToIdleHere(World& world, const SimObject& sim) {
    if (sim.x == kOffLot || sim.y == kOffLot) {
        return true;
    }
    const float x = tileUnits(sim.x), y = tileUnits(sim.y);
    bool crowded = false;
    world.nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& other) {
        if (&other != &sim && other.isPerson() && !routing(other) && other.x != kOffLot && other.y != kOffLot &&
            std::hypot(tileUnits(other.x) - x, tileUnits(other.y) - y) < kOtherSimDistance) {
            crowded = true;
        }
        return crowded;
    });
    if (crowded) {
        return false;
    }
    return !(wallDistanceOnTile(world.walls(), sim.level - 1, x, y) < kWallDistance);
}

float wallDistanceOnTile(const WallTiles& walls, int level, float x, float y) {
    const float tx = std::floor(x), ty = std::floor(y);
    if (!(std::fabs(tx) < 1e6f && std::fabs(ty) < 1e6f)) {
        return std::numeric_limits<float>::infinity();
    }
    const std::uint8_t sides = walls.at(level, static_cast<int>(tx), static_cast<int>(ty));
    const float fx = x - tx, fy = y - ty;
    const float distances[6] = {fy, fx, 1 - fy, 1 - fx, std::fabs(fy - (1 - fx)) / std::sqrt(2.0f), std::fabs(fy - fx) / std::sqrt(2.0f)};
    float nearest = std::numeric_limits<float>::infinity();
    for (int i = 0; i < 6; ++i) {
        if ((sides & (1u << i)) != 0) {
            nearest = std::min(nearest, distances[i]);
        }
    }
    return nearest;
}

namespace {

struct Point {
    float x, y;
};

float cross(Point o, Point a, Point b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

bool crosses(Point a, Point b, Point c, Point d) {
    const float d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}

// Like crosses, but touching counts: an end of one on the other, or the two overlapping on one line.
bool touches(Point a, Point b, Point c, Point d) {
    const float d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
    if (d1 == 0 && d2 == 0) {
        return std::max(std::min(a.x, b.x), std::min(c.x, d.x)) <= std::min(std::max(a.x, b.x), std::max(c.x, d.x)) &&
               std::max(std::min(a.y, b.y), std::min(c.y, d.y)) <= std::min(std::max(a.y, b.y), std::max(c.y, d.y));
    }
    return ((d1 >= 0 && d2 <= 0) || (d1 <= 0 && d2 >= 0)) && ((d3 >= 0 && d4 <= 0) || (d3 <= 0 && d4 >= 0));
}

// The wall pieces of a tile, as line segments.
template <typename F>
void tileWalls(std::uint8_t sides, int x, int y, F&& each) {
    const float x0 = static_cast<float>(x), y0 = static_cast<float>(y), x1 = x0 + 1, y1 = y0 + 1;
    if (sides & WallTiles::kLowY) each(Point{x0, y0}, Point{x1, y0});
    if (sides & WallTiles::kLowX) each(Point{x0, y0}, Point{x0, y1});
    if (sides & WallTiles::kHighY) each(Point{x0, y1}, Point{x1, y1});
    if (sides & WallTiles::kHighX) each(Point{x1, y0}, Point{x1, y1});
    if (sides & WallTiles::kDiagonalFalling) each(Point{x0, y1}, Point{x1, y0});
    if (sides & WallTiles::kDiagonalRising) each(Point{x0, y0}, Point{x1, y1});
}

} // namespace

std::optional<std::int32_t> wallInFront(const World& world, const SimObject& sim) {
    if (sim.x == kOffLot || sim.y == kOffLot) {
        return std::nullopt;
    }
    constexpr float kReach = 1.5f, kPi = 3.14159265358979f;
    // The facing turns the world's +y. The world's x and y are the route's y and x.
    const float angle = static_cast<float>(2 - sim.data[1]) * kPi / 4;
    const float worldX = -kReach * std::sin(angle), worldY = kReach * std::cos(angle);
    const Point from{tileUnits(sim.x), tileUnits(sim.y)};
    const Point to{from.x + worldY, from.y + worldX};
    std::optional<Point> nearTile;
    float nearest = 0;
    const int level = sim.level - 1;
    for (int x = static_cast<int>(std::floor(std::min(from.x, to.x))); x <= static_cast<int>(std::floor(std::max(from.x, to.x))); ++x) {
        for (int y = static_cast<int>(std::floor(std::min(from.y, to.y))); y <= static_cast<int>(std::floor(std::max(from.y, to.y))); ++y) {
            tileWalls(world.walls().at(level, x, y), x, y, [&](Point a, Point b) {
                if (!crosses(from, to, a, b)) {
                    return;
                }
                // The tiles on the wall's two sides (a diagonal's: its own), by their centres.
                const float cx = static_cast<float>(x) + 0.5f, cy = static_cast<float>(y) + 0.5f;
                std::array<Point, 2> sides{Point{cx, cy}, Point{cx, cy}};
                if (a.y == b.y) {
                    sides[1].y = a.y == static_cast<float>(y) ? cy - 1 : cy + 1;
                } else if (a.x == b.x) {
                    sides[1].x = a.x == static_cast<float>(x) ? cx - 1 : cx + 1;
                }
                for (const Point& c : sides) {
                    const float d = (c.x - from.x) * (c.x - from.x) + (c.y - from.y) * (c.y - from.y);
                    if (!nearTile || d < nearest) {
                        nearTile = c;
                        nearest = d;
                    }
                }
            });
        }
    }
    if (!nearTile) {
        return std::nullopt;
    }
    return world.rooms().roomAtPosition(sim.level - 1, nearTile->x, nearTile->y);
}

bool wallTouches(const WallTiles& walls, int level, float x0, float y0, float x1, float y1) {
    constexpr float kLotLimit = 256;
    const Point from{x0, y0}, to{x1, y1};
    const auto first = [&](float a, float b) { return static_cast<int>(std::floor(std::clamp(std::min(a, b), 0.0f, kLotLimit))); };
    const auto last = [&](float a, float b) { return static_cast<int>(std::floor(std::clamp(std::max(a, b), 0.0f, kLotLimit))); };
    bool blocked = false;
    for (int x = first(x0, x1); x <= last(x0, x1) && !blocked; ++x) {
        for (int y = first(y0, y1); y <= last(y0, y1) && !blocked; ++y) {
            tileWalls(walls.at(level, x, y), x, y, [&](Point a, Point b) { blocked |= touches(from, to, a, b); });
        }
    }
    return blocked;
}

bool wallBetween(const World& world, int level, float x0, float y0, float x1, float y1) {
    return wallCrosses(world.walls(), level, x0, y0, x1, y1);
}

bool wallCrosses(const WallTiles& walls, int level, float x0, float y0, float x1, float y1) {
    // Only tiles on the lot can hold walls. This also keeps far-off points from making a long loop.
    constexpr float kLotLimit = 256;
    const Point from{x0, y0}, to{x1, y1};
    const auto first = [&](float a, float b) { return static_cast<int>(std::floor(std::clamp(std::min(a, b), 0.0f, kLotLimit))); };
    const auto last = [&](float a, float b) { return static_cast<int>(std::floor(std::clamp(std::max(a, b), 0.0f, kLotLimit))); };
    bool blocked = false;
    for (int x = first(x0, x1); x <= last(x0, x1) && !blocked; ++x) {
        for (int y = first(y0, y1); y <= last(y0, y1) && !blocked; ++y) {
            tileWalls(walls.at(level, x, y), x, y, [&](Point a, Point b) { blocked |= crosses(from, to, a, b); });
        }
    }
    return blocked;
}

} // namespace ts2::sim
