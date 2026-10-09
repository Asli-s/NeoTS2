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
    // Values after the argument; the second one is the aspiration points (negative for fears).
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
    // Bytes after the history. The game never reads them; many saves have stale data there.
    std::size_t extraBytes = 0;
};

WantsAndFears parse(std::span<const std::uint8_t> data);

} // namespace ts2::swaf
