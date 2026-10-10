#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "content/install.hpp"

namespace ts2::content {

// A wall style from walls.txt: the kind of partition a wall segment is (the wall layer's type).
struct WallStyle {
    bool thick = true;      // wallThickness standard (none: a flat plane, like a deck's skirt)
    bool rendered = true;   // notRendered: never drawn
    bool belowFloor = false;  // verticalSpan floorToFloorBasedCeiling: up to the floor's thickness above
    bool cutAway = true;      // mayCutAway: cut down by the cutaway and walls-down views
    std::string defaultPattern;
};

// The build catalog's text scripts (TSData/Res/Catalog/Scripts): which material each floor and
// wall pattern shows, and the wall styles. Keys are lowercase.
struct BuildScripts {
    std::map<std::string, std::string> floorPatterns;  // pattern name: material
    std::map<std::string, std::string> wallPatterns;
    std::map<std::uint32_t, WallStyle> wallStyles;

    // Adds one product's scripts; entries replace those of earlier products.
    void addPatterns(std::string_view text, bool walls);
    void addWallStyles(std::string_view text);
};

// The scripts of every product, in load order.
BuildScripts loadBuildScripts(const std::vector<Product>& products);

} // namespace ts2::content
