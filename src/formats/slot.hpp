#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::slot {

// Slot definitions (SLOT) of an object type. Only each entry's type and bone are read so far;
// type 0 is a container slot.
struct Slots {
    std::uint32_t version = 0;
    std::vector<std::uint16_t> types;
    // Per entry: the entry of the type's "Model - Bones" string table naming where the slot sits in
    // its model, -1 for none.
    std::vector<std::int32_t> bones;

    std::size_t containerCount() const;
    // The bone of container slot `slot` (counted over container entries only); -1 for none.
    std::int32_t containerBone(std::size_t slot) const;
};

constexpr std::uint16_t kContainerSlot = 0;

Slots parse(std::span<const std::uint8_t> data);

} // namespace ts2::slot
