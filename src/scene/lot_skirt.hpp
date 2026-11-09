#pragma once

#include "content/lot.hpp"
#include "scene/builder.hpp"
#include "scene/lot_parts.hpp"
#include "scene/neighborhood_scene.hpp"
#include "scene/scene.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ts2::scene {

// A lot's own ground look, for a skirt that blends it outward (smooth lot skirts, not in the
// original game): its canvas texture, and each paint's texture with its strength map (0 to 255 at
// four points a tile, as the lot keeps it).
struct LotGroundLook {
    std::string canvas;
    std::vector<std::pair<std::string, ts2::terrain::Array2D>> paints;
};
LotGroundLook lotGroundLook(const content::Lot& lot);

// The ground around a lot in lot view, as the game's lot skirt: the neighborhood's terrain `margin` tiles
// around the lot, less its own tiles, with the lot's terrain texture once per tile and road tiles in the
// lot view's road textures (`roadTextures`, by material "lotSkirtRoad_<terrain type>_<piece>"). Boundary
// points take the lot's ground height (level `groundLevel` of `lotHeights`) as the game's do, so the two
// meet. Added in world coordinates with the sea (`bounds`: the lot's own, for its beach). With smooth lot
// skirts, `look` is blended onto the skirt beside the lot.
void addLotSkirt(SceneBuilder& builder, const PaintedTerrain& terrain, const LotFootprint& lot, int margin,
                 const std::string& terrainType, const std::map<std::string, std::string>& roadTextures,
                 const Heights* lotHeights = nullptr, int groundLevel = 0, const content::WorldBounds* bounds = nullptr,
                 const LotGroundLook* look = nullptr);

// The names of the neighborhood view's pool-style sea and its depth haze (see addNeighborhoodSea).
inline constexpr const char* kNeighborhoodSeaName = "neighborhood sea";
inline constexpr const char* kNeighborhoodSeaDepthName = "neighborhood sea depth";

// Not in the original game: the neighborhood view's sea drawn as a pool-style sea, in place of its water
// sheet. Its surface over each wet tile, rippled and clear but for the reflection, and under it the
// ground hazed by depth and lit by the pools' caustics, as around a lot. `night` picks its tint. False
// when it couldn't be added, and the water sheet stays.
bool addNeighborhoodSea(SceneBuilder& builder, const PaintedTerrain& terrain, bool night);

// Sets the skirt's colour scales for day or night, as the game's lot skirt materials pick them
// (summer by day, as the engine knows no seasons or clouds yet), and its sea's tint. Returns the
// instances changed.
std::vector<std::size_t> lightLotSkirt(Scene& scene, bool day);

// The skirt's area: neighborhood tiles [x0, x1) x [y0, y1).
struct SkirtArea {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
SkirtArea lotSkirtArea(const PaintedTerrain& terrain, const LotFootprint& lot, int margin);

} // namespace ts2::scene
