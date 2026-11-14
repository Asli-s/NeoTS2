#pragma once

#include "formats/objt.hpp"
#include "sim/code_source.hpp"

#include <cstdint>

namespace ts2::sim {

// A lot saves these to notice on load that an object type's code changed (a patch, expansion or mod).
// The signature of `tree` run by `objectGroup`: its version in the high 16 bits, plus the versions of
// it and every tree it can call, each once. Missing trees count nothing, and tree 0 gives 0.
std::uint32_t treeSignature(CodeSource& code, std::uint32_t objectGroup, std::uint16_t tree);

// Whether an installed object type differs from what a lot saved about it (its entry in the lot's
// object type table): the signatures of its Init and Main trees (object functions 0 and 1) and its
// OBJD fields 0x3A and 0x3B. The game resets every object of a changed type when the lot loads.
bool typeChangedSinceSave(CodeSource& code, const ObjectType& type, const objt::Entry& saved);

// The entry a lot's object type table has for a type, as the game writes it: its GUID (twice), the
// signatures of its Init and Main trees and its OBJD fields 0x3A and 0x3B (the five words
// typeChangedSinceSave compares, the fifth its objects' semi-attribute count), kind 4, and its catalog
// name, or flag 1 and no name when its objects are saved as XMTO (with a material block), as the
// game saves multi-tile parts and some others. Its index is the table's to give.
objt::Entry typeTableEntry(CodeSource& code, const ObjectType& type, std::size_t semiAttributes, bool savedWithMaterial, const std::string& name);

} // namespace ts2::sim
