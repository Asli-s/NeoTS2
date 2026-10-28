#pragma once

#include "sim/vm.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <functional>
#include <span>

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
// Sim had, percent played, 1, 0), its temps put back after. A clip still fading out after its
// Animate Sim went on (SimObject::ending) tells its last events too. `onStop` and `nowMs` as for
// tellOverlayEvents.
void tellAnimateSimEvents(World& world, Vm& vm, std::int16_t object, const std::function<void(const RunResult&)>& onStop, double nowMs);

// Runs the look_state trees of a Sim's looks for each state they took, in order, as the game does:
// run on the Sim with (state, the stack object the Look At had, percent through, hold left, 1 for
// 9904 and 9906 else 0), its temps put back after, and once more with 9999 in place of the state
// for 9905 and 9906. Looks the Sim no longer follows drop out of SimObject::lookTargets.
void tellLookStates(World& world, Vm& vm, std::int16_t id, std::span<const LookController::Change> changes,
                    const std::function<void(const RunResult&)>& onStop);

// Puts a Sim's look into person data 0x29 to 0x2C, as the game does each frame: what the look it
// follows looks at, its state and its hold left, or no look (0, 0, 9900, 0).
void noteLook(SimObject& sim);

} // namespace ts2::sim
