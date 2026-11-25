#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
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
    bool bearsLoad = true;    // bearsLoad: holds up floors and objects on the storey above
    bool blocksPlacement = true;  // blocksPlacement: divides rooms
    // requiresFlatBottom and requiresFlatTop: the ground under it, and the corners a storey up, are
    // levelled as it is built and stay so while it stands. Off unless the script says so.
    bool flatBottom = false, flatTop = false;
    // verticalSpan, as the game numbers it.
    enum class Span { Fixed, FloorToFloor, FloorToRoof, FloorToStairs, FloorToFloorBasedCeiling };
    Span span = Span::FloorToFloor;
    // Stage sides (stageWall) and invisible light blockers (invisibleLightBlocking) are kinds of their own.
    enum class Kind { Plain, InvisibleLightBlocking, Stage };
    Kind kind = Kind::Plain;
    std::string defaultPattern;
    // The wall tool's catalog entries for this style, one per clone (the tool's sub-modes). Each has
    // its thumbnail's UI image, its index in the catalog texts and its building tip (counted from 1,
    // 0 for none). None for a style with notInCatalog.
    struct CatalogClone {
        std::uint32_t thumbnailGroup = 0, thumbnailInstance = 0;
        int textIndex = -1;
        int tip = 0;
    };
    std::vector<CatalogClone> clones;
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
    // Where the wall styles' catalog texts are (catalogTextResource): the text file's name and its
    // string table. Each catalog clone's text index N has its price at string 3N, its name at 3N + 1
    // and its description at 3N + 2.
    std::string wallTextFile;
    std::uint32_t wallTextTable = 0;
    // Roof patterns by the id lots save. The base game's script holds pattern 0, the default manor
    // roof, which has no catalog entry.
    std::map<std::uint32_t, RoofPatternTextures> roofPatterns;
    // The wall layer types of fence arches (fencearches.txt), which the game keeps out of its layer
    // of walls and fences.
    std::set<std::uint32_t> fenceArches;
    // The floor pattern on top of each deck, foundation, pool and stage product (deckfoundations.txt),
    // by the product's id, which the lot's platform region grid holds under it. Lowercase.
    std::map<std::int32_t, std::string> platformFloors;

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
    void addFenceArches(std::string_view text);
    void addPlatforms(std::string_view text);
};

// The scripts of every product, in load order.
BuildScripts loadBuildScripts(const std::vector<Product>& products);

// The GUIDs of the build catalog's greenhouse walls: the fences (XFNC resources in each product's
// Catalog/Patterns packages) whose isgreenhouse is set. A fence that can't be read is left out.
std::set<std::uint32_t> loadGreenhouseFences(const std::vector<Product>& products);
// The GUIDs of the catalog's fences a Sim may walk over (XFNC maywalkover). They are the only
// room walls that don't block walking.
std::set<std::uint32_t> loadWalkOverFences(const std::vector<Product>& products);
// The GUIDs of the build catalog's fences whose property `property` (isgreenhouse, bearsload) is set.
std::set<std::uint32_t> loadFencesWith(const std::vector<Product>& products, const std::string& property);

} // namespace ts2::content
