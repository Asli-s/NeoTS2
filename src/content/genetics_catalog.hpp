#pragma once

#include "content/install.hpp"

#include <string>
#include <vector>

namespace ts2::content {

// A colour a baby can inherit: its catalog GUID (the "family" of its property set, as genes name it,
// lowercase) and its "genetic" value. A lower value is dominant, and 0 marks a colour that isn't
// blended (alien skin, grey hair).
struct GeneticColour {
    std::string guid;
    float genetic = 0;
};

// The colours babies inherit, from the skin catalogs (Res/Catalog/Skins) of every product: skin
// tones, hair tones and eye colours (texture overlays of subtype 3). Each GUID is listed once, in
// the order the packages are read.
struct GeneticsCatalog {
    std::vector<GeneticColour> skins, hairs, eyes;
};

GeneticsCatalog loadGeneticsCatalog(const std::vector<Product>& products);

// The colour with this GUID (any case), or null.
const GeneticColour* findColour(const std::vector<GeneticColour>& colours, const std::string& guid);

} // namespace ts2::content
