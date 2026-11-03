#pragma once

#include "formats/swaf.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace ts2::sim {

// What a Sim's wants have done: every want satisfied for it, by want GUID, as the neighborhood
// saves it. A record's tail holds its time, the aspiration points and the influence it gave.
// The want trees offer a node's want when its parent's want is in here.
struct WantHistory {
    // The history's own clock, in sim seconds. It moves with the wants clock while the Sim's
    // history is in use and is saved with it.
    std::int32_t time = 0;
    // The wants clock when `time` last moved.
    std::int32_t lastClock = 0;
    std::map<std::uint32_t, std::vector<swaf::Want>> records;
};

// A record's time (sim seconds on its history's clock), 0 when it has none.
inline std::int32_t recordTime(const swaf::Want& record) { return record.tail.empty() ? 0 : record.tail[0]; }

// The wants manager's state on a lot.
struct WantsState {
    // Sim seconds: 2 more every tick the game runs.
    std::int32_t clock = 0;
    // The Sims (neighbor ids) whose wants and fears are to be offered again, with what goes. 0 and
    // 1 drop every want and fear not held, 2 every fear not held and the wants no longer valid, 3
    // only those no longer valid.
    std::map<std::int16_t, int> queue;
    // Each Sim's history in use on this lot, by neighbor id.
    std::map<std::int16_t, WantHistory> histories;
    // The highest OBJD field 0x12 among the catalog objects of each want category (field 0x2F),
    // read when a category want is first checked.
    std::optional<std::map<std::uint16_t, std::int16_t>> categoryValues;
    // The tree id and node depth each Sim's offered wants came from, by place (fears from 1000).
    std::map<std::int16_t, std::map<std::size_t, std::pair<std::uint32_t, std::int32_t>>> slotNodes;
    // How many times each Sim's want (or fear, from 1000) places were satisfied on this lot. The wants
    // panel plays its achieve animation when a place it shows gets a new want after one.
    std::map<std::int16_t, std::map<std::size_t, std::uint32_t>> satisfied;
};

} // namespace ts2::sim
