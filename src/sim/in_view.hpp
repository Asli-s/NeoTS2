#pragma once

#include <cstdint>
#include <vector>

namespace ts2::sim {

class World;
struct SimObject;

// The objects Set to Next 0x18 walks for a Sim: those on the tiles in a wedge ahead of it (its
// facing, object data 1), up to `range` tiles away (at most 16), in the Sim's room, not the Sim
// itself or something it holds, not of object type 5 and with no wall between them and the Sim.
// The ids come sorted, each once.
std::vector<std::int16_t> objectsInView(const World& world, const SimObject& sim, std::int16_t range);

} // namespace ts2::sim
