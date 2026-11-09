#pragma once

#include "sim/world.hpp"

#include <cstdint>

namespace ts2::sim {

// A Sim forgets object `removed`, which is being deleted: object data 0x5A and 0x5F stop naming it,
// and data 0x19 and 0x3E always go to 0. Returns true when the Sim must be reset instead: it runs one
// of the object's interactions, or the object is in person data 0x50 or 0x9D. Otherwise queued
// interactions on the object or showing it as their icon are dropped.
// Not done yet: the game's steps after clearing 0x5A or 0x5F, and handlers that claim the object first.
[[nodiscard]] bool forgetObject(SimObject& sim, std::int16_t removed);

} // namespace ts2::sim
