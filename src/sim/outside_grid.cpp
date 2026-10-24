#include "sim/outside_grid.hpp"

#include "diag/log.hpp"
#include "scene/lot_parts.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace ts2::sim {

namespace {

constexpr float kStoreyHeight = 3.0f;
constexpr float kAbove = 0.001f;      // a top this little above a floor still counts as level with it
constexpr float kPoolDrop = 0.25f;    // the ground over a pool lies this far below its floor height
constexpr std::int32_t kSameHeightUlps = 32;
constexpr std::uint32_t kPoolGridInstance = 0x14, kBlockedGridInstance = 0x1B;
constexpr std::int32_t kPoolKinds[] = {3, 5};
constexpr std::int32_t kMaxSide = 256, kMaxLevels = 16;

// Each quarter's bit and where the game samples it, from the tile's centre.
struct Quarter {
    std::uint8_t bit;
    float dx, dy;
};
constexpr std::array<Quarter, 4> kQuarters{{{1, -1.0f / 3, 0}, {2, 0, 1.0f / 3}, {4, 1.0f / 3, 0}, {8, 0, -1.0f / 3}}};

// The quarter of a tile holding a point, by its place in the tile: the four triangles between its diagonals.
std::size_t quarterOf(float fx, float fy) {
    if (fx <= fy) {
        return fx + fy < 1 ? 0 : 1;
    }
    return fx + fy < 1 ? 3 : 2;
}

bool sameHeight(float a, float b) {
    const auto order = [](float f) {
        const auto i = std::bit_cast<std::int32_t>(f);
        return i < 0 ? std::numeric_limits<std::int32_t>::min() - i : i;
    };
    return std::abs(static_cast<std::int64_t>(order(a)) - order(b)) <= kSameHeightUlps;
}

const array3d::Array3D* gridOf(const content::Lot& lot, std::uint32_t instance, std::size_t elementSize) {
    const auto it = lot.grids.find(instance);
    return it != lot.grids.end() && it->second.elementSize == elementSize ? &it->second : nullptr;
}

std::int32_t i32(const array3d::Array3D& g, std::int32_t level, std::int32_t x, std::int32_t y, std::size_t index) {
    const auto bytes = g.at(level, x, y);
    std::int32_t v = 0;
    if (bytes.size() >= (index + 1) * 4) {
        std::memcpy(&v, bytes.data() + index * 4, 4);
    }
    return v;
}

class Builder {
public:
    explicit Builder(const content::Lot& lot)
        : ground_(content::gridIndex(lot, 0)), heights_(gridOf(lot, content::kFloorHeightGridInstance, 4)),
          floors_(gridOf(lot, content::kFloorGridInstance, 8)), surface_(gridOf(lot, content::kSurfaceGridInstance, 4)),
          pools_(gridOf(lot, kPoolGridInstance, 16)), blocked_(gridOf(lot, kBlockedGridInstance, 8)) {
        if (heights_ != nullptr) {
            top_ = static_cast<std::int32_t>(heights_->levels) - 1 - ground_;
        }
        for (const auto& roof : lot.roofs) {
            if (!scene::sensibleRoof(roof)) {
                continue;
            }
            const float eave = floorHeight(content::gridIndex(lot, roof.level0) - ground_, roof.x0, roof.y0);
            // A roof covers its rectangle, not its overhang. Those not shaped yet cover it at any height.
            const std::array<float, 4> area{std::min(roof.x0, roof.x1), std::min(roof.y0, roof.y1), std::max(roof.x0, roof.x1),
                                            std::max(roof.y0, roof.y1)};
            roofs_.push_back({area, scene::RoofGrid::of(roof, eave)});
        }
    }

    std::int32_t levels() const { return top_ + 1; }

    // The floor's height on a storey at a point, between the heights at the tile's corners.
    float floorHeight(std::int32_t level, float x, float y) const {
        if (heights_ == nullptr || heights_->width < 2 || heights_->height < 2 || top_ < -ground_) {
            return static_cast<float>(level) * kStoreyHeight;
        }
        if (level > top_) {
            return floorHeight(top_, x, y) + static_cast<float>(level - top_) * kStoreyHeight;
        }
        if (level < -ground_) {
            return floorHeight(-ground_, x, y) - static_cast<float>(-ground_ - level) * kStoreyHeight;
        }
        const float maxX = static_cast<float>(heights_->width - 2), maxY = static_cast<float>(heights_->height - 2);
        const float cx = std::clamp(x, 0.0f, maxX + 1), cy = std::clamp(y, 0.0f, maxY + 1);
        const auto ix = static_cast<std::int32_t>(std::min(std::floor(cx), maxX));
        const auto iy = static_cast<std::int32_t>(std::min(std::floor(cy), maxY));
        const float fx = cx - static_cast<float>(ix), fy = cy - static_cast<float>(iy);
        const std::int32_t l = level + ground_;
        auto at = [&](std::int32_t dx, std::int32_t dy) { return std::bit_cast<float>(i32(*heights_, l, ix + dx, iy + dy, 0)); };
        const float low = at(0, 0) + (at(1, 0) - at(0, 0)) * fx, high = at(0, 1) + (at(1, 1) - at(0, 1)) * fx;
        return low + (high - low) * fy;
    }

    // Whether a storey has a floor to stand on in a quarter (the ground always has).
    bool solid(std::int32_t level, std::int32_t x, std::int32_t y, std::size_t q) const {
        if (level == 0) {
            return true;
        }
        const std::int32_t l = level + ground_;
        const auto open = surface_ != nullptr ? surface_->at(l, x, y) : std::span<const std::uint8_t>{};
        return floors_ != nullptr && open.size() == 4 && floors_->i16(l, x, y, q) != 0 && open[q] != 0;
    }

    bool poolBelow(std::int32_t level, std::int32_t x, std::int32_t y, std::size_t q) const {
        const std::int32_t l = level - 1 + ground_;
        if (pools_ == nullptr || l < 0 || !pools_->contains(l, x, y)) {
            return false;
        }
        const std::int32_t kind = i32(*pools_, l, x, y, q);
        return std::find(std::begin(kPoolKinds), std::end(kPoolKinds), kind) != std::end(kPoolKinds);
    }

    bool blocked(std::int32_t level, std::int32_t x, std::int32_t y) const {
        const std::int32_t l = level + ground_;
        return blocked_ != nullptr && blocked_->contains(l, x, y) && i32(*blocked_, l, x, y, 0) != -1;
    }

    // What lies over a point: the highest roof covering it, else the highest storey with a floor
    // there, else the ground.
    float topHeight(float px, float py, std::int32_t x, std::int32_t y, std::size_t q) const {
        std::optional<float> roof;
        for (const auto& [area, grid] : roofs_) {
            if (area[0] <= px && px <= area[2] && area[1] <= py && py <= area[3]) {
                const float h = grid ? grid->surface(px, py) : std::numeric_limits<float>::infinity();
                roof = std::max(roof.value_or(-std::numeric_limits<float>::infinity()), h);
            }
        }
        if (roof) {
            return *roof;
        }
        for (std::int32_t level = top_; level >= 1; --level) {
            if (solid(level, x, y, q)) {
                return floorHeight(level, px, py);
            }
        }
        return floorHeight(0, px, py) - (poolBelow(0, x, y, q) ? kPoolDrop : 0.0f);
    }

private:
    std::int32_t ground_ = 0, top_ = -1;
    const array3d::Array3D *heights_, *floors_, *surface_, *pools_, *blocked_;
    std::vector<std::pair<std::array<float, 4>, std::optional<scene::RoofGrid>>> roofs_;
};

} // namespace

OutsideGrid::OutsideGrid(const content::Lot& lot, const RoomMap& regions) {
    const Builder b(lot);
    const auto* surface = gridOf(lot, content::kSurfaceGridInstance, 4);
    if (surface == nullptr || b.levels() <= 0) {
        return;
    }
    width_ = std::min(static_cast<std::int32_t>(surface->width), kMaxSide);
    height_ = std::min(static_cast<std::int32_t>(surface->height), kMaxSide);
    levels_ = std::min(b.levels(), kMaxLevels);
    if (static_cast<std::int32_t>(surface->width) > kMaxSide || static_cast<std::int32_t>(surface->height) > kMaxSide ||
        b.levels() > kMaxLevels) {
        diag::warnOnce("outside grid: tiles past 256 and levels past 16 are left out");
    }
    bytes_.assign(static_cast<std::size_t>(width_) * height_ * levels_, 0);
    pools_.assign(bytes_.size(), 0);
    for (std::int32_t level = 0; level < levels_; ++level) {
        for (std::int32_t x = 0; x < width_; ++x) {
            for (std::int32_t y = 0; y < height_; ++y) {
                const std::size_t at = (static_cast<std::size_t>(level) * width_ + x) * height_ + y;
                auto& byte = bytes_[at];
                for (std::size_t q = 0; q < kQuarters.size(); ++q) {
                    if (b.poolBelow(level + 1, x, y, q)) {
                        pools_[at] = static_cast<std::uint8_t>(pools_[at] | 1u << q);
                    }
                }
                for (std::size_t q = 0; q < kQuarters.size(); ++q) {
                    const float px = static_cast<float>(x) + 0.5f + kQuarters[q].dx;
                    const float py = static_cast<float>(y) + 0.5f + kQuarters[q].dy;
                    if (regions.roomAt(level, px, py) > 0) {
                        continue;
                    }
                    if (b.blocked(level, x, y)) {
                        byte = 0x10;
                        continue;
                    }
                    const float floor = b.floorHeight(level, px, py), top = b.topHeight(px, py, x, y, q);
                    const bool ok = !b.solid(level, x, y, q) || sameHeight(floor, top);
                    const bool open = ((level == 0 || top <= floor + kAbove) && ok) || b.poolBelow(level, x, y, q);
                    // A covered quarter marks the whole tile covered, dropping the quarters before it.
                    byte = open ? static_cast<std::uint8_t>(byte | kQuarters[q].bit) : std::uint8_t{0x10};
                }
            }
        }
    }
}

std::uint8_t OutsideGrid::tile(std::int32_t level, std::int32_t x, std::int32_t y) const {
    if (level < 0 || x < 0 || y < 0 || level >= levels_ || x >= width_ || y >= height_) {
        return 0;
    }
    return bytes_[(static_cast<std::size_t>(level) * width_ + x) * height_ + y];
}

bool OutsideGrid::pool(std::int32_t level, std::int32_t x, std::int32_t y, int quarter) const {
    if (level < 0 || x < 0 || y < 0 || level >= levels_ || x >= width_ || y >= height_ || quarter < 0 || quarter > 3) {
        return false;
    }
    return (pools_[(static_cast<std::size_t>(level) * width_ + x) * height_ + y] >> quarter & 1u) != 0;
}

std::optional<bool> OutsideGrid::open(std::int32_t level, float x, float y) const {
    if (!(x >= 0 && y >= 0 && x < static_cast<float>(width_) && y < static_cast<float>(height_)) || level < 0 || level >= levels_) {
        return std::nullopt;
    }
    const auto tx = static_cast<std::int32_t>(x), ty = static_cast<std::int32_t>(y);
    const std::size_t q = quarterOf(x - static_cast<float>(tx), y - static_cast<float>(ty));
    return (tile(level, tx, ty) & kQuarters[q].bit) != 0;
}

} // namespace ts2::sim
