#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ts2::objm {

struct ObjectMap {
    std::uint32_t unknown = 0;
    std::uint32_t version = 0;
    // Object id -> index of its entry in the lot's object type table (objt).
    std::unordered_map<std::uint32_t, std::uint32_t> typeIndexByObject;
    // The ticks each object id still waits before its thread runs again (index id - 1), as a Sleep
    // left it when the lot was saved. Negative or 0 when it doesn't wait. Empty when the map has none.
    std::vector<std::int16_t> waitTicks;
    // As saved, for writing it again: the objects in order (id, type index), each wait entry's second
    // value, and whether the map had a wait table.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order;
    std::vector<std::uint16_t> waitSecond;
    bool hasWaits = false;
    std::vector<std::uint8_t> rest;  // what follows the wait table, not read yet
};

// Reads the object list at the start of an ObjM resource and the wait table after it. Each wait
// entry's second value is not known yet.
ObjectMap parse(std::span<const std::uint8_t> data);
// The map saved as parse reads it (its `order`, `waitTicks` and `waitSecond`), after `name` (the
// 64-byte name its resource starts with).
std::vector<std::uint8_t> write(const ObjectMap& map, std::span<const std::uint8_t> name);

// ObjM instance 2: lists of object ids, each under a number. What the lists are is not known yet.
struct ObjectGroups {
    std::uint32_t version = 0;
    std::map<std::uint32_t, std::vector<std::uint16_t>> objectsByKey;
};

ObjectGroups parseGroups(std::span<const std::uint8_t> data);
// The lists saved again (version 2), after `name`.
std::vector<std::uint8_t> writeGroups(const ObjectGroups& groups, std::span<const std::uint8_t> name);

} // namespace ts2::objm
