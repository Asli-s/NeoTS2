#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ts2::slot {

constexpr std::uint16_t kContainerSlot = 0;
constexpr std::uint16_t kRoutingSlot = 3;
constexpr std::uint16_t kTargetSlot = 4;

// One slot of an object type. Fields an entry's type does not use keep the defaults below. Entries
// of versions before 0x41 are not decoded yet: only their type is read.
struct Entry {
    std::uint16_t type = kContainerSlot;
    // Offset from the object in tiles, z being the height.
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    // Routing slots only.
    std::array<std::int32_t, 3> routingInts{};  // the planner uses each times 10, meaning unknown
    std::uint32_t routingFlags = 0;             // bits 0-7: the facings a Sim may end in
    std::int32_t routingInt = -1;               // -1 in every shipped entry, meaning unknown
    float minDistance = 1.0f;
    float maxDistance = 1.0f;
    float optimalDistance = 1.0f;
    float routingFloat = 0.1875f;  // meaning unknown
    std::int32_t facing = -2;      // 0-7 a direction, while -1, -2 (default) and -3 are special
    float step = 1.0f;

    // Container slots only, meanings unknown.
    std::int32_t containerValue = 0;  // 0, 10 or 100
    std::int32_t containerKind = 0;   // 3 to 13

    std::uint32_t objectFlags = 0;  // 0x20000 or 0
    // The entry of the type's "Model - Bones" string table naming where the slot sits in its model,
    // -1 for none.
    std::int32_t bone = -1;
    // -1 means the slot is numbered by its place in the list, counting from 1.
    std::int32_t id = -1;

    bool allowsFacing(int direction) const { return direction >= 0 && direction < 8 && ((routingFlags >> direction) & 1); }
};

// Slot definitions (SLOT) of an object type.
struct Slots {
    std::uint32_t version = 0;
    std::vector<Entry> entries;

    std::size_t containerCount() const;
    // The bone of container slot `slot` (counted over container entries only), or -1 for none.
    std::int32_t containerBone(std::size_t slot) const;
};

Slots parse(std::span<const std::uint8_t> data);

} // namespace ts2::slot
