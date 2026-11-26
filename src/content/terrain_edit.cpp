#include "terrain_edit.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace ts2::content {

namespace {

constexpr float kStorey = 3.0f;
constexpr float kGap = 0.75f;       // the least gap between a corner and the same corner a level up
constexpr float kScale = 0.1875f;   // the world's height unit
constexpr int kFlattenErrorBelowWater = 0x47;
// Pond water (TSData/Sys/Terrain.ini): dry ground's water sits this far under it, and a corner is wet
// while its ground is at most this above the water.
constexpr float kWaterUnderDryGround = 0.5f;
constexpr float kWetReach = 0.3f;
constexpr float kShoreDrop = 0.1f;   // a dry corner's water under its wet neighbour's
constexpr std::int32_t kNoPiece = -1;
// Per tile, a foundation or deck piece's id and shape (two 32-bit numbers), id -1 for none.
constexpr std::uint32_t kPieceGridInstance = 0x1B;
constexpr std::int32_t kPoolRegionKinds[] = {3, 5};

// The tiles round a corner in the game's order, with the quarters that touch it (bits 1 -x, 2 +y,
// 4 +x, 8 -y).
struct RoundTile {
    std::int32_t dx, dy;
    std::uint8_t quarters;
};
constexpr std::array<RoundTile, 4> kRound{{{-1, -1, 0x6}, {0, -1, 0x3}, {0, 0, 0x9}, {-1, 0, 0xC}}};
// The eight neighbours of a pond corner, in the game's order.
constexpr std::array<std::array<std::int32_t, 2>, 8> kNeighbours{{{1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}, {0, 1}, {1, 1}}};

void writeFloat(std::vector<std::uint8_t>& elements, std::size_t at, float v) {
    if (at + 4 <= elements.size()) {
        const auto bits = std::bit_cast<std::uint32_t>(v);
        for (std::size_t i = 0; i < 4; ++i) {
            elements[at + i] = static_cast<std::uint8_t>(bits >> (8 * i));
        }
    }
}

std::int32_t readInt(std::span<const std::uint8_t> e, std::size_t index) {
    std::int32_t v = kNoPiece;
    if ((index + 1) * 4 <= e.size()) {
        std::memcpy(&v, e.data() + index * 4, 4);
    }
    return v;
}

} // namespace

void TerrainChanges::noteHeight(const TerrainCorner& corner, float before, float after) {
    const auto [it, added] = heights_.try_emplace({corner.x, corner.y, corner.level}, Height{corner, before, after});
    if (!added) {
        it->second.after = after;
    }
}

void TerrainChanges::noteWater(std::int32_t x, std::int32_t y, float before, float after) {
    const auto [it, added] = water_.try_emplace({x, y}, Water{x, y, before, after});
    if (!added) {
        it->second.after = after;
    }
}

std::vector<TerrainChanges::Height> TerrainChanges::heights() const {
    std::vector<Height> out;
    for (const auto& [key, h] : heights_) {
        out.push_back(h);
    }
    return out;
}

std::vector<TerrainChanges::Water> TerrainChanges::water() const {
    std::vector<Water> out;
    for (const auto& [key, w] : water_) {
        out.push_back(w);
    }
    return out;
}

void TerrainChanges::toggle(Lot& lot) {
    const WorldBounds b = lot.worldBounds.value_or(WorldBounds{});
    if (auto g = lot.grids.find(kFloorHeightGridInstance); g != lot.grids.end() && g->second.elementSize == 4) {
        auto& grid = g->second;
        for (auto& [key, h] : heights_) {
            const std::int32_t l = gridIndex(lot, h.corner.level), x = h.corner.x - b.minX, y = h.corner.y - b.minY;
            if (grid.contains(l, x, y)) {
                writeFloat(grid.elements, ((static_cast<std::size_t>(l) * grid.width + x) * grid.height + y) * 4, h.before);
            }
            std::swap(h.before, h.after);
        }
    }
    if (auto w = lot.flatGrids.find(kWaterHeightGridInstance); w != lot.flatGrids.end() && w->second.elementSize == 4) {
        auto& grid = w->second;
        for (auto& [key, v] : water_) {
            if (v.x >= 0 && v.y >= 0 && static_cast<std::uint32_t>(v.x) < grid.width && static_cast<std::uint32_t>(v.y) < grid.height) {
                writeFloat(grid.elements, (static_cast<std::size_t>(v.y) * grid.width + v.x) * 4, v.before);
            }
            std::swap(v.before, v.after);
        }
    }
}

TerrainEdit::TerrainEdit(Lot& lot, const WallManager& walls, const BuildScripts& scripts, TerrainObjects objects, TerrainChanges& changes,
                         std::optional<std::uint8_t> beachSide)
    : lot_(lot), walls_(walls), scripts_(scripts), objects_(std::move(objects)), changes_(changes), bounds_(lot.worldBounds.value_or(WorldBounds{})),
      beachSide_(beachSide) {}

float TerrainEdit::height(const TerrainCorner& c) const {
    const auto g = lot_.grids.find(kFloorHeightGridInstance);
    if (g == lot_.grids.end() || g->second.elementSize != 4) {
        return 0;
    }
    const std::int32_t l = gridIndex(lot_, c.level), x = c.x - bounds_.minX, y = c.y - bounds_.minY;
    return g->second.contains(l, x, y) ? g->second.f32(l, x, y, 0) : 0.0f;
}

void TerrainEdit::put(const TerrainCorner& c, float h) {
    const auto g = lot_.grids.find(kFloorHeightGridInstance);
    if (g == lot_.grids.end() || g->second.elementSize != 4 || !cornerInLot(c)) {
        return;
    }
    auto& grid = g->second;
    const std::int32_t l = gridIndex(lot_, c.level), x = c.x - bounds_.minX, y = c.y - bounds_.minY;
    if (grid.contains(l, x, y)) {
        writeFloat(grid.elements, ((static_cast<std::size_t>(l) * grid.width + x) * grid.height + y) * 4, h);
    }
}

bool TerrainEdit::cornerInLot(const TerrainCorner& c) const {
    if (bounds_.minX >= bounds_.maxX || bounds_.minY >= bounds_.maxY) {
        return false;
    }
    return c.x >= bounds_.minX && c.x <= bounds_.maxX + 1 && c.y >= bounds_.minY && c.y <= bounds_.maxY + 1 && c.level >= bounds_.minLevel &&
           c.level <= bounds_.maxLevel;
}

bool TerrainEdit::tileInLot(const TerrainCorner& t) const {
    return t.x >= bounds_.minX && t.x <= bounds_.maxX && t.y >= bounds_.minY && t.y <= bounds_.maxY && t.level >= bounds_.minLevel &&
           t.level <= bounds_.maxLevel;
}

void TerrainEdit::refuse(std::vector<TerrainRefusal>* refusals, int code, float x, float y, std::int32_t level) const {
    if (refusals != nullptr) {
        refusals->push_back({code, x, y, level});
    }
}

bool TerrainEdit::needsFlat(std::uint32_t style, bool top) const {
    // A half wall and a fence that bears load keep the ground under them level, and no fence keeps the
    // corners a storey up level.
    if (const auto fence = walls_.fence(style)) {
        return !top && (fence->halfWall || fence->bearsLoad);
    }
    const auto s = scripts_.wallStyles.find(style);
    return s != scripts_.wallStyles.end() && (top ? s->second.flatTop : s->second.flatBottom);
}

bool TerrainEdit::tileLocked(const TerrainCorner& t) const {
    const auto grid = lot_.grids.find(kLockedTileGridInstance);
    if (grid == lot_.grids.end() || grid->second.elementSize == 0) {
        return false;
    }
    const std::int32_t l = gridIndex(lot_, t.level), x = t.x - bounds_.minX, y = t.y - bounds_.minY;
    return grid->second.contains(l, x, y) && grid->second.at(l, x, y)[0] != 0;
}

// The tiles round the corner must be on the lot and not locked (the road and sidewalk tiles).
bool TerrainEdit::tilesBad(const TerrainCorner& c, std::vector<TerrainRefusal>* refusals) const {
    bool bad = false;
    if (c.level == 0 && (c.x == bounds_.minX || c.y == bounds_.minY || c.x == bounds_.maxX + 1 || c.y == bounds_.maxY + 1)) {
        if (refusals == nullptr) {
            return true;
        }
        refuse(refusals, 0x63, static_cast<float>(c.x), static_cast<float>(c.y), c.level);
        bad = true;
    }
    for (const auto& r : kRound) {
        const TerrainCorner t{c.x + r.dx, c.y + r.dy, c.level};
        if (!tileInLot(t) || tileLocked(t)) {
            if (refusals == nullptr) {
                return true;
            }
            refuse(refusals, 0x63, static_cast<float>(t.x) + 0.5f, static_cast<float>(t.y) + 0.5f, t.level);
            bad = true;
        }
    }
    return bad;
}

bool TerrainEdit::floorBad(const TerrainCorner& c, std::vector<TerrainRefusal>* refusals) const {
    const auto floors = lot_.grids.find(kFloorGridInstance);
    if (floors == lot_.grids.end() || floors->second.elementSize != 8) {
        return false;
    }
    bool bad = false;
    for (const auto& r : kRound) {
        const TerrainCorner t{c.x + r.dx, c.y + r.dy, c.level};
        if (!tileInLot(t)) {
            continue;
        }
        const std::int32_t l = gridIndex(lot_, t.level), x = t.x - bounds_.minX, y = t.y - bounds_.minY;
        bool floor = false;
        for (std::size_t q = 0; q < 4; ++q) {
            floor = floor || floors->second.i16(l, x, y, q) != 0;
        }
        if (floor) {
            if (refusals == nullptr) {
                return true;
            }
            refuse(refusals, 0x5B, static_cast<float>(t.x) + 0.5f, static_cast<float>(t.y) + 0.5f, t.level);
            bad = true;
        }
    }
    return bad;
}

float TerrainEdit::groundAt(const TerrainCorner& changed, float h, float x, float y, std::int32_t level) const {
    const float ix = std::floor(x), iy = std::floor(y);
    const float fx = x - ix, fy = y - iy;
    const auto hv = [&](std::int32_t a, std::int32_t b) {
        return a == changed.x && b == changed.y && level == changed.level ? h : height({a, b, level});
    };
    const auto x0 = static_cast<std::int32_t>(ix), y0 = static_cast<std::int32_t>(iy);
    return (hv(x0, y0) * (1 - fx) + hv(x0 + 1, y0) * fx) * (1 - fy) + (hv(x0, y0 + 1) * (1 - fx) + hv(x0 + 1, y0 + 1) * fx) * fy;
}

bool TerrainEdit::objectsBad(const TerrainCorner& c, float h, std::vector<TerrainRefusal>* refusals) const {
    constexpr std::uint16_t kOnUnevenGround = 0x100, kOnGround = 0x80;
    constexpr std::int32_t kPersonType = 2, kStairsType = 11, kStairsPortalType = 12;
    if (!objects_) {
        return false;
    }
    const bool tooShort = c.level > bounds_.minLevel && kStorey > h - height({c.x, c.y, c.level - 1}) + 0.001f;
    bool bad = false;
    for (const auto& r : kRound) {
        const TerrainCorner t{c.x + r.dx, c.y + r.dy, c.level};
        if (!tileInLot(t)) {
            continue;
        }
        const auto here = objects_(t.x, t.y, t.level);
        const bool blocked = std::any_of(here.begin(), here.end(), [](const TerrainObject& o) {
            return o.sim || o.type == kPersonType || !(o.placement & kOnUnevenGround);
        });
        if (blocked) {
            if (refusals == nullptr) {
                return true;
            }
            refuse(refusals, 0x62, static_cast<float>(t.x) + 0.5f, static_cast<float>(t.y) + 0.5f, t.level);
            bad = true;
        }
        if (tooShort) {
            const auto below = objects_(t.x, t.y, t.level - 1);
            if (std::any_of(below.begin(), below.end(), [](const TerrainObject& o) { return !o.belowExempt; })) {
                if (refusals == nullptr) {
                    return true;
                }
                refuse(refusals, 0x61, static_cast<float>(t.x) + 0.5f, static_cast<float>(t.y) + 0.5f, t.level - 1);
                bad = true;
            }
        }
        for (const auto& o : here) {
            if (!(o.placement & kOnGround)) {
                continue;
            }
            if (o.type != kStairsType && o.type != kStairsPortalType) {
                if (o.elevation < groundAt(c, h, o.x, o.y, t.level)) {
                    if (refusals == nullptr) {
                        return true;
                    }
                    refuse(refusals, 0x69, o.x, o.y, t.level);
                    bad = true;
                }
                continue;
            }
            // The stairs' four points by facing. The game never set offsets for other facings.
            std::array<float, 4> dx{}, dy{};
            switch (o.direction) {
            case 0: dx = {-0.5f, -0.5f, 0.5f, 0.5f}, dy = {0, -0.25f, -0.25f, 0}; break;
            case 2: dx = {0, 0.25f, 0.25f, 0}, dy = {-0.5f, -0.5f, 0.5f, 0.5f}; break;
            case 4: dx = {0.5f, 0.5f, -0.5f, -0.5f}, dy = {0, 0.25f, 0.25f, 0}; break;
            case 6: dx = {0, -0.25f, -0.25f, 0}, dy = {0.5f, 0.5f, -0.5f, -0.5f}; break;
            default: continue;
            }
            constexpr std::array<float, 4> dz{0, kScale, kScale, 0};
            for (std::size_t k = 0; k < 4; ++k) {
                const float px = o.x + dx[k], py = o.y + dy[k];
                const bool inside = px >= static_cast<float>(t.x) && px <= static_cast<float>(t.x + 1) && py >= static_cast<float>(t.y) &&
                                    py <= static_cast<float>(t.y + 1);
                if (inside && o.elevation + dz[k] < groundAt(c, h, px, py, t.level)) {
                    if (refusals == nullptr) {
                        return true;
                    }
                    refuse(refusals, 0x69, px, py, t.level);
                    bad = true;
                }
            }
        }
    }
    return bad;
}

bool TerrainEdit::upBad(const TerrainCorner& c, float h, std::uint32_t flags, std::vector<TerrainRefusal>* refusals) const {
    if (c.level >= bounds_.maxLevel) {
        return false;
    }
    const TerrainCorner up{c.x, c.y, c.level + 1};
    if (height(up) > h + kGap + 0.25f * kScale || check(up, h + kGap, flags & ~0x20u, nullptr)) {
        return false;
    }
    refuse(refusals, 0x6A, static_cast<float>(up.x), static_cast<float>(up.y), up.level);
    return true;
}

bool TerrainEdit::downBad(const TerrainCorner& c, float h, std::uint32_t flags, std::vector<TerrainRefusal>* refusals) const {
    if (c.level <= bounds_.minLevel) {
        return false;
    }
    const TerrainCorner down{c.x, c.y, c.level - 1};
    if (height(down) < h - kGap - 0.25f * kScale || check(down, h - kGap, flags & ~0x10u, nullptr)) {
        return false;
    }
    refuse(refusals, 0x6B, static_cast<float>(down.x), static_cast<float>(down.y), down.level);
    return true;
}

// Walls at the corner, or on the far diagonals of the tiles round it, that keep their ground flat.
bool TerrainEdit::wallsBad(const TerrainCorner& c, std::vector<TerrainRefusal>* refusals) const {
    const auto found = [&](std::int32_t level, bool top, int code) {
        const float x = static_cast<float>(c.x), y = static_cast<float>(c.y);
        for (const auto& w : walls_.wallsAt(x, y, level)) {
            if (needsFlat(w.style, top)) {
                refuse(refusals, code, (w.segment.ax + w.segment.bx) / 2, (w.segment.ay + w.segment.by) / 2, level);
                return true;
            }
        }
        const std::array<WallSegment, 4> diagonals{{{x, y + 1, x - 1, y, level},
                                                    {x - 1, y, x, y - 1, level},
                                                    {x, y - 1, x + 1, y, level},
                                                    {x + 1, y, x, y + 1, level}}};
        for (const auto& d : diagonals) {
            if (const auto style = walls_.styleOn(d); style && needsFlat(*style, top)) {
                refuse(refusals, code, (d.ax + d.bx) / 2, (d.ay + d.by) / 2, level);
                return true;
            }
        }
        return false;
    };
    return found(c.level, false, 0x5C) || found(c.level - 1, true, 0x5D);
}

// A tile round the corner that still holds a floor up.
bool TerrainEdit::floorHeldBad(const TerrainCorner& c, std::vector<TerrainRefusal>* refusals) const {
    if (!floorAllowed_) {
        return false;
    }
    for (const auto& r : kRound) {
        const TerrainCorner t{c.x + r.dx, c.y + r.dy, c.level};
        if (tileInLot(t) && floorAllowed_(t)) {
            refuse(refusals, 0x6C, static_cast<float>(c.x), static_cast<float>(c.y), c.level);
            return true;
        }
    }
    return false;
}

// Whether a platform is one storey down at any corner of the four tiles round the corner, in the
// quarters touching that corner.
bool TerrainEdit::platformNear(const TerrainCorner& c) const {
    const auto regions = lot_.grids.find(kPlatformRegionGridInstance);
    constexpr std::size_t kCellSize = 16;
    if (regions == lot_.grids.end() || regions->second.elementSize != kCellSize) {
        return false;
    }
    // The quarters' platform when they all have the same one, else none.
    const auto platformOn = [&](const TerrainCorner& t, std::uint8_t quarters) {
        const auto cell = regions->second.at(gridIndex(lot_, t.level), t.x - bounds_.minX, t.y - bounds_.minY);
        std::optional<std::int32_t> id;
        for (std::size_t q = 0; q < 4; ++q) {
            if (quarters & (1u << q)) {
                const std::int32_t v = readInt(cell, q);
                if (id && *id != v) {
                    return false;
                }
                id = v;
            }
        }
        return id && *id != kNoPiece;
    };
    constexpr std::array<std::array<std::int32_t, 2>, 4> kTileCorners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (const auto& round : kRound) {
        for (const auto& [cx, cy] : kTileCorners) {
            const TerrainCorner p{c.x + round.dx + cx, c.y + round.dy + cy, c.level - 1};
            for (const auto& r : kRound) {
                const TerrainCorner t{p.x + r.dx, p.y + r.dy, p.level};
                if (tileInLot(t) && platformOn(t, r.quarters)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void TerrainEdit::platformRemoved(const TerrainCorner& c) {
    const TerrainCorner below{c.x, c.y, c.level - 1};
    if (!cornerInLot(c) || !cornerInLot(below) || c.x <= bounds_.minX || c.x > bounds_.maxX || c.y <= bounds_.minY || c.y > bounds_.maxY) {
        return;
    }
    const float target = height(below) + kStorey;
    if (target == height(c)) {
        return;
    }
    std::uint32_t flags = kWallFlags;
    if (c.level != 1 || platformNear(c)) {
        flags ^= 8;
    }
    if (check(c, target, flags, nullptr)) {
        setHeight(c, target);
    }
}

// Pool water on the storey below, in the quarters touching the corner.
bool TerrainEdit::poolBad(const TerrainCorner& c) const {
    const auto regions = lot_.grids.find(kPlatformRegionGridInstance);
    constexpr std::size_t kCellSize = 16;
    if (regions == lot_.grids.end() || regions->second.elementSize != kCellSize) {
        return false;
    }
    for (const auto& r : kRound) {
        const TerrainCorner b{c.x + r.dx, c.y + r.dy, c.level - 1};
        if (!tileInLot(b) || b.level >= bounds_.maxLevel) {
            continue;
        }
        const auto cell = regions->second.at(gridIndex(lot_, b.level), b.x - bounds_.minX, b.y - bounds_.minY);
        std::optional<std::int32_t> id;
        bool same = true;
        for (std::size_t q = 0; q < 4; ++q) {
            if (r.quarters & (1u << q)) {
                const std::int32_t v = readInt(cell, q);
                same = same && (!id || *id == v);
                id = v;
            }
        }
        if (same && id && std::find(std::begin(kPoolRegionKinds), std::end(kPoolRegionKinds), *id) != std::end(kPoolRegionKinds)) {
            return true;
        }
    }
    return false;
}

// Whether the quarters of `q` all have the same pool region one storey down.
bool TerrainEdit::poolUnder(const TileQuarter& q) const {
    const auto regions = lot_.grids.find(kPlatformRegionGridInstance);
    constexpr std::size_t kCellSize = 16;
    const TerrainCorner below{q.x, q.y, q.level - 1};
    if (regions == lot_.grids.end() || regions->second.elementSize != kCellSize || !tileInLot(below) || below.level >= bounds_.maxLevel) {
        return false;
    }
    const auto cell = regions->second.at(gridIndex(lot_, below.level), below.x - bounds_.minX, below.y - bounds_.minY);
    std::optional<std::int32_t> id;
    for (std::size_t i = 0; i < 4; ++i) {
        if (q.mask & (1u << i)) {
            const std::int32_t v = readInt(cell, i);
            if (id && *id != v) {
                return false;
            }
            id = v;
        }
    }
    return id && std::find(std::begin(kPoolRegionKinds), std::end(kPoolRegionKinds), *id) != std::end(kPoolRegionKinds);
}

bool TerrainEdit::overPool(const WallSegment& section, std::vector<TerrainRefusal>& refusals) const {
    const TileQuarter sides[] = {leftQuarter(section), leftQuarter(section.reversed())};
    for (const auto& q : sides) {
        if (tileInLot({q.x, q.y, q.level}) && !poolUnder(q)) {
            return false;
        }
    }
    // The message points at the first side's quarter, a third of a tile from the tile's middle.
    const auto& q = sides[0];
    constexpr float kThird = 1.0f / 3.0f;
    const float x = static_cast<float>(q.x) + 0.5f + ((q.mask & 1) ? -kThird : 0) + ((q.mask & 4) ? kThird : 0);
    const float y = static_cast<float>(q.y) + 0.5f + ((q.mask & 2) ? kThird : 0) + ((q.mask & 8) ? -kThird : 0);
    refuse(&refusals, 0x46, x, y, q.level);
    return true;
}

// A fence at the corner that the new height would make steeper than 60 degrees.
bool TerrainEdit::fenceBad(const TerrainCorner& c, float h, std::vector<TerrainRefusal>* refusals) const {
    constexpr std::uint32_t kFirstFenceStyle = 0x10000;  // smaller styles are walls.txt walls
    constexpr float kSteepest = 1.7320508f;
    const float x = static_cast<float>(c.x), y = static_cast<float>(c.y);
    for (const auto& w : walls_.wallsAt(x, y, c.level)) {
        if (w.style < kFirstFenceStyle) {
            continue;
        }
        const float qx = w.segment.bx, qy = w.segment.by;
        const float dz = height({static_cast<std::int32_t>(qx), static_cast<std::int32_t>(qy), c.level}) - h;
        const float len = std::hypot(qx - x, qy - y);
        if (len > 0 && std::fabs(dz) / len > kSteepest) {
            refuse(refusals, 0x31, (x + qx) / 2, (y + qy) / 2, c.level);
            return true;
        }
    }
    return false;
}

bool TerrainEdit::pieceBad(const TerrainCorner& c) const {
    const auto pieces = lot_.grids.find(kPieceGridInstance);
    if (pieces == lot_.grids.end() || pieces->second.elementSize != 8 || c.level > bounds_.maxLevel) {
        return false;
    }
    for (const auto& r : kRound) {
        const TerrainCorner t{c.x + r.dx, c.y + r.dy, c.level};
        if (tileInLot(t) && readInt(pieces->second.at(gridIndex(lot_, t.level), t.x - bounds_.minX, t.y - bounds_.minY), 0) != kNoPiece) {
            return true;
        }
    }
    return false;
}

bool TerrainEdit::pieceOn(const TerrainCorner& t) const {
    const auto pieces = lot_.grids.find(kPieceGridInstance);
    if (pieces == lot_.grids.end() || pieces->second.elementSize != 8 || !tileInLot(t)) {
        return false;
    }
    const std::int32_t l = gridIndex(lot_, t.level), x = t.x - bounds_.minX, y = t.y - bounds_.minY;
    return pieces->second.contains(l, x, y) && readInt(pieces->second.at(l, x, y), 0) != kNoPiece;
}

bool TerrainEdit::beachBad(const TerrainCorner& c) const {
    constexpr int kAllBands = 7;
    return c.level == 0 && cornerInLot(c) && isBeachLocation(bounds_, beachSide_, c.x, c.y, kAllBands);
}

bool TerrainEdit::check(const TerrainCorner& c, float h, std::uint32_t flags, std::vector<TerrainRefusal>* refusals) const {
    if (!cornerInLot(c)) {
        refuse(refusals, 0x5A, static_cast<float>(c.x), static_cast<float>(c.y), c.level);
        return false;
    }
    bool ok = true;
    const auto failed = [&](bool bad) {
        ok = ok && !bad;
        return bad && refusals == nullptr;
    };
    if ((flags & 1) && failed(tilesBad(c, refusals))) {
        return false;
    }
    if (height(c) == h) {
        return ok;
    }
    if (flags & 2) {
        constexpr float kHighest = 150 * kScale;
        if (-kHighest > h) {
            refuse(refusals, 0x66, static_cast<float>(c.x), static_cast<float>(c.y), c.level);
            if (failed(true)) {
                return false;
            }
        } else if (kHighest < h) {
            refuse(refusals, 0x67, static_cast<float>(c.x), static_cast<float>(c.y), c.level);
            if (failed(true)) {
                return false;
            }
        }
    }
    // The rest only while the game keeps floors to their heights (constrainFloorElevation, on in play).
    if ((flags & 8) && failed(floorHeldBad(c, refusals))) {
        return false;
    }
    if ((flags & 4) && failed(floorBad(c, refusals))) {
        return false;
    }
    if ((flags & 0x80) && failed(objectsBad(c, h, refusals))) {
        return false;
    }
    if ((flags & 0x10) && failed(upBad(c, h, flags, refusals))) {
        return false;
    }
    if ((flags & 0x20) && failed(downBad(c, h, flags, refusals))) {
        return false;
    }
    if ((flags & 0x100) && failed(wallsBad(c, refusals))) {
        return false;
    }
    if ((flags & 0x200) && failed(poolBad(c))) {
        return false;
    }
    if ((flags & 0x800) && failed(fenceBad(c, h, refusals))) {
        return false;
    }
    if ((flags & 0x2000) && failed(pieceBad(c))) {
        return false;
    }
    if (failed(beachBad(c))) {
        return false;
    }
    return ok;
}

void TerrainEdit::write(const TerrainCorner& c, float h) {
    changes_.noteHeight(c, height(c), h);
    put(c, h);
}

// Keeps the levels above and below at least a gap from a corner set to `h`.
void TerrainEdit::push(const TerrainCorner& c, float h) {
    if (c.level < bounds_.maxLevel) {
        const TerrainCorner up{c.x, c.y, c.level + 1};
        if (height(up) <= h + kGap - 0.25f * kScale) {
            write(up, h + kGap);
            push(up, h + kGap);
        }
    }
    if (c.level > bounds_.minLevel) {
        const TerrainCorner down{c.x, c.y, c.level - 1};
        if (height(down) >= h - kGap + 0.25f * kScale) {
            write(down, h - kGap);
            push(down, h - kGap);
        }
    }
}

// Sets a corner. The basement corner under the ground follows, and the storeys above move by as much
// where the check allows.
void TerrainEdit::setHeight(const TerrainCorner& c, float h) {
    const float old = height(c);
    if (old == h) {
        return;
    }
    const float delta = h - old;
    if (c.level == 0 && bounds_.minLevel < 0) {
        write({c.x, c.y, -1}, h - kStorey);
    }
    std::vector<float> saved;
    for (std::int32_t l = c.level + 1; l <= bounds_.maxLevel; ++l) {
        saved.push_back(height({c.x, c.y, l}));
    }
    write(c, h);
    push(c, h);
    for (std::int32_t l = c.level + 1; l <= bounds_.maxLevel; ++l) {
        if (l == 0) {
            break;
        }
        const TerrainCorner up{c.x, c.y, l};
        const float v = saved[static_cast<std::size_t>(l - c.level - 1)] + delta;
        if (!check(up, v, kWallFlags, nullptr)) {
            break;
        }
        write(up, v);
        push(up, v);
    }
}

// Works pond water out again after a ground corner moved. Wet corners join their pond, whose water
// settles under the lowest dry ground round it.
void TerrainEdit::reflood(const TerrainCorner& corner, float terrainBefore) {
    auto w = lot_.flatGrids.find(kWaterHeightGridInstance);
    if (w == lot_.flatGrids.end() || w->second.elementSize != 4) {
        return;
    }
    auto& grid = w->second;
    const auto inGrid = [&](std::int32_t x, std::int32_t y) {
        return x >= 0 && y >= 0 && static_cast<std::uint32_t>(x) < grid.width && static_cast<std::uint32_t>(y) < grid.height;
    };
    const auto water = [&](std::int32_t x, std::int32_t y) { return inGrid(x, y) ? grid.f32(x, y).value_or(0.0f) : 0.0f; };
    const auto setWater = [&](std::int32_t x, std::int32_t y, float v) {
        if (inGrid(x, y)) {
            writeFloat(grid.elements, (static_cast<std::size_t>(y) * grid.width + x) * 4, v);
        }
    };
    const auto terrain = [&](std::int32_t x, std::int32_t y) { return height({x, y, 0}); };
    const auto wet = [&](std::int32_t x, std::int32_t y) { return terrain(x, y) - water(x, y) <= kWetReach; };
    struct Entry {
        std::int32_t x, y;
        float terrain, before, after;
        bool visited = false, pond = false;
    };
    std::vector<Entry> list;
    std::map<std::pair<std::int32_t, std::int32_t>, std::size_t> listed;  // each corner's place in the list
    const auto add = [&](std::int32_t x, std::int32_t y, bool pond) {
        listed.emplace(std::pair{x, y}, list.size());
        list.push_back({x, y, terrain(x, y), water(x, y), water(x, y), false, pond});
    };
    add(corner.x, corner.y, false);
    // The corner as it stood before the change decides whether it was wet.
    {
        Entry& e = list.front();
        if (!(terrainBefore - e.before <= kWetReach)) {
            e.after = e.terrain - kWaterUnderDryGround;
            setWater(e.x, e.y, e.after);
        }
        e.pond = e.terrain - e.after <= kWetReach;
    }
    // The pass reaches the corners it adds too, so the whole wet patch is listed in one.
    for (std::size_t i = 0; i < list.size(); ++i) {
        for (const auto& [dx, dy] : kNeighbours) {
            const std::int32_t nx = list[i].x + dx, ny = list[i].y + dy;
            if (inGrid(nx, ny) && !listed.contains({nx, ny}) && wet(nx, ny)) {
                add(nx, ny, true);
            }
        }
    }
    const std::size_t count = list.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (list[i].visited) {
            continue;
        }
        if (!list[i].pond) {
            list[i].visited = true;
            continue;
        }
        std::vector<std::size_t> group{i};
        list[i].visited = true;
        float lowest = std::numeric_limits<float>::max();
        for (std::size_t g = 0; g < group.size(); ++g) {
            const Entry here = list[group[g]];
            for (const auto& [dx, dy] : kNeighbours) {
                const std::int32_t nx = here.x + dx, ny = here.y + dy;
                if (!inGrid(nx, ny)) {
                    continue;
                }
                if (!listed.contains({nx, ny})) {
                    add(nx, ny, false);
                }
                const std::size_t index = listed.at({nx, ny});
                Entry& at = list[index];
                if (at.pond) {
                    if (!at.visited) {
                        at.visited = true;
                        group.push_back(index);
                    }
                } else {
                    lowest = std::min(lowest, at.terrain);
                }
            }
        }
        const float level = lowest - kWetReach;
        for (const std::size_t g : group) {
            Entry& e = list[g];
            e.after = e.terrain - level <= kWetReach ? level : e.terrain - kWaterUnderDryGround;
            setWater(e.x, e.y, e.after);
        }
    }
    for (Entry& e : list) {
        if (wet(e.x, e.y)) {
            continue;
        }
        e.after = e.terrain - kWaterUnderDryGround;
        for (const auto& [dx, dy] : kNeighbours) {
            const std::int32_t nx = e.x + dx, ny = e.y + dy;
            if (inGrid(nx, ny) && wet(nx, ny)) {
                e.after = water(nx, ny) - kShoreDrop;
                break;
            }
        }
        setWater(e.x, e.y, e.after);
    }
    for (const Entry& e : list) {
        if (e.after != e.before) {
            changes_.noteWater(e.x, e.y, e.before, e.after);
        }
    }
}

// One corner to `h`: refused under a pond's water or by the check, else set.
bool TerrainEdit::flattenOne(const TerrainCorner& c, float h, std::vector<TerrainRefusal>& refusals) {
    bool ok = true;
    if (c.level == 0 && cornerInLot(c)) {
        const auto w = lot_.flatGrids.find(kWaterHeightGridInstance);
        const auto water = w != lot_.flatGrids.end() ? w->second.f32(c.x, c.y) : std::nullopt;
        if (water && height(c) < *water) {
            refuse(&refusals, kFlattenErrorBelowWater, static_cast<float>(c.x), static_cast<float>(c.y), c.level);
            ok = false;
        }
    }
    if (!check(c, h, kWallFlags, &refusals)) {
        return false;
    }
    if (ok && height(c) != h) {
        const float before = height({c.x, c.y, 0});
        setHeight(c, h);
        reflood({c.x, c.y, 0}, before);
    }
    return ok;
}

bool TerrainEdit::flattenCorner(const TerrainCorner& c, float h, std::uint32_t style, std::vector<TerrainRefusal>& refusals) {
    if (!needsFlat(style, false)) {
        return true;
    }
    if (!flattenOne(c, h, refusals)) {
        return false;
    }
    const TerrainCorner up{c.x, c.y, c.level + 1};
    if (!needsFlat(style, true) || !cornerInLot(up)) {
        return true;
    }
    return flattenOne(up, height(c) + kStorey, refusals);
}

bool TerrainEdit::flattenSection(const WallSegment& s, float h, std::uint32_t style, std::vector<TerrainRefusal>& refusals) {
    const auto trunc = [](float v) { return static_cast<std::int32_t>(v); };
    if (s.ax == s.bx || s.ay == s.by) {
        const bool end = flattenCorner({trunc(s.bx), trunc(s.by), s.level}, h, style, refusals);
        const bool start = flattenCorner({trunc(s.ax), trunc(s.ay), s.level}, h, style, refusals);
        return end && start;
    }
    bool ok = true;
    for (std::int32_t x = trunc(std::min(s.ax, s.bx)); x <= trunc(std::max(s.ax, s.bx)); ++x) {
        for (std::int32_t y = trunc(std::min(s.ay, s.by)); y <= trunc(std::max(s.ay, s.by)); ++y) {
            ok = flattenCorner({x, y, s.level}, h, style, refusals) && ok;
        }
    }
    return ok;
}

bool TerrainEdit::flattenBox(const TerrainCorner& a, const TerrainCorner& b, float h, std::uint32_t style, std::vector<TerrainRefusal>& refusals) {
    bool ok = true;
    for (std::int32_t x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x) {
        for (std::int32_t y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y) {
            ok = flattenCorner({x, y, a.level}, h, style, refusals) && ok;
        }
    }
    return ok;
}

void TerrainEdit::flattenFloorBox(const TerrainCorner& a, const TerrainCorner& b, float h) {
    // The floor tool keeps no reasons and lays its floors whatever was refused.
    std::vector<TerrainRefusal> refused;
    for (std::int32_t x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x) {
        for (std::int32_t y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y) {
            (void)flattenOne({x, y, a.level}, h, refused);
        }
    }
}

bool TerrainEdit::flattenDiamond(const TerrainCorner& from, const TerrainCorner& to, float h, std::uint32_t style, std::vector<TerrainRefusal>& refusals) {
    const std::int32_t dx = to.x - from.x, dy = to.y - from.y;
    if (std::abs(dx) == std::abs(dy)) {
        return true;
    }
    // A is the end with the smaller coordinate along the longer side.
    const bool alongY = std::abs(dx) < std::abs(dy);
    const bool fromFirst = alongY ? from.y <= to.y : from.x <= to.x;
    const TerrainCorner a = fromFirst ? from : to, b = fromFirst ? to : from;
    const std::int32_t ua = a.x + a.y, va = a.x - a.y, ub = b.x + b.y, vb = b.x - b.y;
    const TerrainCorner s1{(ua + vb) / 2, (ua - vb) / 2, a.level}, s2{(ub + va) / 2, (ub - va) / 2, a.level};
    std::array<TerrainCorner, 4> tips = alongY ? std::array<TerrainCorner, 4>{s1, s2, a, b} : std::array<TerrainCorner, 4>{a, b, s1, s2};
    tips[0].x -= 2;
    tips[1].x += 2;
    tips[2].y -= 2;
    tips[3].y += 2;
    std::int32_t uLow = std::numeric_limits<std::int32_t>::max(), uHigh = std::numeric_limits<std::int32_t>::min();
    std::int32_t vLow = uLow, vHigh = uHigh, xLow = uLow, xHigh = uHigh, yLow = uLow, yHigh = uHigh;
    for (const auto& t : tips) {
        uLow = std::min(uLow, t.x + t.y), uHigh = std::max(uHigh, t.x + t.y);
        vLow = std::min(vLow, t.x - t.y), vHigh = std::max(vHigh, t.x - t.y);
        xLow = std::min(xLow, t.x), xHigh = std::max(xHigh, t.x);
        yLow = std::min(yLow, t.y), yHigh = std::max(yHigh, t.y);
    }
    bool ok = true;
    for (std::int32_t x = xLow; x <= xHigh; ++x) {
        for (std::int32_t y = yLow; y <= yHigh; ++y) {
            const std::int32_t u = x + y, v = x - y;
            const TerrainCorner c{x, y, a.level};
            if (u < uLow || u > uHigh || v < vLow || v > vHigh || std::any_of(tips.begin(), tips.end(), [&](const TerrainCorner& t) { return t == c; })) {
                continue;
            }
            ok = flattenCorner(c, h, style, refusals) && ok;
        }
    }
    return ok;
}

} // namespace ts2::content
