#pragma once

#include "formats/objt.hpp"
#include "sim/code_source.hpp"

#include <cstdint>

namespace ts2::sim {

// What the game saves with a lot about an object type's code, to notice on load that it changed (a
// patch, an expansion or a mod): objects of a changed type are reset instead of resuming.
//
// The signature of tree `tree` as run by objects of `objectGroup`: the tree's version in the high
// 16 bits, plus the sum of the versions of the tree and every tree it can call, each counted once.
// Missing trees count nothing. 0 for tree 0 (none).
std::uint32_t treeSignature(CodeSource& code, std::uint32_t objectGroup, std::uint16_t tree);

// Whether an installed object type differs from what a lot saved about it (its entry in the lot's
// object type table): the signatures of its Init and Main trees (object functions 0 and 1) and its
// OBJD fields 0x3A and 0x3B. The game resets every object of a changed type when the lot loads.
bool typeChangedSinceSave(CodeSource& code, const ObjectType& type, const objt::Entry& saved);

} // namespace ts2::sim
