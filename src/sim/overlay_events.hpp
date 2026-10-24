#pragma once

#include "sim/vm.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <functional>

namespace ts2::sim {

// Tells the event trees of an object's overlays how far each has played, as the game does: when one
// starts blending out (kOverlayBlendMs before its end) and when it is gone, each event run on the
// object with (event, the overlay's stack object, percent played, 1, 0). Finished overlays are
// dropped. `onStop` hears of event trees that end other than true or false. `nowMs` is the time on the
// animation clock (the game tells events as animations play, between simulation ticks).
void tellOverlayEvents(World& world, Vm& vm, std::int16_t object, const std::function<void(const RunResult&)>& onStop, double nowMs);
inline void tellOverlayEvents(World& world, Vm& vm, std::int16_t object, const std::function<void(const RunResult&)>& onStop) {
    tellOverlayEvents(world, vm, object, onStop, world.clockMs());
}

// Tells the event tree of the object's Animate Sim each SimAntics event ("event=N") its clip has
// played past, in order, as the game does: run on the object with (N, the stack object the Animate
// Sim had, percent played, 1, 0), its temps put back after. The Animate Sim keeps waiting for its
// clip. `onStop` and `nowMs` as for tellOverlayEvents.
void tellAnimateSimEvents(World& world, Vm& vm, std::int16_t object, const std::function<void(const RunResult&)>& onStop, double nowMs);

} // namespace ts2::sim
