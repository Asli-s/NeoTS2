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
#include <vector>

namespace ts2::scene {

// Where a lot lies in the neighborhood: the ground it covers (world units) at its elevation.
struct LotFootprint {
    std::uint32_t id = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, z = 0;
    float top = 0;  // the height of its imposter's highest point (its elevation without one)
    Transform place;  // where the lot itself goes in the neighborhood (from its own coordinates)
    std::string texture;  // its terrain texture (the ground around it in lot view)
};

struct NeighborhoodSceneStats {
    std::uint32_t width = 0, height = 0;  // tiles
    float seaLevel = 0;
    std::string terrainType;
    std::optional<nhood::SavedCamera> camera;  // where the view was when last saved
    std::size_t lots = 0, imposters = 0;      // lots, and those shown as their imposters
    std::size_t plants = 0, plantsShown = 0;  // plants, and those whose model was found
    std::vector<LotFootprint> lotFootprints;
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
