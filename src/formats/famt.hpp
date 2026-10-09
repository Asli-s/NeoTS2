#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ts2::famt {

// Family ties (FAMT, one per neighborhood): for each Sim (neighbor id), its ties to other
// neighbors.
enum class TieType : std::uint32_t { Father = 0, Mother = 1, Spouse = 2, Sibling = 3, Child = 4 };

struct Tie {
    std::uint32_t type = 0;
    std::int16_t other = 0;
};

struct SimTies {
    std::int16_t sim = 0;
    std::uint32_t value = 0;
    std::vector<Tie> ties;
};

struct FamilyTies {
    std::uint32_t version = 0;
    std::vector<SimTies> sims;
    std::size_t extraBytes = 0;  // left after the last Sim
};

FamilyTies parse(std::span<const std::uint8_t> data);

} // namespace ts2::famt
