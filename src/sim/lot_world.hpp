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
    // Placeholders and TemplatePersons marked for delete as the lot loaded (see World::keepUntilUnload).
    std::size_t keptUntilUnload = 0;
    std::vector<LotWorldProblem> problems;
};

// Recreates a loaded lot's objects in `world` under their saved ids, with their saved
// attributes, semi-attributes, temps, object data, motives, person data and script stack
// (each frame with its own stack object).
LotWorldReport buildWorld(const content::Lot& lot, CodeSource& code, World& world);

// Gives an object of `type` the state it was saved with, as the game's object reader loads it:
// attributes, semi-attributes, object data, temps, token, arrays, material names, slots, effects
// and a Sim's motives and person data. Not its place, stack or animations. Returns what didn't fit.
std::vector<std::string> applySavedState(SimObject& object, const xobj::SavedObject& state, CodeSource& code, const ObjectType& type);

// The lot's platform regions as World::platformRegion reads them, or none when the lot has no
// region grid. The lookup keeps its own copy of the grid.
World::PlatformRegion platformRegions(const content::Lot& lot);

// The neighborhood's neighbors and relationships, ready to share between worlds. As the game loads
// a neighborhood it drops every neighbor whose Sim isn't a person (OBJD type 2) or a family-tree-only
// Sim (type 0x11), its type unknown included, with its relationships both ways and its family ties.
// Wants saved by an older game take the numbers they lack from `wantDefs`, as the game's load does.
std::shared_ptr<const SavedNeighborhood> savedNeighborhood(const content::Neighborhood& hood, CodeSource& code,
                                                          std::map<std::int32_t, nhoodid::NeighborhoodId> all = {},
                                                          const content::WantDefinitions* wantDefs = nullptr);

} // namespace ts2::sim
