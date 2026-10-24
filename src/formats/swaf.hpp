#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace ts2::swaf {

// A Sim's current wants and fears and its want history, saved in the neighborhood package
// (one resource per Sim, instance = neighbor id).

struct Want {
    std::uint32_t version = 0;
    std::uint16_t id = 0;       // neighbor id of the Sim it belongs to
    std::uint32_t guid = 0;     // which want
    std::uint8_t type = 0;      // what kind of argument it has (0 = none)
    std::int32_t argument = 0;
    std::int16_t value = 0;
    // Values after the argument. The second one is the aspiration points (negative for fears).
    std::vector<std::int32_t> tail;
    std::uint8_t flags = 0;
};

struct WantsAndFears {
    std::uint32_t version = 0;
    std::optional<Want> extra;
    std::uint32_t maxWants = 0;
    std::vector<Want> wants;
    std::uint32_t maxFears = 0;
    std::vector<Want> fears;
    std::uint32_t lastValue = 0;
    std::uint32_t historyVersion = 0;
    std::int32_t historyValue = 0;
    std::map<std::uint32_t, std::vector<Want>> history;  // past wants, by want GUID
    // Bytes after the history. The game never reads them, and many saves have stale data there.
    std::size_t extraBytes = 0;
};

WantsAndFears parse(std::span<const std::uint8_t> data);

// A Sim's want and fear slots and how many wants it may hold, as the game reads them: the saved
// slot counts (4 and 3 before version 2) and one hold more than the saved extra holds (none
// before version 4). Saved wants past the slots are dropped.
struct Slots {
    std::uint32_t wants = 4, fears = 3;
    std::uint32_t holds = 1;
};
Slots slots(const WantsAndFears& saved);

} // namespace ts2::swaf
