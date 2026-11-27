#include "sim/placement_walls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace ts2::sim {

namespace {

constexpr std::int32_t kMaxSide = 256;  // lots are at most this many tiles a side
constexpr std::int32_t kMaxLevels = 16;  // so a broken file can't make a huge grid
constexpr std::uint8_t kLowX = 1, kLowY = 2, kRising = 4, kFalling = 8;

// Each side's wall-needed bit, wall-forbidden bit (wall-allowed for a diagonal) and corners. Corners
// are 1 (x, y + 1), 3 (x, y), 5 (x + 1, y) and 7 (x + 1, y + 1), and a side's are looked up six
// steps on from its own, plus two per quarter turn.
struct Side {
    std::uint16_t needs, forbids;
    int from, to;
};
constexpr Side kSides[] = {{0x01, 0x100, 3, 1}, {0x02, 0x200, 1, 7}, {0x04, 0x400, 7, 5},
                           {0x08, 0x800, 5, 3}, {0x10, 0x020, 7, 3}, {0x40, 0x080, 1, 5}};
constexpr int kCornerX[8] = {0, 0, 0, 0, 0, 1, 0, 1}, kCornerY[8] = {0, 1, 0, 0, 0, 0, 0, 1};

} // namespace

PlacementWalls::PlacementWalls(const content::Lot& lot) {
    if (!lot.roomGraph) {
        return;
    }
    const auto& g = *lot.roomGraph;
    if (g.width <= 0 || g.height <= 0 || g.levels <= 0 || g.width > kMaxSide || g.height > kMaxSide || g.levels > kMaxLevels) {
        return;
    }
    minX_ = lot.worldBounds ? lot.worldBounds->minX : 0;
    minY_ = lot.worldBounds ? lot.worldBounds->minY : 0;
    width_ = g.width;
    height_ = g.height;
    levels_ = g.levels;
    firstLevel_ = g.firstLevel;
    bits_.assign(static_cast<std::size_t>(levels_) * width_ * height_, 0);
    built_ = solid_ = bits_;
    // A covering names a wall style, or a fence's or half wall's catalog GUID.
    constexpr std::uint32_t kLastWallStyle = 0xFFFF;
    std::map<std::uint32_t, std::uint32_t> styles;
    for (const auto& c : lot.wallCoverings) {
        styles[c.wall] = c.type;
    }
    std::uint8_t built = 0, solid = 0;
    const auto mark = [&](std::int32_t level, std::int32_t x, std::int32_t y, std::uint8_t bit) {
        const std::int32_t l = level - firstLevel_, tx = x - minX_, ty = y - minY_;
        if (l >= 0 && l < levels_ && tx >= 0 && tx < width_ && ty >= 0 && ty < height_) {
            const std::size_t i = (static_cast<std::size_t>(l) * width_ + tx) * height_ + ty;
            bits_[i] |= bit;
            built_[i] |= bit & built;
            solid_[i] |= bit & solid;
        }
    };
    const wallgraph::PointIndex points(g);
    for (const auto& w : g.walls) {
        const auto* a = points(w.from);
        const auto* b = points(w.to);
        // Points off the lot (or not numbers, in a broken file) make no wall.
        if (a == nullptr || b == nullptr || a->level != b->level ||
            !(a->x >= -kMaxSide && a->x <= 2 * kMaxSide && a->y >= -kMaxSide && a->y <= 2 * kMaxSide && b->x >= -kMaxSide &&
              b->x <= 2 * kMaxSide && b->y >= -kMaxSide && b->y <= 2 * kMaxSide)) {
            continue;
        }
        const auto style = styles.find(w.id);
        built = style != styles.end() ? 0xFF : 0;
        solid = style != styles.end() && style->second <= kLastWallStyle ? 0xFF : 0;
        const auto x0 = static_cast<std::int32_t>(std::lround(std::min(a->x, b->x)));
        const auto y0 = static_cast<std::int32_t>(std::lround(std::min(a->y, b->y)));
        const auto dx = static_cast<std::int32_t>(std::lround(std::abs(b->x - a->x)));
        const auto dy = static_cast<std::int32_t>(std::lround(std::abs(b->y - a->y)));
        if (dy == 0) {
            for (std::int32_t i = 0; i < dx; ++i) {
                mark(a->level, x0 + i, y0, kLowY);
            }
        } else if (dx == 0) {
            for (std::int32_t i = 0; i < dy; ++i) {
                mark(a->level, x0, y0 + i, kLowX);
            }
        } else if (dx == dy) {
            // Rising when x and y grow together.
            const bool rising = (b->x - a->x) * (b->y - a->y) > 0;
            for (std::int32_t i = 0; i < dx; ++i) {
                mark(a->level, x0 + i, rising ? y0 + i : y0 + dy - 1 - i, rising ? kRising : kFalling);
            }
        }
    }
}

std::uint8_t PlacementWalls::at(const std::vector<std::uint8_t>& grid, std::int32_t level, std::int32_t x, std::int32_t y) const {
    const std::int32_t l = level - firstLevel_, tx = x - minX_, ty = y - minY_;
    if (grid.empty() || l < 0 || l >= levels_ || tx < 0 || tx >= width_ || ty < 0 || ty >= height_) {
        return 0;
    }
    return grid[(static_cast<std::size_t>(l) * width_ + tx) * height_ + ty];
}

std::optional<PlacementWalls::SideRefusal> PlacementWalls::sides(std::uint16_t sides, std::int32_t x, std::int32_t y, std::int32_t level,
                                                                 std::int16_t direction, bool standsInFor) const {
    constexpr std::uint16_t kOnFences = 0x1000;
    if (sides == 0 || (sides & kOnFences) != 0) {
        return std::nullopt;
    }
    const int turn = (direction / 2) * 2;
    const std::uint8_t diagonals = static_cast<std::uint8_t>(at(built_, level, x, y) & (kRising | kFalling));
    for (std::size_t i = 0; i < std::size(kSides); ++i) {
        const Side& s = kSides[i];
        // Two corner steps per quarter turn.
        const int c = (turn + s.from + 6) & 7, d = (turn + s.to + 6) & 7;
        const int cx = kCornerX[c], cy = kCornerY[c], dx = kCornerX[d], dy = kCornerY[d];
        std::int32_t tx = x, ty = y;
        std::uint8_t bit = 0;
        if (cx == dx) {
            tx += cx;
            ty += std::min(cy, dy);
            bit = kLowX;
        } else if (cy == dy) {
            tx += std::min(cx, dx);
            ty += cy;
            bit = kLowY;
        } else {
            bit = cx == cy ? kRising : kFalling;  // (x, y) to (x + 1, y + 1), or the other diagonal
        }
        const bool built = (at(built_, level, tx, ty) & bit) != 0;
        const bool wall = (at(solid_, level, tx, ty) & bit) != 0;
        const std::array<float, 2> middle{static_cast<float>(x) + 0.5f * static_cast<float>(cx + dx),
                                          static_cast<float>(y) + 0.5f * static_cast<float>(cy + dy)};
        const bool forbidden = i < 4 ? (sides & s.forbids) != 0 : (sides & s.forbids) == 0;
        if (forbidden && built) {
            return SideRefusal{8, middle};
        }
        if ((sides & s.needs) != 0 && !wall) {
            constexpr std::int16_t kAgainstWall = 7, kDiagonalWall = 0x3B, kStandsIn = 1;
            return SideRefusal{standsInFor ? kStandsIn : diagonals != 0 ? kDiagonalWall : kAgainstWall, middle};
        }
    }
    return std::nullopt;
}

std::optional<std::array<float, 2>> PlacementWalls::crossing(const GameRect& r, std::int32_t level) const {
    constexpr float kShrink = 0.001f, kHalf = 0.5f;
    if (bits_.empty() || !(r.x0 < r.x1) || !(r.y0 < r.y1) || level < firstLevel_ || level > firstLevel_ + levels_ - 1) {
        return std::nullopt;
    }
    const float minX = static_cast<float>(minX_), maxX = static_cast<float>(minX_ + width_ - 1);
    const float minY = static_cast<float>(minY_), maxY = static_cast<float>(minY_ + height_ - 1);
    const float loX = std::clamp(r.x0 + kShrink, minX, maxX), hiX = std::clamp(r.x1 - kShrink, minX, maxX);
    const float loY = std::clamp(r.y0 + kShrink, minY, maxY), hiY = std::clamp(r.y1 - kShrink, minY, maxY);
    const auto tx0 = static_cast<std::int32_t>(std::floor(loX)), ty0 = static_cast<std::int32_t>(std::floor(loY));
    const auto tx1 = static_cast<std::int32_t>(std::floor(hiX)), ty1 = static_cast<std::int32_t>(std::floor(hiY));
    // Which quarter of the corner tiles the footprint stays in.
    const bool loHighX = loX - static_cast<float>(tx0) > kHalf, loHighY = loY - static_cast<float>(ty0) > kHalf;
    const bool hiLowX = hiX - static_cast<float>(tx1) < kHalf, hiLowY = hiY - static_cast<float>(ty1) < kHalf;
    for (std::int32_t x = tx0; x <= tx1; ++x) {
        for (std::int32_t y = ty0; y <= ty1; ++y) {
            std::uint8_t mask = kLowX | kLowY | kRising | kFalling;
            if (x == tx0) {
                mask ^= kLowX;
            }
            if (y == ty0) {
                mask ^= kLowY;
            }
            if (x == tx0 && y == ty0 && loHighX && loHighY) {
                mask ^= kFalling;
            }
            if (x == tx1 && y == ty1 && hiLowX && hiLowY) {
                mask ^= kFalling;
            }
            if (x == tx0 && y == ty1 && loHighX && hiLowY) {
                mask ^= kRising;
            }
            if (x == tx1 && y == ty0 && hiLowX && loHighY) {
                mask ^= kRising;
            }
            if (const std::uint8_t hit = bits(level, x, y) & mask; hit != 0) {
                const auto fx = static_cast<float>(x), fy = static_cast<float>(y);
                return (hit & kLowX) != 0   ? std::array<float, 2>{fx, fy + kHalf}
                       : (hit & kLowY) != 0 ? std::array<float, 2>{fx + kHalf, fy}
                                            : std::array<float, 2>{fx + kHalf, fy + kHalf};
            }
        }
    }
    return std::nullopt;
}

std::vector<std::array<std::int32_t, 4>> PlacementWalls::neededEdges(std::uint16_t sides, std::int32_t x, std::int32_t y, std::int16_t direction) {
    std::vector<std::array<std::int32_t, 4>> edges;
    const int turn = (direction / 2) * 2;
    for (const Side& s : kSides) {
        if ((sides & s.needs) == 0) {
            continue;
        }
        const int c = (turn + s.from + 6) & 7, d = (turn + s.to + 6) & 7;
        edges.push_back({x + kCornerX[c], y + kCornerY[c], x + kCornerX[d], y + kCornerY[d]});
    }
    return edges;
}

} // namespace ts2::sim
