#include "sim/floor_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kRegionLayerGraph = 6;  // the game's second room layer
constexpr std::int32_t kMaxSide = 256;           // lots are at most this many tiles a side
constexpr std::int32_t kMaxLevels = 16;           // so a broken file can't make a huge grid

// Room map quarters (0 top, 1 right, 2 bottom, 3 left) and the floor grid's order of them.
constexpr int kAllQuarters = 0xF;
std::size_t storedQuarter(int quarter) { return static_cast<std::size_t>(3 - quarter); }

struct Step {
    std::int32_t dx, dy;
    int facing;  // the neighbour's quarter that faces back
};
constexpr Step kSteps[4] = {{1, 0, 3}, {-1, 0, 1}, {0, 1, 0}, {0, -1, 2}};

} // namespace

FloorSupport::FloorSupport(const content::Lot& lot, const PartitionLookup& partitions, const ColumnLookup& columns) : lot_(lot) {
    const auto floors = lot.grids.find(content::kFloorGridInstance);
    if (floors == lot.grids.end() || floors->second.elementSize != 8) {
        return;
    }
    const auto& g = floors->second;
    if (g.width == 0 || g.height == 0 || g.width > kMaxSide || g.height > kMaxSide || g.levels > kMaxLevels) {
        return;
    }
    width_ = static_cast<std::int32_t>(g.width);
    height_ = static_cast<std::int32_t>(g.height);
    levels_ = static_cast<std::int32_t>(g.levels);
    ground_ = content::groundGridLevel(lot);
    bounds_ = lot.worldBounds;
    if (const auto graph = lot.layerGraphs.find(kRegionLayerGraph); graph != lot.layerGraphs.end()) {
        regions_.emplace(graph->second);
    }
    const auto tiles = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    walls_.assign(static_cast<std::size_t>(levels_) * tiles, 0);
    const auto corners = static_cast<std::size_t>(width_ + 1) * static_cast<std::size_t>(height_ + 1);
    posts_.assign(static_cast<std::size_t>(levels_) * corners, 0);
    columns_.assign(static_cast<std::size_t>(levels_) * tiles, 0);
    if (columns) {
        for (const auto& o : lot.objects) {
            const auto& h = o.state.header;
            // Off the lot, or not a number in a broken file.
            if (!o.type || !(h.location[0] >= 0 && h.location[0] < kMaxSide && h.location[1] >= 0 && h.location[1] < kMaxSide) ||
                !columns(o.type->guid)) {
                continue;
            }
            // Saved locations run along the lot's y, then x.
            const auto x = static_cast<std::int32_t>(h.location[1]), y = static_cast<std::int32_t>(h.location[0]);
            const auto level = static_cast<std::int32_t>(content::gridLevel(lot, h.value1));
            if (level >= 0 && level < levels_ && x < width_ && y < height_) {
                columns_[(static_cast<std::size_t>(level) * width_ + x) * height_ + y] = 1;
            }
        }
    }
    if (!partitions) {
        return;
    }
    auto bears = [&](std::uint32_t type, bool fence) {
        const auto p = partitions(type);
        return p && p->bearsLoad && p->fence == fence;
    };
    auto markWall = [&](std::int32_t level, std::int32_t x, std::int32_t y, std::uint8_t bit) {
        if (level >= 0 && level < levels_ && x >= 0 && x < width_ && y >= 0 && y < height_) {
            walls_[(static_cast<std::size_t>(level) * width_ + x) * height_ + y] |= bit;
        }
    };
    if (lot.roomGraph) {
        std::map<std::int32_t, std::uint32_t> types;
        for (const auto& c : lot.wallCoverings) {
            types[static_cast<std::int32_t>(c.wall)] = c.type;
        }
        const wallgraph::PointIndex points(*lot.roomGraph);
        for (const auto& w : lot.roomGraph->walls) {
            const auto type = types.find(w.id);
            const auto* a = points(w.from);
            const auto* b = points(w.to);
            // A segment without a covering only divides rooms. Points off the lot (or not numbers, in
            // a broken file) make no wall.
            if (type == types.end() || a == nullptr || b == nullptr || a->level != b->level || !bears(type->second, false) ||
                !(a->x >= 0 && a->x <= kMaxSide && a->y >= 0 && a->y <= kMaxSide && b->x >= 0 && b->x <= kMaxSide &&
                  b->y >= 0 && b->y <= kMaxSide)) {
                continue;
            }
            const auto level = static_cast<std::int32_t>(content::gridIndex(lot, a->level));
            const auto x0 = static_cast<std::int32_t>(std::lround(std::min(a->x, b->x)));
            const auto y0 = static_cast<std::int32_t>(std::lround(std::min(a->y, b->y)));
            const auto dx = static_cast<std::int32_t>(std::lround(std::abs(b->x - a->x)));
            const auto dy = static_cast<std::int32_t>(std::lround(std::abs(b->y - a->y)));
            if (dy == 0) {
                for (std::int32_t i = 0; i < dx; ++i) {
                    markWall(level, x0 + i, y0, 1);
                }
            } else if (dx == 0) {
                for (std::int32_t i = 0; i < dy; ++i) {
                    markWall(level, x0, y0 + i, 2);
                }
            } else if (dx == dy) {
                // Rising when x and y grow together from the lower point.
                const bool rising = (b->x - a->x) * (b->y - a->y) > 0;
                for (std::int32_t i = 0; i < dx; ++i) {
                    markWall(level, x0 + i, rising ? y0 + i : y0 + dy - 1 - i, rising ? 4 : 8);
                }
            }
        }
    }
    for (const auto& p : lot.fencePosts) {
        if (!bears(p.fence, true) || !(p.x >= 0 && p.x <= kMaxSide && p.y >= 0 && p.y <= kMaxSide)) {
            continue;
        }
        const auto level = static_cast<std::int32_t>(content::gridIndex(lot, p.level));
        const auto x = static_cast<std::int32_t>(std::lround(p.x)), y = static_cast<std::int32_t>(std::lround(p.y));
        if (level >= 0 && level < levels_ && x <= width_ && y <= height_) {
            posts_[(static_cast<std::size_t>(level) * (width_ + 1) + x) * (height_ + 1) + y] = 1;
        }
    }
}

bool FloorSupport::inBounds(std::int32_t level, std::int32_t x, std::int32_t y) const {
    if (level < 0 || level >= levels_ || x < 0 || x >= width_ || y < 0 || y >= height_) {
        return false;
    }
    return !bounds_ || (x >= bounds_->minX && x <= bounds_->maxX && y >= bounds_->minY && y <= bounds_->maxY);
}

bool FloorSupport::insideEdge(std::int32_t x, std::int32_t y) const {
    if (x <= 0 || x >= width_ - 1 || y <= 0 || y >= height_ - 1) {
        return false;
    }
    return !bounds_ || (x > bounds_->minX && x < bounds_->maxX && y > bounds_->minY && y < bounds_->maxY);
}

bool FloorSupport::level(std::int32_t gridLevel, std::int32_t x, std::int32_t y) const {
    const auto heights = lot_.grids.find(content::kFloorHeightGridInstance);
    if (heights == lot_.grids.end() || heights->second.elementSize != 4 || !heights->second.contains(gridLevel, x + 1, y + 1)) {
        return false;
    }
    const auto& h = heights->second;
    const float z = h.f32(gridLevel, x, y, 0);
    return h.f32(gridLevel, x + 1, y, 0) == z && h.f32(gridLevel, x, y + 1, 0) == z && h.f32(gridLevel, x + 1, y + 1, 0) == z;
}

bool FloorSupport::regionBelow(std::int32_t gridLevel, std::int32_t x, std::int32_t y, int quarters) const {
    const std::int32_t below = gridLevel - 1;
    if (below >= 0 && below < levels_ && x >= 0 && x < width_ && y >= 0 && y < height_ &&
        columns_[(static_cast<std::size_t>(below) * width_ + x) * height_ + y] != 0) {
        return true;
    }
    if (!regions_) {
        return false;
    }
    const std::int32_t storey = gridLevel - 1 + (lot_.roomGraph ? lot_.roomGraph->firstLevel : 0);
    for (int q = 0; q < 4; ++q) {
        if ((quarters >> q & 1) != 0) {
            const std::int32_t region = regions_->quarterRoom(storey, x, y, q);
            if (region > 0) {
                return true;
            }
        }
    }
    return false;
}

bool FloorSupport::wallsBelow(std::int32_t gridLevel, std::int32_t x, std::int32_t y) const {
    const std::int32_t below = gridLevel - 1;
    if (below < 0 || below >= levels_ || x < 0 || x >= width_ || y < 0 || y >= height_) {
        return false;
    }
    auto wall = [&](std::int32_t tx, std::int32_t ty) -> std::uint8_t {
        if (tx < 0 || tx >= width_ || ty < 0 || ty >= height_) {
            return 0;
        }
        return walls_[(static_cast<std::size_t>(below) * width_ + tx) * height_ + ty];
    };
    if ((wall(x, y) & 0xF) != 0 || (wall(x, y + 1) & 1) != 0 || (wall(x + 1, y) & 2) != 0) {
        return true;
    }
    for (std::int32_t cx = x; cx <= x + 1; ++cx) {
        for (std::int32_t cy = y; cy <= y + 1; ++cy) {
            if (posts_[(static_cast<std::size_t>(below) * (width_ + 1) + cx) * (height_ + 1) + cy] != 0) {
                return true;
            }
        }
    }
    return false;
}

bool FloorSupport::floored(std::int32_t gridLevel, std::int32_t x, std::int32_t y, int quarter) const {
    const auto floors = lot_.grids.find(content::kFloorGridInstance);
    return floors != lot_.grids.end() && floors->second.contains(gridLevel, x, y) &&
           floors->second.i16(gridLevel, x, y, storedQuarter(quarter)) != 0;
}

bool FloorSupport::allowed(std::int32_t gridLevel, std::int32_t x, std::int32_t y) const {
    if (gridLevel <= ground_) {
        return true;
    }
    if (!inBounds(gridLevel, x, y) || !insideEdge(x, y) || !level(gridLevel, x, y)) {
        return false;
    }
    if (regionBelow(gridLevel, x, y, kAllQuarters) || wallsBelow(gridLevel, x, y)) {
        return true;
    }
    bool flooredNeighbour = false;
    for (const auto& s : kSteps) {
        const std::int32_t nx = x + s.dx, ny = y + s.dy;
        if (!inBounds(gridLevel, nx, ny)) {
            continue;
        }
        if (regionBelow(gridLevel, nx, ny, 1 << s.facing)) {
            return true;
        }
        flooredNeighbour = flooredNeighbour || floored(gridLevel, nx, ny, s.facing);
    }
    if (!flooredNeighbour) {
        return false;
    }
    for (std::int32_t dx = -2; dx <= 2; ++dx) {
        for (std::int32_t dy = -2; dy <= 2; ++dy) {
            const std::int32_t d = std::abs(dx) + std::abs(dy);
            if ((d == 2 || d == 3) && inBounds(gridLevel, x + dx, y + dy) && regionBelow(gridLevel, x + dx, y + dy, kAllQuarters)) {
                return true;
            }
        }
    }
    for (std::int32_t dx = -1; dx <= 1; ++dx) {
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            if (inBounds(gridLevel, x + dx, y + dy) && wallsBelow(gridLevel, x + dx, y + dy)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace ts2::sim
