#pragma once

#include "content/lot.hpp"
#include "content/recolour_catalog.hpp"
#include "sim/code_source.hpp"
#include "sim/world.hpp"

#include <optional>
#include <span>
#include <string>

namespace ts2::sim {

// The row of an object's material names (string table 0x88) that a recolour material stands in
// for, as the game finds it when it applies a recolour. A row naming exactly `model` wins at once.
// Otherwise it is the last row naming the model without "_cres", "_<subset>_", and the state
// ending of `material` ("_dirty", "_clean", "_lit" or "_unlit", the first one it holds) when it
// has one. None when no row fits.
std::optional<std::size_t> recolourRow(std::span<const std::string> rows, const std::string& model, const std::string& subset,
                                       const std::string& material);

// As the game applies each object's saved chosen materials (the save's per-subset recolour list)
// when a lot loads: every material of the chosen recolour's family is put in the object's own
// material names at its row, so that Change Material shows the recolour in every state of the
// object (a lamp's lit and unlit shades). A recolour the catalog lacks changes nothing.
void applyChosenRecolours(const content::Lot& lot, CodeSource& code, World& world, const content::RecolourCatalog& catalog);

// Puts every material of the recolour family of (`model`, `subset`, `material`) in `object`'s own
// material names, as applyChosenRecolours does for a saved one. False when the catalog lacks it.
bool applyRecolour(CodeSource& code, SimObject& object, const content::RecolourCatalog& catalog, const std::string& model,
                   const std::string& subset, const std::string& material);

} // namespace ts2::sim
