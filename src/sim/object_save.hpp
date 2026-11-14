#pragma once

#include "content/package_rewrite.hpp"
#include "formats/dbpf.hpp"
#include "formats/objt.hpp"
#include "formats/xobj.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ts2::sim {

// Saving a lot's objects (third stage of saving): objects moved, sold and bought in Buy and Build mode.
//
// An object is saved in several resources, each by its id: its state (XOBJ, or XMTO with a material
// block), its scene record (cObject: model, place, joints and, for animated objects, cAnimatable's
// part with its own copy of the place in the world), the object map's entry (its type's index in the
// object type table) and its entry in the map's wait table (ticks a Sleep still waits, and flags: bit
// 0 in use, bit 1 as object data 0x3C bit 0, bit 3 not known). The type table has an entry per type
// the lot uses (see typeTableEntry). A scene record's place is the object's (y, x, floor height under
// it plus any height of its own) turned to its facing; one held in a slot has none of its own.

// An object as it is now, to be saved.
struct ObjectNow {
    std::int16_t id = 0;
    std::uint32_t guid = 0;
    // Its state now. For an object saved before, only its place (header location and level), data and
    // slots are taken from it; the rest stays as saved.
    xobj::SavedObject state;
    std::int16_t waitTicks = 0;
};

// An object of a type as a saved lot has it, that a new object of the type starts from: its scene
// record, its state, its state's kind and its wait flags.
struct SceneTemplate {
    std::vector<std::uint8_t> bytes;
    xobj::SavedObject state;
    xobj::RecordKind kind = xobj::RecordKind::Object;
    std::uint16_t waitFlags = 1;
};

struct BoughtObject {
    ObjectNow now;  // its state complete, its tail too
    SceneTemplate scene;
    // The type whose code each of its stack frames runs (their codeOwner, an index into the type
    // table, is set from these).
    std::vector<std::uint32_t> frameOwners;
};

struct ObjectChanges {
    std::vector<ObjectNow> moved;   // objects the lot saved, now elsewhere
    std::vector<BoughtObject> bought;
    std::vector<std::int16_t> sold;  // objects the lot saved, now gone
    // The type table entry for a type the lot's table doesn't have (its index is given on saving).
    std::function<std::optional<objt::Entry>(std::uint32_t guid)> newType;
    // The floor's height under (x, y) on a storey (as object headers count them, 1 the ground).
    std::function<float(std::int32_t storey, float x, float y)> floorHeight;
};

struct ObjectSave {
    std::vector<content::ReplacedResource> resources;
    std::vector<dbpf::ResourceKey> removed;
    std::vector<std::string> problems;  // objects that couldn't be saved, and why: they are left as saved
    std::set<std::int16_t> failed;      // those objects
    std::size_t moved = 0, bought = 0, sold = 0;
};

// The changes to the lot package `original` (the lot as saved) that save `changes`.
ObjectSave objectChanges(dbpf::Package& original, const ObjectChanges& changes);

// An object of type `guid` from the lot packages `lotPackages` (searched in order): the first with a
// readable scene record that isn't a Sim's.
std::optional<SceneTemplate> findSceneTemplate(const std::vector<std::filesystem::path>& lotPackages, std::uint32_t guid);

// The scene rotation of an object facing `direction` (object data 0x01, 0-7), as x, y, z, w.
std::array<float, 4> facingRotation(std::int16_t direction);

} // namespace ts2::sim
