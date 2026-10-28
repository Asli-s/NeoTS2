#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <unordered_map>
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
};

// Reads the object list at the start of an ObjM resource and the wait table after it. Each wait
// entry's second value is not known yet.
ObjectMap parse(std::span<const std::uint8_t> data);

// ObjM instance 2: lists of object ids, each under a number. What the lists are is not known yet.
struct ObjectGroups {
    std::uint32_t version = 0;
    std::map<std::uint32_t, std::vector<std::uint16_t>> objectsByKey;
};

ObjectGroups parseGroups(std::span<const std::uint8_t> data);

} // namespace ts2::objm
