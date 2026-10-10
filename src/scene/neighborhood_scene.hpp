#pragma once

#include "content/nhood_catalog.hpp"
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
    std::size_t plants = 0, plantsShown = 0;  // plants, and those whose model was found
};

// Adds a neighborhood's terrain, painted by the game's rules (NeighborhoodTerrain.ini), and its
// water (from Neighborhoods/<hood>/<hood>_Neighborhood.package).
// `roadTextures` maps the game's road material names to their textures. Lots are shown as their
// imposters, lifted by `imposterZOffset` (Neighborhood.ini's ImposterZOffset); plants as the models
// `objectTypes` (the neighborhood object catalog) names.
NeighborhoodSceneStats addNeighborhood(SceneBuilder& builder, const std::filesystem::path& neighborhood,
                                       const nhood::PaintRules& rules, const NeighborhoodLight& light,
                                       const std::map<std::string, std::string>& roadTextures, float imposterZOffset,
                                       const std::map<std::uint32_t, content::NeighborhoodObjectType>& objectTypes);

} // namespace ts2::scene
