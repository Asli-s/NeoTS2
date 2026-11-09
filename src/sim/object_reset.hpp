#pragma once

#include "sim/code_source.hpp"
#include "sim/sim_random.hpp"
#include "sim/world.hpp"

namespace ts2::sim {

// Resets an object whose type changed since the lot was saved (see typeChangedSinceSave): object
// data is cleared except what places it (direction, room, container and slot, multi-tile lead...),
// some fields come from its OBJD, and its stack restarts at Main. Not for Sims, whose person data the
// game also refills. A single-tile object then runs Init and finishes with finishSingleTileReset.
void resetObject(SimObject& object, const ObjectType& type, CodeSource& code);
void finishSingleTileReset(SimObject& object, const ObjectType& type);

// Resets a Sim as the game does: it is placed again (see placeAfterReset), its interaction queue is
// emptied, its animations stop, object data 0x22 and person data "Sitting?" and "Social Menu Obj ID"
// are cleared and its script stack is replaced by its Main tree. Its other data stays. The caller
// runs its Cleanup function next.
void resetSim(World& world, SimObject& sim, const ObjectType& type, CodeSource& code, SimRandom& rng);

// Where a Sim reset leaves the Sim. One that isn't sitting or swimming (person data "Sitting?" and
// "Is Swimming?" both 0) and still fits where it is stays there, back in its container's slot when
// it was in one. Any other is taken out of its slot and moved, keeping its facing, to the first tile
// Find Location For's search finds around its place, in its room first and then anywhere. When
// there is none it stands at its place outside the slot. Sims out of the world stay as they are.
void placeAfterReset(World& world, SimObject& sim, SimRandom& rng);

// The object's script stack is replaced by its Main tree, which starts from the top on the next tick.
void restartMain(SimObject& object, const ObjectType& type, CodeSource& code);

// Takes whether the object's scripts run while the game is paused from its object data 0x3C, as the
// game does each time it loads or resets an object.
void notePausedRun(SimObject& object);

// Whether an object of this type is a part of a multi-tile object.
bool isMultiTilePart(const ObjectType& type);

} // namespace ts2::sim
