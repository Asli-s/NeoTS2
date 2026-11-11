#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::sim {

class CodeSource;
class World;
struct SimObject;

// Whether the player can select this Sim: bit 0 of Selection Flags (person data 0x9E), or for a
// Sim with a person type (person data 0x20) bit 3 too.
bool isSelectable(const SimObject& sim);

// The lot's selectable Sims in the order the game passes control in: humans before pets (person
// data 0xBA), then the larger life stage (person data 0x3A) x 1000 - days left (0x5B) + the stage's
// length (0x9C) first, then catalog names (line 0 of the catalog strings) A to Z, then the higher GUID.
std::vector<std::int16_t> selectionOrder(World& world, CodeSource& code);

// The game's check after every lot update: when no Sim is selected (global 3), or the selected one
// isn't selectable or has Selection Flags bit 1, control passes to the next Sim in selection order
// that lacks bit 1, from the start when there is no selected Sim. A Sim moved in fresh has never been
// selected, so this picks the first Sim of a new household.
void keepSelection(World& world, CodeSource& code, std::span<std::int16_t> globals);

} // namespace ts2::sim
