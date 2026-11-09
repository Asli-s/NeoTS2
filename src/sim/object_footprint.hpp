#pragma once

#include "formats/scenegraph.hpp"
#include "sim/route_grid.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ts2::sim {

// Where an object stops Sims, as rectangles in route coordinates (route x is model x, route y is
// model -y). The footprint is, in order of choice:
//  - a square with object data 8 bit 0x80 (stair pieces, flower beds), `squareSize` tiles wide
//    (OBJD field 0x43 in 1/16 tile, 0 for a whole tile), less the OBJD 0x4D insets.
//  - the model's "footprint" data list for its graphic (object data 9): a 16x16 bit mask per tile
//    covered, named "(x,y)".
//  - the model's extent, cut to the tile.
struct FootprintShape {
    bool square = false;
    float squareSize = 1;
    // Four 4-bit insets in 1/16 tile, from the low nibble: the -y, -x, +y and +x sides at direction 0
    // (they turn with the object).
    std::uint16_t insets = 0;
    // The model's "footprint" list. It is not owned, so it must outlive the shape (keep the parsed lists).
    const scenegraph::DataValue* masks = nullptr;
    int graphic = 0;
    std::optional<RouteRect> modelBox;  // the model's extent in model coordinates
    // Which tile of the model this object is (multi-tile objects: one object per tile), counted from
    // the lead object's tile along route x and y before turning.
    int tileX = 0, tileY = 0;
};

// The object's footprint around its tile's centre, before turning.
std::vector<RouteRect> footprintRects(const FootprintShape& shape);

// One tile of a model's masks (`masks` is its "footprint" list) for `graphic`, around the tile's
// centre, at the eighth-tile steps routing uses and kept 0.01 inside the cells they cover. Empty
// when the model has no mask for that tile.
std::vector<RouteRect> footprintTile(const scenegraph::DataValue& masks, int graphic, int tileX, int tileY);

// The tiles a model's masks for `graphic` name, along route x and y from the lead's tile before
// turning. Names that are not "(x,y)", masks of the wrong size and tiles more than 64 tiles away are
// left out.
std::vector<std::pair<int, int>> footprintMaskTiles(const scenegraph::DataValue& footprint, int graphic);

// A doorway square that lets Sims through a door's wall, centred on (`x`, `y`).
RouteRect doorwaySquare(float x, float y);

// Turns rectangles around an object's tile centre to face `direction` (object data 1, 0-7) and moves
// them to the object's position (`x`, `y`), snapped to eighths.
std::vector<RouteRect> placeFootprint(const std::vector<RouteRect>& local, float x, float y, int direction);

// How an object's footprint routes, from its live object data (the game sets some of these flags
// while the lot loads, so saved values are not enough).
struct FootprintRouting {
    bool added = true;      // false: the object adds no footprint
    bool passable = false;  // Sims may walk through it, at `cost`
    float cost = kBlockedCost;
    int type = 0;           // the item type of the footprint (its padding is one less)
    bool dynamic = false;   // the object's "allow intersection" function decides per Sim
};
// `flags` is object data 8, `cost` data 0x0A, `flags2` data 0x28. `objectType` is OBJD field 9 and
// `allowsIntersection` whether the object has object function 5.
FootprintRouting footprintRouting(std::uint16_t flags, std::int16_t cost, std::uint16_t flags2, int objectType, bool allowsIntersection);

// The tile of a multi-tile object's model a part at (`x`, `y`) is, from the lead part's position and
// direction. False (and tile 0, 0) when the part is implausibly far from its lead.
bool footprintTileOf(float leadX, float leadY, int leadDirection, float x, float y, int& tileX, int& tileY);

} // namespace ts2::sim
