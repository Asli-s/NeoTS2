#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>

namespace ts2::objm {

struct ObjectMap {
    std::uint32_t unknown = 0;
    std::uint32_t version = 0;
    // Object id -> index of its entry in the lot's object type table (objt).
    std::unordered_map<std::uint32_t, std::uint32_t> typeIndexByObject;
};

// Reads the object list at the start of an ObjM resource; the data after it is not read yet.
ObjectMap parse(std::span<const std::uint8_t> data);

} // namespace ts2::objm
