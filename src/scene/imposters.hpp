#pragma once

#include "formats/lot_description.hpp"
#include "scene/scene.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ts2::scene {

// Adds a lot's imposter (the "imposter" model and textures in its lot package) to the terrain's
// imposters, placed where the neighborhood has the lot and lifted by `zOffset` (the game's
// ImposterZOffset). Returns false when the lot has no imposter; problems go to `warnings`.
bool addImposter(PaintedTerrain& terrain, const std::filesystem::path& lotPackage, const nhood::LotDescription& lot,
                 float zOffset, std::vector<std::string>& warnings);

} // namespace ts2::scene
