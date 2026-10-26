#pragma once

#include "scene/builder.hpp"
#include "scene/lot_parts.hpp"
#include "scene/neighborhood_scene.hpp"
#include "scene/scene.hpp"

#include <map>
#include <string>
#include <vector>

namespace ts2::scene {

// The ground around a lot in lot view, as the game's lot skirt: the neighborhood's terrain points
// `margin` tiles around the lot at the neighborhood's heights, less the lot's own tiles, showing the
// lot's terrain texture once per neighborhood tile, and the neighborhood's road tiles there with the
// lot view's road textures (`roadTextures`: texture by material "lotSkirtRoad_<terrain type>_<piece>").
// Points on the lot's boundary take the lot's own ground height there (grid level `groundLevel` of
// `lotHeights`), as the game's skirt does, so the two meet where a lot's edge differs from the
// neighborhood. Added in world coordinates.
void addLotSkirt(SceneBuilder& builder, const PaintedTerrain& terrain, const LotFootprint& lot, int margin,
                 const std::string& terrainType, const std::map<std::string, std::string>& roadTextures,
                 const Heights* lotHeights = nullptr, int groundLevel = 0);

// Sets the skirt's colour scales for day or night, as the game's lot skirt materials pick them
// (summer by day, as the engine knows no seasons or clouds yet). Returns the instances changed.
std::vector<std::size_t> lightLotSkirt(Scene& scene, bool day);

// The skirt's area: neighborhood tiles [x0, x1) x [y0, y1).
struct SkirtArea {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
SkirtArea lotSkirtArea(const PaintedTerrain& terrain, const LotFootprint& lot, int margin);

} // namespace ts2::scene
