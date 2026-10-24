#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace ts2::nhoodid {

constexpr std::uint32_t kType = 0xAC8A7A2E;

// A neighborhood's identity, one record in each neighborhood package (main neighborhoods and their
// sub-neighborhoods alike). Scripts reach these through NeighborhoodManager.
struct NeighborhoodId {
    std::uint32_t version = 0;
    std::string prefix;     // the main neighborhood's folder name, such as "N002"
    std::int32_t id = 0;    // unique across the Neighborhoods folder, 0 is the tutorial
    // 1 primary, 2 university, 3 downtown, 4 suburb, 5 asia, 6 mountains, 7 tropics.
    std::int32_t type = 0;
    std::string subPrefix;  // a sub-neighborhood's own prefix, such as "U001", empty for a main one
    std::int32_t value1 = 0;  // meaning not known yet (1 for a downtown saved before version 6)
    std::uint32_t requiredProducts = 0;  // the game refuses the neighborhood with fewer products installed
    std::int32_t value2 = 0;  // meaning not known yet (by type before version 8)
    std::int32_t flags = 0;   // meaning not known yet
    std::array<std::int32_t, 4> seasons{1, 2, 3, 0};  // the season of each quadrant
};

// Reads the record as the game does, filling what older versions lack from the type.
NeighborhoodId parse(std::span<const std::uint8_t> data);

} // namespace ts2::nhoodid
