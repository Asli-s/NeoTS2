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
    bool toRoof = false;      // verticalSpan floorToRoof: up to the roof (the attic walls under gable ends)
    // verticalSpan floorToFloor, the default. A stage wall (stageWall) is a span of its own.
    bool floorToFloor = true;
    bool cutAway = true;      // mayCutAway: cut down by the cutaway and walls-down views
    bool blocksLight = true;  // blocksLight: stops lamp and window light spreading past it
    std::string defaultPattern;
};

// A roof pattern from roofPatterns.txt: the textures of its top, underside, edge strip and trim strip.
struct RoofPatternTextures {
    std::string top, under, edges, trim;
};

// The build catalog's text scripts (TSData/Res/Catalog/Scripts): which material each floor and
// wall pattern shows, the wall styles and the roof patterns. Pattern names are lowercase.
struct BuildScripts {
    std::map<std::string, std::string> floorPatterns;  // pattern name: material
    std::map<std::string, std::string> wallPatterns;
    std::map<std::uint32_t, WallStyle> wallStyles;
    // Roof patterns by the id lots save. The base game's script holds pattern 0, the default manor
    // roof, which has no catalog entry.
    std::map<std::uint32_t, RoofPatternTextures> roofPatterns;

    // Whether a wall segment of this wall layer type is a full room wall: a drawn wall style spanning
    // floor to floor. Fences (types that are GUIDs) and unknown styles are not.
    bool isRoomWall(std::uint32_t type) const {
        const auto it = wallStyles.find(type);
        return it != wallStyles.end() && it->second.rendered && it->second.floorToFloor;
    }

    // Adds one product's scripts. Its entries replace those of earlier products.
    void addPatterns(std::string_view text, bool walls);
    void addWallStyles(std::string_view text);
    void addRoofPatterns(std::string_view text);
};

// The scripts of every product, in load order.
BuildScripts loadBuildScripts(const std::vector<Product>& products);

} // namespace ts2::content
