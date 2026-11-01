#pragma once

#include "formats/slot.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace ts2::sim {

// Where Snap puts an object, and which of its checks apply.
struct SnapTarget {
    std::int32_t x = 0;  // 1/65536 tiles, as objects keep their position
    std::int32_t y = 0;
    std::int32_t level = 1;
    std::optional<std::int16_t> facing;  // none: the object keeps its own
    bool checkRoom = true;   // the same-room check (a slot skips it with flag bit 11)
    bool checkWalls = true;  // the wall check along the way (a slot skips it with flag bit 19)
};

// Whether the slot sits at a bone of its object's model (flag word 0x20000) instead of its offset.
bool isBoneSlot(const slot::Entry& slot);

// The spot routing slot `slot` of `owner` stands for: the slot's offset, or for a bone slot the
// bone's place in the model, turned with the owner's facing, on the owner's level, facing the slot's
// (or bone's) way when its flag bit 12 says so. None for a bone slot without `bone` and for spots far
// beyond any lot.
[[nodiscard]] std::optional<SnapTarget> routingSlotTarget(const SimObject& owner, const slot::Entry& slot, const std::optional<ModelBone>& bone = std::nullopt);

// The facing a bone slot's bone gives a Sim on it: the owner's facing turned by the bone's turn, in
// steps of 45 degrees. Snap uses it with the slot's flag bit 12. A route to a bone slot always ends
// in it.
[[nodiscard]] std::int16_t boneSlotFacing(const SimObject& owner, const ModelBone& bone);

enum class SnapRefusal {
    None,
    OffLot,        // the room check is on and the spot is off the lot: placement code 2
    NotSameRoom,
    WallInTheWay,  // a wall crosses the straight line there
};
// The room and wall checks Snap makes before moving `object` to `target` (see placementCode for the
// placement test). The game also checks walls close to the spot, which isn't done yet.
SnapRefusal snapRefusal(const World& world, const SimObject& object, const SnapTarget& target);

// Moves `object` to `target`. Its footprint is marked as changed. What sits in its slots and the
// other parts of a multi-tile object move with it (see moveContents and moveParts).
void snapTo(World& world, SimObject& object, const SnapTarget& target);

// Puts what sits in `container`'s slots, and what sits in theirs, at the container's place and
// level, as the game keeps a passenger at its car's place while the car drives.
void moveContents(World& world, const SimObject& container, int depth = 0);

// Where one part of a multi-tile object goes.
struct PartSpot {
    std::int16_t id = 0;
    std::int32_t x = kOffLot;  // kOffLot when the part would land off the lot
    std::int32_t y = kOffLot;
    std::int32_t level = 1;
    std::int16_t facing = 0;
};
// Where the other parts of `moved`'s multi-tile object go when `moved` stands at `at`, as the game
// moves a multi-tile object as one: each at its offset from `moved`, turned with the facing, which
// they all take. A part's offset is its sub-index x and y (OBJD field 0x0B) and its storey (field
// 0x38). Empty for an object of one tile. Diagonal facings are not known yet and give none.
[[nodiscard]] std::vector<PartSpot> partSpots(const World& world, const SimObject& moved, const SnapTarget& at);

// Puts the other parts of `moved`'s multi-tile object where they belong around it (see partSpots).
// A part landing off the lot goes out of the world.
void moveParts(World& world, const SimObject& moved);

// The game's placement code when Snap's placement test refuses `object` at `target`, none when it
// passes: every part of a multi-tile object is tested at its own spot, in turn. 2: the spot is off
// the lot. 0x0C: another object or a Sim is in the way (Snap tests Sims by other rules, which aren't
// run yet, so a Sim is never in the way of another Sim here).
[[nodiscard]] std::optional<std::int16_t> placementCode(const World& world, const SimObject& object, const SnapTarget& target);

} // namespace ts2::sim
