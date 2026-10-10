#pragma once

#include "formats/nhood_paint_rules.hpp"
#include "formats/nhood_terrain.hpp"
#include "scene/neighborhood_lighting.hpp"
#include "scene/builder.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace ts2::scene {

struct NeighborhoodSceneStats {
    std::uint32_t width = 0, height = 0;  // tiles
    float seaLevel = 0;
    std::string terrainType;
    std::optional<nhood::SavedCamera> camera;  // where the view was when last saved
};

// Adds a neighborhood's terrain, painted by the game's rules (NeighborhoodTerrain.ini), and its
// water (from Neighborhoods/<hood>/<hood>_Neighborhood.package).
NeighborhoodSceneStats addNeighborhood(SceneBuilder& builder, const std::filesystem::path& neighborhood,
                                       const nhood::PaintRules& rules, const NeighborhoodLight& light);

} // namespace ts2::scene
