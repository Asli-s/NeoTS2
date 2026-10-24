#include "sim/route_destinations.hpp"

#include "sim/route_goals.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::sim {

namespace {

constexpr float kFixedPoint = 65536.0f;
constexpr float kPi = 3.14159265358979f;
constexpr float kSidestepReach = 2.0f;  // tiles: the farthest `other` may be for a sidestep

float tiles(std::int32_t fixed) { return static_cast<float>(fixed) / kFixedPoint; }

// One of the eight directions as a step of one tile in object coordinates (see eighthToward).
std::pair<float, float> eighthStep(int direction) {
    const float angle = static_cast<float>(direction & 7) * kPi / 4.0f;
    return {std::sin(angle), -std::cos(angle)};
}

} // namespace

std::optional<slot::Entry> makeWaySlot(int mode, const SimObject& mover, const SimObject& other) {
    slot::Entry entry;
    entry.type = slot::kRoutingSlot;
    entry.routingInts = {1, 0, 0};
    if (mode == 1) {
        // Directions 1, 2, 6 and 7, flags 13, 15, 18 and 20.
        entry.routingFlags = 0x0014A0C6;
        entry.minDistance = 1.0f;
        entry.maxDistance = 3.0f;
        entry.optimalDistance = 1.0f;
        entry.step = 0.25f;
        entry.facing = -3;
        entry.routingFloat = 0.625f;
        return entry;
    }
    if (mode != 0) {
        return std::nullopt;
    }
    const float ox = tiles(other.x), oy = tiles(other.y), mx = tiles(mover.x), my = tiles(mover.y);
    if (other.removed || other.x == kOffLot || other.y == kOffLot || !(std::hypot(ox - mx, oy - my) <= kSidestepReach)) {
        return std::nullopt;
    }
    // Points a tenth of a tile to either side of `other` (its heading turned by 2 and by 6): the
    // diagonal ahead on the side the mover is nearer is the one it steps to (a tie takes heading + 7).
    const int heading = other.data[1] & 7;
    const auto [rx, ry] = eighthStep(heading + 2);
    const auto [lx, ly] = eighthStep(heading + 6);
    const float toRight = std::hypot(ox + 0.1f * rx - mx, oy + 0.1f * ry - my);
    const float toLeft = std::hypot(ox + 0.1f * lx - mx, oy + 0.1f * ly - my);
    const int direction = (heading + (toRight < toLeft ? 1 : 7)) & 7;
    // That direction alone, flags 15 and 18.
    entry.routingFlags = 0x48000u | (1u << direction);
    entry.minDistance = 0.0625f;
    entry.maxDistance = 0.3f;
    entry.optimalDistance = 0.3f;
    entry.step = 0.05f;
    entry.routingFloat = 100.0f;
    // The eighth from the mover toward `other`, written as the game writes it.
    entry.facing = eighthToward(ox - mx, oy - my) & 7;
    return entry;
}

void cleanUpDestinations(World& world, SimObject& owner, bool force) {
    const auto used = [&](std::int16_t id) {
        const auto on = [id](const std::optional<QueuedInteraction>& q) { return q && q->object == id; };
        return on(owner.running) || on(owner.runningSub) ||
               std::ranges::any_of(owner.queue, [id](const QueuedInteraction& q) { return q.object == id; }) ||
               (owner.walking && (owner.walking->slotOwner == id || owner.walking->aside == id));
    };
    std::erase_if(owner.destinations, [&](std::int16_t id) {
        SimObject* made = world.find(id);
        if (made == nullptr || made->removed) {
            return true;
        }
        if (!force && used(id)) {
            return false;
        }
        world.remove(*made);
        return true;
    });
}

} // namespace ts2::sim
