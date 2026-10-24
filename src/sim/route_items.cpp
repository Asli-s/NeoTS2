#include "sim/route_items.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace ts2::sim {

namespace {

// Wall piece sizes, in tiles.
constexpr float kWallHalfWidth = 0.075f;    // the solid core is 0.15 wide
constexpr float kPaddedHalfWidth = 0.2f;    // the padded copy is 0.4 wide...
constexpr float kPaddedOverhang = 0.125f;   // ...and reaches this far past each end
constexpr float kJointHalfSize = 0.3f;      // joints are 0.6 squares
constexpr float kJointCost = 12;

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

} // namespace

std::vector<RouteItem> wallRouteItems(const wallgraph::WallGraph& rooms, const std::vector<walllayer::Entry>& coverings, int level,
                                      const FenceBlocks& fenceBlocks) {
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
        // Only walls along the grid are routed for now. Diagonal walls are not handled yet.
        if (ax != bx && ay != by) {
            continue;
        }
        const bool fence = kind == Barrier::Fence;
        const RouteRect span{std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)};
        const bool alongX = ay == by;
        auto widened = [&](float halfWidth, float overhang) {
            return alongX ? RouteRect{span.x0 - overhang, span.y0 - halfWidth, span.x1 + overhang, span.y1 + halfWidth}
                          : RouteRect{span.x0 - halfWidth, span.y0 - overhang, span.x1 + halfWidth, span.y1 + overhang};
        };
        items.push_back({widened(kWallHalfWidth, 0), kBlockedCost, fence ? kFenceType : kWallType, RouteItemKind::Wall, false,
                         static_cast<std::uint32_t>(w.id)});
        items.push_back({widened(kPaddedHalfWidth, kPaddedOverhang), kBlockedCost, fence ? kFencePaddingType : kWallPaddingType,
                         RouteItemKind::Wall, false, static_cast<std::uint32_t>(w.id)});
    }
    // A joint where a wall ends or turns. Straight runs and T-junctions get none.
    for (const auto& [point, dirs] : directions) {
        const bool end = dirs.size() == 1;
        const bool corner = dirs.size() == 2 && !(dirs[0].first == -dirs[1].first && dirs[0].second == -dirs[1].second);
        if (end || corner) {
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

std::vector<RouteItem> floorRouteItems(const array3d::Array3D& floors, const std::map<std::uint16_t, stringmap::Entry>& patterns,
                                       int level, int lowestLevel, int width, int height) {
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

    // The tiles that have floor: on the ground, every lot tile but those under a raised floor, where the
    // foundation blocks instead.
    std::set<std::pair<int, int>> tiles;
    std::vector<RouteItem> items;
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) {
            const bool hasFloor = floored(level, x, y);
            const bool raised = level == 0 && floored(1, x, y);
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

} // namespace ts2::sim
