#include "sim/placement_walls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ts2::sim {

namespace {

constexpr std::int32_t kMaxSide = 256;  // lots are at most this many tiles a side
constexpr std::int32_t kMaxLevels = 16;  // so a broken file can't make a huge grid
constexpr std::uint8_t kLowX = 1, kLowY = 2, kRising = 4, kFalling = 8;

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
    const auto mark = [&](std::int32_t level, std::int32_t x, std::int32_t y, std::uint8_t bit) {
        const std::int32_t l = level - firstLevel_, tx = x - minX_, ty = y - minY_;
        if (l >= 0 && l < levels_ && tx >= 0 && tx < width_ && ty >= 0 && ty < height_) {
            bits_[(static_cast<std::size_t>(l) * width_ + tx) * height_ + ty] |= bit;
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

std::uint8_t PlacementWalls::bits(std::int32_t level, std::int32_t x, std::int32_t y) const {
    const std::int32_t l = level - firstLevel_, tx = x - minX_, ty = y - minY_;
    if (l < 0 || l >= levels_ || tx < 0 || tx >= width_ || ty < 0 || ty >= height_) {
        return 0;
    }
    return bits_[(static_cast<std::size_t>(l) * width_ + tx) * height_ + ty];
}

bool PlacementWalls::crossed(const GameRect& r, std::int32_t level) const {
    constexpr float kShrink = 0.001f, kHalf = 0.5f;
    if (bits_.empty() || !(r.x0 < r.x1) || !(r.y0 < r.y1) || level < firstLevel_ || level > firstLevel_ + levels_ - 1) {
        return false;
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
            if ((bits(level, x, y) & mask) != 0) {
                return true;
            }
        }
    }
    return false;
}

} // namespace ts2::sim
