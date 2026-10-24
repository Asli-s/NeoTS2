#pragma once

#include "formats/slot.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ts2::sim {

struct SimObject;
class World;

// When a Sim has to make way for another (it is asked to move, or it steps aside itself), the router
// makes a "Destination - For Go Here & Shoo" object where the moving Sim stands and gives it a
// routing slot of its own in place of the type's slot 0. The moving Sim then walks to that slot.

// The routing slot for `mover` making way for `other`:
//   mode 1  1 to 3 tiles away (best 1), in the directions 1, 2, 6 and 7 of the destination's facing,
//           any facing at the end.
//   mode 0  a sidestep of 0.0625 to 0.3 tiles, on the forward diagonal of `other` on the side the
//           mover already stands on, ending facing toward `other`. None when `other` is off the
//           lot or farther than 2 tiles.
// Other modes give none.
std::optional<slot::Entry> makeWaySlot(int mode, const SimObject& mover, const SimObject& other);

// The most destinations one Sim keeps at once. Another is refused while it has this many.
inline constexpr std::size_t kMaxDestinations = 16;

// Removes the destinations `owner` made that nothing uses any more: none of its queued or running
// interactions is on one and its route doesn't lead to one. With `force` (the Sim is reset or
// removed), all of them. Destinations already gone are dropped from the list.
void cleanUpDestinations(World& world, SimObject& owner, bool force);

} // namespace ts2::sim
