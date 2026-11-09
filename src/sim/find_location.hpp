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
    // 0-7: in place of the rings, walk the line through the start tile this way, both ways in turn
    // (1 tile this way, 1 back, 2 this way, 2 back, ...). -1 for the rings.
    int lineDirection = -1;
};

struct FoundLocation {
    std::int32_t x = 0;  // a tile's centre, 1/65536 tiles
    std::int32_t y = 0;
    std::optional<std::int16_t> facing;  // none: the object keeps its own
};

// The first tile passing the checks, ring by ring out from the start tile. A start direction starts
// the rings on that side (from the tile in front when the start tile isn't that way), a line direction
// walks that line instead. With allowed facings, each passing tile draws 0-7 for its first facing try,
// and each try runs the placement test (see placementCode), which tests a Sim as a walking Sim.
// Not checked yet: blocked tiles and tiles the player can't edit.
[[nodiscard]] std::optional<FoundLocation> findLocation(const World& world, const SimObject& placed,
                                                        const LocationSearch& search, SimRandom& rng);

} // namespace ts2::sim
