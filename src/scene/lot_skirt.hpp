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
// And of the lots' ponds and pools drawn over their imposters (see addNeighborhoodPonds).
inline constexpr const char* kNeighborhoodPondsName = "neighborhood ponds";

// Not in the original game: the neighborhood view's sea drawn as a pool-style sea, in place of its water
// sheet. Its surface over each wet tile, rippled and clear but for the reflection, and under it the
// ground hazed by depth and lit by the pools' caustics, as around a lot. `night` picks its tint. False
// when it couldn't be added, and the water sheet stays.
bool addNeighborhoodSea(SceneBuilder& builder, const PaintedTerrain& terrain, bool night);

// Not in the original game: with the pool-style sea and ponds, each lot's ponds and pools drawn live
// over its imposter in the neighborhood view, as the sea is, so they match it instead of the
// imposter's own picture of them. A pool's surface stays see-through, so its colour and tiles show.
// Each lot's water is an instance of its own, named kNeighborhoodPondsName, with its imposterLot set.
// With `highPictures` the imposters' High pictures already show the ponds drawn like pools, and the
// ponds' surface is see-through as a pool's is. `neighborhood` is the neighborhood's folder, whose
// lot files give the water.
void addNeighborhoodPonds(SceneBuilder& builder, const PaintedTerrain& terrain, const std::vector<LotFootprint>& lots,
                          const std::filesystem::path& neighborhood, bool night, bool highPictures);

// Not in the original game: bakes the sea's depth around a lot for lot view's pool-style sea haze into
// `terrain` (see PaintedTerrain::seaDepthMap), over the lot and 8 neighborhood tiles around it, a point a
// unit. Each point's depth is the water's averaged over 6 around, so the haze shades in gradually over
// folds and sudden drops. It counts half as deep over the lot, so the lot's own beach stays light where
// its ground drops away, and twice as deep from 6 out from the lot's edge, so a beach lot's shallow sea
// reads as blue as the open sea does from the neighborhood view's height. `lotHeights`, `groundLevel`
// and `smoothLotSkirts` are as for addLotSkirt.
void bakeSeaDepth(PaintedTerrain& terrain, const LotFootprint& lot, const Heights* lotHeights, int groundLevel, bool smoothLotSkirts);

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
