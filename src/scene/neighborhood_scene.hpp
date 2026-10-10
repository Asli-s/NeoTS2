#pragma once

#include "formats/nhood_paint_rules.hpp"
#include "formats/nhood_terrain.hpp"
#include "scene/neighborhood_lighting.hpp"
#include "scene/builder.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace ts2::scene {

struct NeighborhoodSceneStats {
    std::uint32_t width = 0, height = 0;  // tiles
    float seaLevel = 0;
    std::string terrainType;
    std::optional<nhood::SavedCamera> camera;  // where the view was when last saved
    std::size_t lots = 0, imposters = 0;      // lots, and those shown as their imposters
};

// Adds a neighborhood's terrain, painted by the game's rules (NeighborhoodTerrain.ini), and its
// water (from Neighborhoods/<hood>/<hood>_Neighborhood.package).
// `roadTextures` maps the game's road material names to their textures. Lots are shown as their
// imposters, lifted by `imposterZOffset` (Neighborhood.ini's ImposterZOffset).
NeighborhoodSceneStats addNeighborhood(SceneBuilder& builder, const std::filesystem::path& neighborhood,
                                       const nhood::PaintRules& rules, const NeighborhoodLight& light,
                                       const std::map<std::string, std::string>& roadTextures, float imposterZOffset);

} // namespace ts2::scene
