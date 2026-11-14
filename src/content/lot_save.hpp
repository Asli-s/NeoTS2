#pragma once

#include "content/lot.hpp"
#include "content/package_rewrite.hpp"
#include "formats/dbpf.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ts2::content {

// Saving a lot, first stage: what Build mode changes without changing the lot's walls, doors or
// objects. Each part is written into the resources the lot was loaded from, every byte the edit
// doesn't touch kept as it was saved.
struct LotSaveParts {
    bool floors = false;         // the floor grid and floor pattern map
    bool wallCoverings = false;  // the wall pattern map and the wall layer's coverings (walls as saved)
    bool terrainPaint = false;   // the terrain textures and paint strength maps
    // The money spent (below 0) or got back on what is saved, added to the household's money as saved
    // in the simulator's budget globals.
    std::int32_t fundsChange = 0;
};

struct LotChanges {
    std::vector<ReplacedResource> resources;  // only those whose bytes differ from the saved ones
    std::vector<std::string> problems;        // parts that couldn't be saved, and why
    std::optional<std::int32_t> funds;        // the household's money as now saved, when it changed
};

// The resources of the lot package `original` (the lot as saved) that `edited` changes, for the
// parts `parts` asks for. `edited` is the lot as loadLot gave it, then edited in Build mode.
LotChanges lotChanges(dbpf::Package& original, const Lot& edited, const LotSaveParts& parts);

// The family records (FAMI) of the neighborhood package `hood` for the family living on lot
// `lotNumber`, with their money set to `funds`.
std::vector<ReplacedResource> familyFundsChanges(dbpf::Package& hood, std::int32_t lotNumber, std::int32_t funds);

} // namespace ts2::content
