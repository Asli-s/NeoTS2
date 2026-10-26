#pragma once

#include "formats/slot.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <optional>

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
    NotSameRoom,   // or the spot is not on the lot
    WallInTheWay,  // a wall crosses the straight line there
};
// The checks Snap makes before moving `object` to `target`. The game also checks walls close to the
// spot and other objects and Sims in the way, which isn't done yet.
SnapRefusal snapRefusal(const World& world, const SimObject& object, const SnapTarget& target);

// Moves `object` to `target`. Its footprint is marked as changed.
void snapTo(World& world, SimObject& object, const SnapTarget& target);

} // namespace ts2::sim
