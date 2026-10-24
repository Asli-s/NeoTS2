#include "sim/ground_click.hpp"

#include "sim/idle_spot.hpp"
#include "sim/snap.hpp"
#include "sim/vm.hpp"

#include <cmath>

namespace ts2::sim {

GroundPoint clearOfWalls(const WallTiles& walls, GroundPoint p) {
    constexpr float kClearance = 0.45f, kPast = 0.001f, kRoot2 = 1.41421356f;
    if (!(p.x >= 0 && p.y >= 0 && p.x < 256 && p.y < 256)) {
        return p;
    }
    // The walls are those of the clicked tile, even once the point has moved.
    const std::uint8_t sides = walls.at(p.level - 1, static_cast<int>(p.x), static_cast<int>(p.y));
    const auto low = [](float v) { return v - std::floor(v); };
    const auto high = [](float v) { return std::ceil(v) - v; };
    if ((sides & WallTiles::kLowY) && low(p.y) < kClearance) {
        p.y += kClearance - low(p.y) + kPast;
    }
    if ((sides & WallTiles::kLowX) && low(p.x) < kClearance) {
        p.x += kClearance - low(p.x) + kPast;
    }
    if ((sides & WallTiles::kHighY) && high(p.y) < kClearance) {
        p.y -= kClearance - high(p.y) + kPast;
    }
    if ((sides & WallTiles::kHighX) && high(p.x) < kClearance) {
        p.x -= kClearance - high(p.x) + kPast;
    }
    // Across the tile the game moves the point along the wall's direction, not away from it.
    if (sides & WallTiles::kDiagonalFalling) {
        const float d = (low(p.y) - high(p.x)) / kRoot2;
        if (d >= 0 && d < kClearance) {
            const float s = (kClearance - d + kPast) / kRoot2;
            p.x += s;
            p.y -= s;
        } else if (d < 0 && d > -kClearance) {
            const float s = (d + kClearance + kPast) / kRoot2;
            p.x -= s;
            p.y += s;
        }
    }
    if (sides & WallTiles::kDiagonalRising) {
        const float d = (low(p.y) - low(p.x)) / kRoot2;
        if (d >= 0 && d < kClearance) {
            const float s = (kClearance - d + kPast) / kRoot2;
            p.x += s;
            p.y += s;
        } else if (d < 0 && d > -kClearance) {
            const float s = (d + kClearance + kPast) / kRoot2;
            p.x -= s;
            p.y -= s;
        }
    }
    return p;
}

SimObject* groundDestination(Vm& vm, World& world, const SimObject* sim, GroundPoint point) {
    if (sim == nullptr || !(point.x >= 0 && point.y >= 0 && point.x < 256 && point.y < 256)) {
        return nullptr;
    }
    const GroundPoint at = clearOfWalls(world.walls(), point);
    SimObject* made = vm.create(destination::kGoHere);
    if (made == nullptr) {
        return nullptr;
    }
    SnapTarget target;
    target.x = static_cast<std::int32_t>(at.x * 65536.0f);
    target.y = static_cast<std::int32_t>(at.y * 65536.0f);
    target.level = at.level;
    snapTo(world, *made, target);
    return made;
}

} // namespace ts2::sim
