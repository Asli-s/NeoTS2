#pragma once

#include "content/wants.hpp"
#include "formats/swaf.hpp"
#include "formats/want_trees.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ts2::sim {

class World;
class Vm;
class CodeSource;

// How the game offers a Sim new wants and fears. A satisfied want goes into the Sim's history and
// queues the Sim. Each tick the wants manager takes a queued Sim, makes candidates from the want
// trees and the history, scores them and fills the Sim's empty slots from the best.

// The want trees' code for a Sim's life stage (person data 0x3A): Baby 13, Toddler 2, Child 1,
// Teen 3, Adult 0, Elder 4.
int wantAgeCode(std::int16_t lifeStage);

// What a want record's argument and value are compared with: the operators of the definitions.
bool wantCompare(content::WantOperator op, std::int32_t got, std::int32_t wanted);

// Whether a Sim's want (or fear) is the one a satisfied record names: the same Sim and GUID, then
// by the want's argument kind. 0 in the want accepts any argument. `categoryOf` gives an object
// type's want category (OBJD field 0x2F) by GUID.
bool wantMatches(const swaf::Want& want, const swaf::Want& request, content::WantOperator op,
                 const std::function<std::int32_t(std::uint32_t)>& categoryOf);

// What the want trees need from the rest of the simulation.
struct WantContext {
    World& world;
    CodeSource& code;
    Vm& vm;
    std::uint32_t installedPacks = 0;  // content::installedPacksMask
};

// Queues the Sim (a neighbor id) for new wants and fears. A Sim already queued keeps the lowest
// level asked for (see WantsState::queue).
void queueWantRefill(World& world, std::int16_t neighbor, int level);

// The wants manager's step each tick: its clock moves on 2 sim seconds, and one queued Sim gets
// its new wants and fears (the selected Sim first, then by neighbor id).
void stepWants(const WantContext& context);

// Forgets which tree node filled a place of the Sim's wants (or fears) once the place empties.
void forgetWantPlace(World& world, std::int16_t neighbor, std::size_t place, bool fear);

// Puts a satisfied want into the Sim's history at the history's current time.
void addWantHistory(World& world, std::int16_t neighbor, swaf::Want record);

// The Sim's history in use, made from the saved one the first time. Records whose want definition
// is unknown are dropped, as the game drops them when it loads them.
struct WantHistory;
WantHistory& wantHistory(World& world, std::int16_t neighbor);

} // namespace ts2::sim
