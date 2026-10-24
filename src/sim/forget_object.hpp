#pragma once

#include "sim/world.hpp"

#include <cstdint>

namespace ts2::sim {

// A Sim forgets object `removed`, which is being deleted: its object data 0x5A and 0x5F stop naming
// it, and its own data 0x19 and 0x3E go to 0 (the game does this on every such pass). True when the
// Sim must be reset instead: it runs one of the object's interactions, or the object is its current
// or sub-queue interaction object (person data 0x50, 0x9D). Otherwise the queued interactions on the
// object, or showing it as their icon, are dropped.
// Not done yet: what the game does after clearing 0x5A or 0x5F, and the handlers that can claim
// the object first.
[[nodiscard]] bool forgetObject(SimObject& sim, std::int16_t removed);

} // namespace ts2::sim
