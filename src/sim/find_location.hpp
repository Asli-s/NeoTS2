#pragma once

#include "sim/sim_random.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <optional>

namespace ts2::sim {

// What Find Location For's search (modes 0 and 2) looks for.
struct LocationSearch {
    std::int32_t startX = 0;  // the reference object's position, 1/65536 tiles
    std::int32_t startY = 0;
    std::int32_t level = 1;
    std::int16_t referenceFacing = 0;
    std::uint8_t allowedFacings = 0xFF;  // bit r: the reference's facing turned by r, 0 keeps the object's own
    bool preferEmpty = true;             // skip tiles holding another object or a Sim
    bool sameRoom = true;                // only the start tile's room
    int startDirection = -1;             // 0-7: start in front of the reference this way, -1 the default walk
};

struct FoundLocation {
    std::int32_t x = 0;  // a tile's centre, 1/65536 tiles
    std::int32_t y = 0;
    std::optional<std::int16_t> facing;  // none: the object keeps its own
};

// The game's walk: ring by ring out from the start tile, the first tile passing the checks. With a
// start direction the walk goes round from that side, and moves to the tile in front of the start
// point when the start tile isn't that way. With allowed facings, every tile that passes the other
// checks draws a random number 0-7, where its facing tries begin.
// Not checked yet: blocked tiles, tiles the player can't edit, and whether the object fits there.
[[nodiscard]] std::optional<FoundLocation> findLocation(const World& world, const SimObject& placed,
                                                        const LocationSearch& search, SimRandom& rng);

} // namespace ts2::sim
