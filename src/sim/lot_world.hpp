#pragma once

#include "content/lot.hpp"
#include "content/neighborhood.hpp"
#include "sim/code_source.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts2::sim {

struct LotWorldProblem {
    std::uint32_t id = 0;
    std::string message;
};

struct LotWorldReport {
    std::size_t objects = 0;
    std::size_t threads = 0;
    // Objects and Sims whose type changed since the lot was saved, by id: the game resets them
    // when the lot loads instead of letting them resume (see LotSimulation).
    std::vector<std::int16_t> changedType;
    // Data table records of objects the lot no longer has (the game leaves them in the package).
    std::size_t staleDataTables = 0;
    std::vector<LotWorldProblem> problems;
};

// Recreates a loaded lot's objects in `world` under their saved ids, with their saved
// attributes, semi-attributes, temps, object data, motives, person data and script stack
// (each frame with its own stack object).
LotWorldReport buildWorld(const content::Lot& lot, CodeSource& code, World& world);

// The neighborhood's neighbors and relationships, ready to share between worlds.
std::shared_ptr<const SavedNeighborhood> savedNeighborhood(const content::Neighborhood& hood,
                                                          std::map<std::int32_t, nhoodid::NeighborhoodId> all = {});

} // namespace ts2::sim
