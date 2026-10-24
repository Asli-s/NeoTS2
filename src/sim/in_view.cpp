#include "sim/in_view.hpp"

#include "formats/objd.hpp"
#include "sim/idle_spot.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace ts2::sim {

namespace {

constexpr float kFixedPoint = 65536.0f;
constexpr std::int16_t kMaxRange = 16;
constexpr std::uint16_t kSkippedObjectType = 5;
constexpr std::size_t kFacingField = 1;
constexpr std::size_t kRoomField = 0x1D;

float tileUnits(std::int32_t fixed) { return static_cast<float>(fixed) / kFixedPoint; }
std::int32_t toFixed(float tiles) { return static_cast<std::int32_t>(tiles * kFixedPoint); }

// Turns the offset (a, b) by `facing` steps of -45 degrees, in 16.16 fixed point. The game works
// with its position's two halves the other way round from ours: a is our y and b our x.
std::pair<std::int32_t, std::int32_t> turned(int facing, float a, float b) {
    constexpr float r = 0.70710677f;
    switch (facing) {
    case 1:  return {toFixed((a + b) * r), toFixed((b - a) * r)};
    case 2:  return {toFixed(b), toFixed(-a)};
    case 3:  return {toFixed((b - a) * r), toFixed((-a - b) * r)};
    case 4:  return {toFixed(-a), toFixed(-b)};
    case 5:  return {toFixed((-a - b) * r), toFixed((a - b) * r)};
    case 6:  return {toFixed(-b), toFixed(a)};
    case 7:  return {toFixed((a - b) * r), toFixed((a + b) * r)};
    default: return {toFixed(a), toFixed(b)};
    }
}

// The tiles in view: rows straight ahead, each 2 tiles wider on both sides than the one before
// (the first row is the Sim's own tile and its two sides), cut to a circle. Facing a diagonal, the
// rows are longer and the offsets shrink by sqrt 2, so the wedge covers about the same ground.
std::set<Tile> tilesInView(const SimObject& sim, std::int16_t range) {
    std::set<Tile> tiles;
    // Facings outside 0-7 are read as 0.
    const int facing = sim.data[kFacingField] >= 0 && sim.data[kFacingField] < 8 ? sim.data[kFacingField] : 0;
    const bool diagonal = facing % 2 != 0;
    const float sqrt2 = std::sqrt(2.0f);
    const int rows = diagonal ? static_cast<int>(static_cast<float>(range) * sqrt2) + 1 : range;
    const int limit = diagonal ? 2 * (range + 1) * (range + 1) : (range + 1) * (range + 1);
    for (int e = 0; e < rows; ++e) {
        for (int j = std::max(-(2 * e + 1), -rows); j <= std::min(2 * e + 1, rows); ++j) {
            if (j * j + e * e > limit) {
                continue;
            }
            auto [a, b] = turned(facing, static_cast<float>(-e), static_cast<float>(j));
            if (diagonal) {
                const float shrink = 1.0f / sqrt2;
                a = static_cast<std::int32_t>(static_cast<float>(a) * shrink);
                b = static_cast<std::int32_t>(static_cast<float>(b) * shrink);
            }
            const std::int64_t x = (std::int64_t{b} + sim.x) >> 16;
            const std::int64_t y = (std::int64_t{a} + sim.y) >> 16;
            if (x >= 0 && x <= std::numeric_limits<std::uint8_t>::max() && y >= 0 &&
                y <= std::numeric_limits<std::uint8_t>::max()) {
                tiles.insert(Tile{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y),
                                  static_cast<std::uint8_t>(sim.level)});
            }
        }
    }
    return tiles;
}

} // namespace

std::vector<std::int16_t> objectsInView(const World& world, const SimObject& sim, std::int16_t range) {
    std::vector<std::int16_t> ids;
    range = std::min(range, kMaxRange);
    if (range <= 0) {
        return ids;
    }
    const auto tiles = tilesInView(sim, range);
    const float x = tileUnits(sim.x), y = tileUnits(sim.y);
    world.nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& s) {
        const bool seen = s.tile && tiles.contains(*s.tile) && s.id != sim.id &&
                          std::find(sim.slots.begin(), sim.slots.end(), s.id) == sim.slots.end() &&
                          s.definition.field(objd::field::kType) != kSkippedObjectType &&
                          s.data[kRoomField] == sim.data[kRoomField] &&
                          !wallTouches(world.sightWalls(), sim.level - 1, x, y, tileUnits(s.x), tileUnits(s.y));
        if (seen) {
            ids.push_back(s.id);
        }
        return false;
    });
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace ts2::sim
