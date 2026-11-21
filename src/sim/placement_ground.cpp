#include "sim/placement_ground.hpp"

#include <cmath>

namespace ts2::sim {

namespace {

constexpr int kQuarters = 4;
// Platform regions 3 and 5 are pools (see World::platformRegion).
constexpr std::int32_t kPool = 3, kOtherPool = 5;

template <typename Map>
std::optional<array3d::Array3D> grid(const Map& grids, std::uint32_t instance, std::size_t elementSize) {
    const auto g = grids.find(instance);
    if (g == grids.end() || g->second.elementSize != elementSize) {
        return std::nullopt;
    }
    return g->second;
}

} // namespace

PlacementGround::PlacementGround(const content::Lot& lot) : bounds_(lot.worldBounds) {
    firstLevel_ = lot.roomGraph ? lot.roomGraph->firstLevel : 0;
    floors_ = grid(lot.grids, content::kFloorGridInstance, 8);
    surfaces_ = grid(lot.grids, content::kSurfaceGridInstance, 4);
    heights_ = grid(lot.grids, content::kFloorHeightGridInstance, 4);
    regions_ = grid(lot.grids, content::kPlatformRegionGridInstance, 16);
    if (const auto w = lot.flatGrids.find(content::kWaterHeightGridInstance); w != lot.flatGrids.end()) {
        water_ = w->second;
    }
}

bool PlacementGround::validTile(const Tile& t) const {
    return bounds_ && t.x >= bounds_->minX && t.x <= bounds_->maxX && t.y >= bounds_->minY && t.y <= bounds_->maxY &&
           t.level >= bounds_->minLevel && t.level <= bounds_->maxLevel;
}

// A corner may lie on the lot's far edges.
bool PlacementGround::validCorner(std::int32_t x, std::int32_t y, std::int32_t level) const {
    return bounds_ && x >= bounds_->minX && x <= bounds_->maxX + 1 && y >= bounds_->minY && y <= bounds_->maxY + 1 &&
           level >= bounds_->minLevel && level <= bounds_->maxLevel;
}

bool PlacementGround::floorTest(const Tile& t, bool all, bool surface) const {
    const std::int32_t g = t.level - firstLevel_;
    if (!floors_ || !floors_->contains(g, t.x, t.y)) {
        return false;
    }
    for (int k = 0; k < kQuarters; ++k) {
        const bool floored = floors_->i16(g, t.x, t.y, static_cast<std::size_t>(k)) != 0;
        if (all && !floored) {
            return false;
        }
        if (!all && floored) {
            return true;
        }
    }
    if (!surface || !all) {
        return all;
    }
    if (!surfaces_ || !surfaces_->contains(g, t.x, t.y)) {
        return false;
    }
    const auto cell = surfaces_->at(g, t.x, t.y);
    if (cell.size() < static_cast<std::size_t>(kQuarters)) {
        return false;
    }
    for (int k = 0; k < kQuarters; ++k) {
        if (cell[static_cast<std::size_t>(k)] == 0) {
            return false;
        }
    }
    return true;
}

bool PlacementGround::solidFloor(const Tile& t, bool all, bool surface) const {
    return t.level == 0 || floorTest(t, all, surface);
}

std::int32_t PlacementGround::wholeRegion(const Tile& t) const {
    const std::int32_t g = t.level - firstLevel_;
    if (!validTile(t) || !regions_ || !regions_->contains(g, t.x, t.y)) {
        return -1;
    }
    const auto first = static_cast<std::int32_t>(regions_->u32(g, t.x, t.y, 0));
    for (int k = 1; k < kQuarters; ++k) {
        if (static_cast<std::int32_t>(regions_->u32(g, t.x, t.y, static_cast<std::size_t>(k))) != first) {
            return -1;
        }
    }
    return first;
}

bool PlacementGround::anyQuarterRegion(const Tile& t, std::int32_t a, std::int32_t b) const {
    const std::int32_t g = t.level - firstLevel_;
    if (!validTile(t) || !regions_ || !regions_->contains(g, t.x, t.y)) {
        return false;
    }
    for (int k = 0; k < kQuarters; ++k) {
        const auto r = static_cast<std::int32_t>(regions_->u32(g, t.x, t.y, static_cast<std::size_t>(k)));
        if (r == a || r == b) {
            return true;
        }
    }
    return false;
}

// Only the ground storey has water: a corner is under it when the ground lies below the water's height.
bool PlacementGround::cornerUnderWater(std::int32_t x, std::int32_t y, std::int32_t level) const {
    if (level != 0 || !validCorner(x, y, level) || !water_ || !heights_) {
        return false;
    }
    const std::int32_t g = -firstLevel_;
    const auto water = water_->f32(x, y);
    if (!water || !heights_->contains(g, x, y)) {
        return false;
    }
    return heights_->f32(g, x, y, 0) < *water;
}

// The game compares the four corner heights exactly.
bool PlacementGround::flat(const Tile& t) const {
    const std::int32_t g = t.level - firstLevel_;
    if (!heights_ || !heights_->contains(g, t.x + 1, t.y + 1) || t.x < 0 || t.y < 0) {
        return false;
    }
    const float z = heights_->f32(g, t.x, t.y, 0);
    return heights_->f32(g, t.x, t.y + 1, 0) == z && heights_->f32(g, t.x + 1, t.y + 1, 0) == z && heights_->f32(g, t.x + 1, t.y, 0) == z;
}

// The storey above, when it exists (and with every quarter floored when `needFloorAbove`), lies
// less than a storey (3.0) above this one at one of the tile's corners.
bool PlacementGround::lowCeiling(const Tile& t, bool needFloorAbove) const {
    constexpr float kStorey = 3.0f, kSlack = 0.001f;
    const Tile above{t.x, t.y, t.level + 1};
    if (!validTile(above) || (needFloorAbove && !floorTest(above, true, true)) || !heights_) {
        return false;
    }
    const std::int32_t g = t.level - firstLevel_;
    for (const auto [dx, dy] : {std::pair{0, 0}, {0, 1}, {1, 1}, {1, 0}}) {
        const std::int32_t cx = t.x + dx, cy = t.y + dy;
        if (!validCorner(cx, cy, t.level) || !validCorner(cx, cy, above.level) || !heights_->contains(g, cx, cy) ||
            !heights_->contains(g + 1, cx, cy)) {
            continue;
        }
        if (heights_->f32(g + 1, cx, cy, 0) - heights_->f32(g, cx, cy, 0) + kSlack < kStorey) {
            return true;
        }
    }
    return false;
}

bool PlacementGround::beach(std::optional<std::uint8_t> side, float x, float y, std::int32_t level, int mask) const {
    return level == 0 && bounds_ && content::isBeachLocation(*bounds_, side, x, y, mask);
}

std::optional<std::int16_t> PlacementGround::code(const PlacedFlags& object, float x, float y, std::int32_t level,
                                                  const FloorStandIn& standIn, std::optional<std::uint8_t> beachSide) const {
    constexpr std::uint16_t kOnFloor = 0x1, kOnGround = 0x2, kInWater = 0x4, kOnSlope = 0x100, kNeedsNoFloor = 0x200,
                            kInPool = 0x800, kOnFenceTop = 0x2000, kFloats = 0x4000;
    constexpr std::uint16_t kNeedsNoFloor3 = 0x4, kBeachFar = 0x40, kBeachMiddle = 0x80, kBeachNear = 0x100;
    constexpr std::uint16_t kNoSurfaceFlag = 0x4000;
    constexpr int kFarBand = 4, kMiddleBand = 2, kNearBand = 1;
    const std::uint16_t p = object.placement, f3 = object.flags3;
    // The float bounds take in the lot's far edges.
    if (!bounds_ || !(x >= static_cast<float>(bounds_->minX) && x <= static_cast<float>(bounds_->maxX + 1) &&
                      y >= static_cast<float>(bounds_->minY) && y <= static_cast<float>(bounds_->maxY + 1))) {
        return std::int16_t{2};
    }
    if (level < bounds_->minLevel || level > bounds_->maxLevel) {
        return std::int16_t{3};
    }
    const Tile tile{static_cast<std::int32_t>(std::floor(x)), static_cast<std::int32_t>(std::floor(y)), level};
    if (!(p & kNeedsNoFloor) && !(f3 & kNeedsNoFloor3)) {
        bool floor = solidFloor(tile, true, !(object.flags & kNoSurfaceFlag));
        if (!floor && standIn) {
            floor = standIn(tile.level, tile.x, tile.y);
        }
        // Nothing stands over a pool on the storey below.
        if (floor && anyQuarterRegion(Tile{tile.x, tile.y, tile.level - 1}, kPool, kOtherPool)) {
            floor = false;
        }
        if (!floor) {
            return std::int16_t{0x8F};
        }
    }
    // Objects that may stand on a fence or half wall aren't tested by the rest yet.
    if (p & kOnFenceTop) {
        return std::nullopt;
    }
    bool allWater = true, anyWater = false;
    for (const auto [dx, dy] : {std::pair{0, 0}, {0, 1}, {1, 0}, {1, 1}}) {
        const bool under = cornerUnderWater(tile.x + dx, tile.y + dy, tile.level);
        allWater = allWater && under;
        anyWater = anyWater || under;
    }
    if (anyWater && !(p & kInWater) && !(p & kFloats)) {
        return std::int16_t{0x91};
    }
    const std::int32_t region = wholeRegion(tile);
    const bool pool = region == kPool || region == kOtherPool;
    if (!(p & kInPool) && pool) {
        return std::int16_t{0x90};
    }
    if (!(p & kOnSlope) && !flat(tile)) {
        return std::int16_t{0x12};
    }
    const bool farBand = beach(beachSide, x, y, level, kFarBand);
    const std::int32_t below = wholeRegion(Tile{tile.x, tile.y, tile.level - 1});
    const bool poolBelow = below == kPool || below == kOtherPool;
    const bool open = !allWater && !pool && !poolBelow && !farBand;
    const std::int16_t beachCode = (f3 & kBeachFar) && !(p & kOnGround) ? std::int16_t{0xA6} : std::int16_t{0};
    if (!(p & kOnFloor) && open && floorTest(tile, false, true)) {
        return beachCode ? beachCode : std::int16_t{0x8D};
    }
    if (!(p & kOnGround) && open && solidFloor(tile, true, true) && !floorTest(tile, true, true)) {
        return beachCode ? beachCode : std::int16_t{0x8E};
    }
    if (!(f3 & kBeachFar) && farBand) {
        return std::int16_t{0x9F};
    }
    if (!(f3 & kBeachMiddle) && beach(beachSide, x, y, level, kMiddleBand)) {
        return std::int16_t{0xA0};
    }
    if (!(f3 & kBeachNear) && beach(beachSide, x, y, level, kNearBand)) {
        return std::int16_t{0xA1};
    }
    // Data 0x52 bit 0x20 lets an object stand under a low ceiling. Objects of type 5 and 10 don't need
    // the storey above to be floored.
    constexpr std::uint16_t kUnderLowCeiling = 0x20;
    constexpr int kType5 = 5, kType10 = 10;
    if (!(f3 & kUnderLowCeiling) && lowCeiling(tile, object.type != kType5 && object.type != kType10)) {
        return std::int16_t{0x2F};
    }
    return std::nullopt;
}

} // namespace ts2::sim
