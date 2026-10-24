#pragma once

#include "sim/code_source.hpp"
#include "sim/world.hpp"

namespace ts2::sim {

// Resets an object as the game does when the object's type changed since the lot was saved (see
// typeChangedSinceSave): its object data is cleared except for what places it (direction, room,
// container and slot, its multi-tile lead...), some fields are set from its OBJD, and its script
// stack is replaced by its Main tree, which starts from the top on the next tick. Sims are not
// handled here: the game also clears their person data and refills it from elsewhere.
//
// For an object that isn't part of a multi-tile one, the game then runs its Init function and
// finishes with finishSingleTileReset.
void resetObject(SimObject& object, const ObjectType& type, CodeSource& code);
void finishSingleTileReset(SimObject& object, const ObjectType& type);

// Resets a Sim as the game does: its interaction queue is emptied, its animations stop, object data
// 0x22 and person data "Sitting?" and "Social Menu Obj ID" are cleared and its script stack is
// replaced by its Main tree. Its other data stays. The caller runs its Cleanup function next.
void resetSim(SimObject& sim, const ObjectType& type, CodeSource& code);

// The object's script stack is replaced by its Main tree, which starts from the top on the next tick.
void restartMain(SimObject& object, const ObjectType& type, CodeSource& code);

// Whether an object of this type is a part of a multi-tile object.
bool isMultiTilePart(const ObjectType& type);

} // namespace ts2::sim
