#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace ts2::content {
struct Lot;
}

namespace ts2::scene::lotshadow {

// The sun's shadow over a lot, as the game works it out for its light maps: a height map of eight
// samples a tile (the topmost floor over each spot, the tops of its walls), swept away from the sun.
// The light maps then dim the sun wherever the shadow passes above a surface.

constexpr int kSamplesPerTile = 8;
constexpr float kCell = 1.0f / kSamplesPerTile;

// A wall as the shadow sees it: tile points on a level; a fence or railing stands `fenceHeight`
// above its own level, any other wall reaches the floor of the level above.
struct Wall {
    int level = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool fence = false;
    float fenceHeight = 0;
};

struct Inputs {
    int width = 0, height = 0;  // the lot in tiles
    int levels = 0;             // 0 .. levels - 1
    // The height of a level's floor at a tile corner.
    std::function<float(int x, int y, int level)> corner;
    // Whether a tile quarter has floor on a level (bit 0 west, 1 north, 2 east, 3 south); the ground
    // level always counts.
    std::function<bool(int x, int y, int level, int quarter)> floorAt;
    std::vector<Wall> walls;
};

struct Grid {
    int width = 0, height = 0;          // samples
    std::vector<float> heights, ground, shadow;
    // The shadow's height over a point (world tiles), or -1000 outside the grid.
    float shadowAt(float x, float y) const;
    float groundAt(int sampleX, int sampleY) const;
};

// A lot's floors, corner heights and walls (any covering for which `isFence` holds stands as a
// fence or railing, 1.1824 high: the railings seen on the game's lots; where the game keeps that
// height per style is not read). The inputs read from `lot`, which must outlive them.
Inputs fromLot(const content::Lot& lot, const std::function<bool(std::uint32_t covering)>& isFence);

// The height of a level's floor at a point: between its four corners (levels outside the lot's are
// a storey (3) below its lowest or above its highest).
float elevation(const Inputs& in, float x, float y, int level);

// `sun` points toward the sun, normalized. Lots larger than 256 tiles a side give nothing.
Grid build(const Inputs& in, const std::array<float, 3>& sun);

// How much of the sun reaches a point under a shadow `depth` deep (the shadow's height above it):
// all of it at or above the shadow, fading out over `zone` (hmSunLightTransitionZone) below it.
float sunVisible(float depth, float zone);

} // namespace ts2::scene::lotshadow
