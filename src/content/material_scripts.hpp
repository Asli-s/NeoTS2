#pragma once

#include "content/install.hpp"

#include <map>
#include <string>
#include <vector>

namespace ts2::content {

// The neighborhood road materials the game's material scripts define (TSData/Res/Materials of
// every product, "create NHoodRoadMaterial(<material> <texture>)"): texture name by material name,
// later products overriding earlier ones.
std::map<std::string, std::string> neighborhoodRoadTextures(const std::vector<Product>& products);

// The same for the roads around a lot in lot view ("create LotSkirtRoadMaterialInstance(<material>
// <texture>)", with materials named "lotSkirtRoad_<terrain type>_<piece, 8 hex digits>").
std::map<std::string, std::string> lotSkirtRoadTextures(const std::vector<Product>& products);

} // namespace ts2::content
