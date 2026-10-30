#include "sim/route_items.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>

namespace ts2::sim {

namespace {

// Wall piece sizes, in tiles.
constexpr float kWallHalfWidth = 0.075f;    // the solid core is 0.15 wide
constexpr float kPaddingGrowth = 0.12499f;  // the padded copy is the core grown this much on every side
// A diagonal piece is this many small squares, each 1/16 tile, corner to corner across its tile.
constexpr int kDiagonalSquares = 16;
constexpr float kDiagonalSquare = 1.0f / kDiagonalSquares;
// Longer diagonals than any lot's come only from broken files and are left out.
constexpr float kMaxDiagonalTiles = 256;
constexpr float kJointHalfSize = 0.3f;      // joints are 0.6 squares
constexpr float kJointCost = 12;
// Walls round a point whose largest gap in angle order is wider than this get a joint.
constexpr double kJointGap = 1.05 * 3.14159265358979323846;

// Item types: the core and the padding around it, for walls and for fences.
constexpr int kWallType = 15, kWallPaddingType = 14;
constexpr int kFenceType = 13, kFencePaddingType = 12;

// Wall coverings below this are wall styles. Above, they are a fence's GUID.
constexpr std::uint32_t kFirstGuid = 0x10000;

enum class Barrier { None, Wall, Fence };

// Post types: the post and the padding around it (as for walls).
constexpr int kPostType = 15, kPostPaddingType = 14;
constexpr float kPostPadding = 0.125f;

// Floor costs.
constexpr float kFloorCost = 1;
constexpr float kGroundCost = 1.3f;  // bare ground, a little slower than floor
constexpr float kRoadCost = 4;       // the street, avoided
// The strip along a floor's edge.
constexpr float kEdgeWidth = 0.125f;
constexpr int kEdgeType = 2;
// The foundation under a raised floor: the tile itself and a margin around it.
constexpr int kFoundationType = 7, kFoundationPaddingType = 6;
constexpr float kFoundationMargin = 0.125f;
// A floor above the ground is raised on a foundation where it stands less than a storey above it.
constexpr float kStoreyHeight = 3.0f;
constexpr float kHeightTolerance = 0.001f;

// Tells whether two of the directions, neighbours in angle order (the last and first included), are
// more than kJointGap apart. The order and the gaps are the wall graph's, whose x and y the route
// swaps. A wall of no length sorts as a half turn and is no gap from either neighbour, and walls in
// the same direction keep their file order.
bool hasWideGap(const std::vector<std::pair<float, float>>& dirs) {
    constexpr double kPi = 3.14159265358979323846;
    struct Direction {
        double x, y, angle;
    };
    std::vector<Direction> around;
    around.reserve(dirs.size());
    for (const auto& [routeX, routeY] : dirs) {
        const double x = routeY, y = routeX;
        around.push_back({x, y, y == 0 ? (x > 0 ? 0 : kPi) : std::atan2(y, x)});
    }
    std::ranges::stable_sort(around, {}, &Direction::angle);
    for (std::size_t i = 0; i < around.size(); ++i) {
        const Direction& u = around[i];
        const Direction& v = around[(i + 1) % around.size()];
        double gap = std::atan2(u.x * v.y - v.x * u.y, u.x * v.x + u.y * v.y);
        if (gap < 0) {
            gap += 2 * kPi;
        }
        if (gap > kJointGap) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<RouteItem> wallRouteItems(const wallgraph::WallGraph& rooms, const std::vector<walllayer::Entry>& coverings, int level,
                                      const FenceBlocks& fenceBlocks, int width, int height) {
    std::map<std::int32_t, std::uint32_t> coveringOf;
    for (const auto& c : coverings) {
        coveringOf[static_cast<std::int32_t>(c.wall)] = c.type;
    }
    // Wall styles route as walls, fences that stop Sims as fences.
    auto barrier = [&](const wallgraph::Wall& w) {
        const auto it = coveringOf.find(w.id);
        if (it == coveringOf.end()) {
            return Barrier::None;  // only divides rooms
        }
        const std::uint32_t covering = it->second;
        if (covering < kFirstGuid) {
            return Barrier::Wall;
        }
        return fenceBlocks(covering) ? Barrier::Fence : Barrier::None;
    };

    std::vector<RouteItem> items;
    // The directions walls leave each point in, to find ends and corners. Route coordinates swap the
    // wall graph's x and y.
    std::map<std::pair<float, float>, std::vector<std::pair<float, float>>> directions;
    // The tiles holding a diagonal piece already, by direction.
    std::set<std::tuple<int, int, bool>> diagonalTiles;
    const wallgraph::PointIndex pointById(rooms);
    for (const auto& w : rooms.walls) {
        const auto* a = pointById(w.from);
        const auto* b = pointById(w.to);
        const Barrier kind = barrier(w);
        if (a == nullptr || b == nullptr || a->level != level || kind == Barrier::None) {
            continue;
        }
        const float ax = a->y, ay = a->x, bx = b->y, by = b->x;
        if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by)) {
            continue;
        }
        directions[{ax, ay}].push_back({bx - ax, by - ay});
        directions[{bx, by}].push_back({ax - bx, ay - by});
        const bool fence = kind == Barrier::Fence;
        auto add = [&](const RouteRect& core) {
            const float g = kPaddingGrowth;
            const auto owner = static_cast<std::uint32_t>(w.id);
            items.push_back({core, kBlockedCost, fence ? kFenceType : kWallType, RouteItemKind::Wall, false, owner});
            items.push_back({{core.x0 - g, core.y0 - g, core.x1 + g, core.y1 + g}, kBlockedCost,
                             fence ? kFencePaddingType : kWallPaddingType, RouteItemKind::Wall, false, owner});
        };
        if (ax == bx || ay == by) {
            const RouteRect span{std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)};
            const float h = kWallHalfWidth;
            add(ay == by ? RouteRect{span.x0, span.y0 - h, span.x1, span.y1 + h} : RouteRect{span.x0 - h, span.y0, span.x1 + h, span.y1});
            continue;
        }
        // A diagonal runs corner to corner through whole tiles. Other slants are not walls the game builds.
        const float run = std::abs(bx - ax);
        if (run != std::abs(by - ay) || run > kMaxDiagonalTiles || std::floor(run) != run || std::floor(ax) != ax || std::floor(ay) != ay) {
            continue;
        }
        const float x0 = std::min(ax, bx), yAtX0 = ax < bx ? ay : by;
        const bool rising = (ax < bx) == (ay < by);
        for (int tile = 0; tile < static_cast<int>(run); ++tile) {
            const float tx = x0 + static_cast<float>(tile);
            const float ty = rising ? yAtX0 + static_cast<float>(tile) : yAtX0 - static_cast<float>(tile + 1);
            if (tx < 0 || ty < 0 || tx >= static_cast<float>(width) || ty >= static_cast<float>(height) ||
                !diagonalTiles.insert({static_cast<int>(tx), static_cast<int>(ty), rising}).second) {
                continue;
            }
            for (int i = 0; i < kDiagonalSquares; ++i) {
                const float t = static_cast<float>(i) * kDiagonalSquare;
                const float y = rising ? ty + t : ty + 1 - t - kDiagonalSquare;
                add({tx + t, y, tx + t + kDiagonalSquare, y + kDiagonalSquare});
            }
        }
    }
    // A joint where a wall ends, or where the walls round a point leave a gap wider than 1.05 pi between
    // two neighbours in angle order. Straight runs and T-junctions get none, and walls bunched within
    // less than 0.95 pi of each other get one.
    for (const auto& [point, dirs] : directions) {
        if (dirs.size() == 1 || hasWideGap(dirs)) {
            const auto [x, y] = point;
            items.push_back({{x - kJointHalfSize, y - kJointHalfSize, x + kJointHalfSize, y + kJointHalfSize}, kJointCost, 0,
                             RouteItemKind::WallJoint, false, 0});
        }
    }
    return items;
}

std::vector<RouteItem> postRouteItems(const std::vector<fencepostlayer::Post>& posts, int level, const PostFootprint& footprint) {
    std::vector<RouteItem> items;
    for (const auto& post : posts) {
        const auto shape = footprint(post.fence);
        if (post.level != level || !shape) {
            continue;
        }
        // Route coordinates swap the wall graph's x and y.
        const float x = post.y, y = post.x, p = kPostPadding;
        const RouteRect core{x + shape->x0, y + shape->y0, x + shape->x1, y + shape->y1};
        items.push_back({core, kBlockedCost, kPostType, RouteItemKind::Post, false, 0});
        items.push_back({{core.x0 - p, core.y0 - p, core.x1 + p, core.y1 + p}, kBlockedCost, kPostPaddingType, RouteItemKind::Post, false, 0});
    }
    return items;
}

std::vector<RouteItem> floorRouteItems(const array3d::Array3D& floors, const array3d::Array3D* heights, const array3d::Array3D* surface,
                                       const std::map<std::uint16_t, stringmap::Entry>& patterns, int level, int lowestLevel, int width,
                                       int height) {
    // Floor grid cells are indexed by the wall graph's x and y. Route tile (x, y) is grid (y, x).
    auto floored = [&](int floorLevel, int x, int y) {
        const int index = floorLevel - lowestLevel;
        if (!floors.contains(index, y, x)) {
            return false;
        }
        for (std::size_t q = 0; q < 4; ++q) {
            if (floors.i16(index, y, x, q) != 0) {
                return true;
            }
        }
        return false;
    };
    auto costOf = [&](int x, int y) {
        // A tile's cost follows its floor pattern: lot roads cost more, everything else the same.
        for (std::size_t q = 0; q < 4; ++q) {
            const auto pattern = patterns.find(static_cast<std::uint16_t>(floors.i16(level - lowestLevel, y, x, q)));
            if (pattern != patterns.end() && pattern->second.name.starts_with("road_")) {
                return kRoadCost;
            }
        }
        return kFloorCost;
    };

    // A ground tile is the foundation of the storey above when that storey has floor in all four
    // quarters there, its surface flag set in each, and stands less than a storey above the ground at
    // one of the tile's corners at least. A second storey over a ground floor room is a storey up.
    auto raisedAbove = [&](int x, int y) {
        const int ground = -lowestLevel, above = 1 - lowestLevel;
        const auto open = surface != nullptr ? surface->at(above, y, x) : std::span<const std::uint8_t>{};
        if (heights == nullptr || open.size() != 4 || !floors.contains(above, y, x)) {
            return false;
        }
        for (std::size_t q = 0; q < 4; ++q) {
            if (floors.i16(above, y, x, q) == 0 || open[q] == 0) {
                return false;
            }
        }
        for (const auto [dx, dy] : {std::pair{0, 0}, {1, 0}, {0, 1}, {1, 1}}) {
            const int cx = y + dy, cy = x + dx;
            if (heights->contains(ground, cx, cy) && heights->contains(above, cx, cy) &&
                heights->f32(above, cx, cy, 0) - heights->f32(ground, cx, cy, 0) + kHeightTolerance < kStoreyHeight) {
                return true;
            }
        }
        return false;
    };

    // The tiles that have floor: on the ground, every lot tile but those under a raised floor, where the
    // foundation blocks instead.
    std::set<std::pair<int, int>> tiles;
    std::vector<RouteItem> items;
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) {
            const bool hasFloor = floored(level, x, y);
            const bool raised = level == 0 && raisedAbove(x, y);
            const bool ground = level == 0 && !raised;
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            if (raised) {
                const float m = kFoundationMargin;
                items.push_back({{fx, fy, fx + 1, fy + 1}, kBlockedCost, kFoundationType, RouteItemKind::Floor, true, 0});
                items.push_back({{fx - m, fy - m, fx + 1 + m, fy + 1 + m}, kFloorCost, kFoundationPaddingType, RouteItemKind::Floor, true, 0});
            }
            if (!hasFloor && !ground) {
                continue;
            }
            tiles.insert({x, y});
            const float cost = hasFloor ? costOf(x, y) : kGroundCost;
            items.push_back({{fx, fy, fx + 1, fy + 1}, cost, 0, RouteItemKind::Floor, true, 0});
        }
    }
    // A strip inside each tile along every edge without floor beyond it, and a small square in each
    // inside corner. On the ground only the lot's border counts as an edge.
    const float s = kEdgeWidth;
    auto strip = [&](RouteRect r) { items.push_back({r, kFloorCost, kEdgeType, RouteItemKind::Floor, true, 0}); };
    for (const auto& [x, y] : tiles) {
        const float fx = static_cast<float>(x), fy = static_cast<float>(y);
        auto has = [&](int dx, int dy) {
            const int nx = x + dx, ny = y + dy;
            return level == 0 ? nx >= 0 && ny >= 0 && nx < width && ny < height : tiles.contains({nx, ny});
        };
        if (!has(-1, 0)) strip({fx, fy, fx + s, fy + 1});
        if (!has(1, 0)) strip({fx + 1 - s, fy, fx + 1, fy + 1});
        if (!has(0, -1)) strip({fx, fy, fx + 1, fy + s});
        if (!has(0, 1)) strip({fx, fy + 1 - s, fx + 1, fy + 1});
        for (const auto [dx, dy] : {std::pair{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}) {
            if (has(dx, 0) && has(0, dy) && !has(dx, dy)) {
                const float cx = dx < 0 ? fx : fx + 1 - s, cy = dy < 0 ? fy : fy + 1 - s;
                strip({cx, cy, cx + s, cy + s});
            }
        }
    }
    return items;
}

std::vector<RouteItem> simRingItems(const RouteRect& square, bool target, std::uint32_t owner) {
    struct Ring {
        float growth;
        float cost;
    };
    constexpr Ring kOuter{0.375f, 5.0f}, kMiddle{0.1875f, 7.5f}, kInner{0.0625f, 10.0f};
    std::vector<RouteItem> items;
    for (const Ring& ring : {kOuter, kMiddle, kInner}) {
        if (target && ring.growth == kOuter.growth) {
            continue;
        }
        RouteItem item;
        item.rect = {square.x0 - ring.growth, square.y0 - ring.growth, square.x1 + ring.growth, square.y1 + ring.growth};
        item.cost = ring.cost;
        item.type = 1;
        item.kind = RouteItemKind::Soft;
        item.owner = owner;
        items.push_back(item);
    }
    return items;
}

} // namespace ts2::sim
