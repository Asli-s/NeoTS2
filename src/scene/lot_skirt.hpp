#pragma once

#include "scene/builder.hpp"
#include "scene/neighborhood_scene.hpp"
#include "scene/scene.hpp"

#include <map>
#include <string>

namespace ts2::scene {

// The ground around a lot in lot view, as the game's lot skirt: the neighborhood's terrain points
// `margin` tiles around the lot at the neighborhood's heights, less the lot's own tiles, showing the
// lot's terrain texture once per neighborhood tile; and the neighborhood's road tiles there with the
// lot view's road textures (`roadTextures`: texture by material "lotSkirtRoad_<terrain type>_<piece>").
// Added in world coordinates.
void addLotSkirt(SceneBuilder& builder, const PaintedTerrain& terrain, const LotFootprint& lot, int margin,
                 const std::string& terrainType, const std::map<std::string, std::string>& roadTextures);

// The skirt's area: neighborhood tiles [x0, x1) x [y0, y1).
struct SkirtArea {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
SkirtArea lotSkirtArea(const PaintedTerrain& terrain, const LotFootprint& lot, int margin);

} // namespace ts2::scene
