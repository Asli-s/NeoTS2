#pragma once

#include "scene/light_source.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace ts2::scene::floorlight {

// How light from lamps and windows spreads over one level's floors, as the game's lighting
// manager does it ("diffusion"). The floor is a grid of half-tile cells; a cell on a diagonal
// wall is split into two halves. Light only crosses between neighbouring halves that are linked,
// so walls stop it.

// The eight neighbour directions, as (dx, dy): 0 (0,-1), 1 (-1,-1), 2 (-1,0), 3 (-1,1), 4 (0,1),
// 5 (1,1), 6 (1,0), 7 (1,-1). Odd ones are diagonal.
inline constexpr int kDirX[8] = {0, -1, -1, -1, 0, 1, 1, 1};
inline constexpr int kDirY[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

struct Half {
    std::array<float, 3> light{0, 0, 0};
    std::uint8_t links = 0;       // bit d: linked to the neighbour in direction d
    std::uint8_t toSecond = 0;    // bit d: that link goes to the neighbour's second half
    bool valid = false;           // there is floor here
    bool outside = false;
    std::int16_t room = 0;
    // Working state while lighting; nothing to set.
    bool active = false;          // got brighter last round, so it can pass light on
    bool changed = false;
};

struct Cell {
    std::array<Half, 2> halves;
};

// A tile's floor: whether it has one, and its diagonal wall if any (0 none, 1 along
// x + y = constant, 2 along x = y).
struct Tile {
    bool floor = false;
    std::uint8_t diagonal = 0;
};

struct Grid {
    int level = 0;
    int width = 0, height = 0;    // in tiles; the grid has 2 * width by 2 * height cells
    std::vector<Cell> cells;      // row by row
    std::vector<Tile> tiles;      // width by height, row by row
    std::array<float, 3> insideAmbient{0, 0, 0};   // colour times strength
    std::array<float, 3> outsideAmbient{0, 0, 0};
    float falloffScale = 0.4f;    // floorDiffusionThreshold
    int steps = 50;               // wallPropagationSteps (the game uses it for floors too)
    bool dimOutside = false;      // samples leave out the outside ambient

    int columns() const { return 2 * width; }
    int rows() const { return 2 * height; }
    bool fits() const;            // sizes in range and cells and tiles matching them
    Cell* at(int x, int y);
    const Cell* at(int x, int y) const;
};

// Whether the floor at a cell on a level stops light shining down from above it.
using FloorStops = std::function<bool(int cellX, int cellY, int level)>;

// Resets the grid to its ambient light, then lets the sources light it: sources with nearly
// the same falloff (within 0.01) go together, slowest fading last; each fills its area in its
// room, then the light spreads, fading by 1 / (1 + falloff * falloffScale) per half tile
// (1.4 times the falloff diagonally) plus 0.01, until it no longer brightens anything.
// Returns false when it stopped early at its safety limit (never on a real lot; worth logging).
bool light(Grid& grid, const std::vector<LightSource>& sources, const FloorStops& stops);

// The floor's light at a point (in tiles) as walls and rooms see it: twice the light of the
// half-tile cell (and diagonal half) the point is in; black where there is no floor.
std::array<float, 3> sample(const Grid& grid, float x, float y);

} // namespace ts2::scene::floorlight
