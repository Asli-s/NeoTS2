#pragma once

#include "scene/floor_light.hpp"
#include "scene/wall_light.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace ts2::scene::lightmaptexels {

// The light-map textures the game draws floors and walls with, texel for texel as its lighting
// manager writes them. Texels are packed colours (walllight::Pixel). The shaders read them times 2.

// A level's floor light map: 8 x 8 texels per tile, rows running with +y (row 8y + j holds the
// tile's j-th row), columns with +x. Tiles with no floor keep `fill`.
struct FloorImage {
    int width = 0, height = 0;  // in texels: 8 x the grid's tiles
    std::vector<walllight::Pixel> texels;
    const walllight::Pixel* at(int x, int y) const;
};
FloorImage floorTexels(const floorlight::Grid& grid, const walllight::Pixel& fill);

// The size of a wall face's rect: 4 x 8 for a face looking into a room whose wall is one unit
// high, else 8 wide by 8 per unit of height (3 for ordinary walls, 1 to 4 for walls whose height
// varies, such as foundations).
struct Size {
    int width = 8, height = 24;
};
Size panelSize(bool inside, int heightUnits);

// One wall segment's texels in its rect, rows from the bottom of the wall up, columns from the
// segment's start: its padded light (`texels`, see walllight::Segment) less `ambient` (the
// outside wall ambient for faces looking outdoors, else none), times `scale` (0.5 in the game).
// `attic`: an inside face of an attic wall, whose rows above the sampled ones are cleared.
// `fade` scales those rows of tall rects (wallDiffusionThreshold).
std::vector<walllight::Pixel> panelTexels(const std::array<walllight::Column, 4>& texels, Size size, const std::array<float, 3>& ambient,
                                          float scale, bool attic, float fade);

// The byte the game adds to every light-map texel of a room for its unlit room light: half the
// colour times the room's share (roomlight::unlitShare).
walllight::Pixel roomAdd(const std::array<float, 3>& unlitColor, float share);
// Adds each room's byte (none for room 0 or a zero byte) to its floor tiles' texels, saturating.
// On a tile split by a diagonal wall each half takes its own room.
void addRoomLight(FloorImage& image, const floorlight::Grid& grid, const std::function<walllight::Pixel(std::int16_t room)>& add);
// Adds a byte to every texel of a wall face's rect, saturating.
void addRoomLight(std::vector<walllight::Pixel>& panel, const walllight::Pixel& add);

// The sunlight the game adds to the light maps of floors and wall faces outdoors: each of the
// lighting state's outdoor lights (the sun first, times how much of it gets past the buildings'
// shadow) by how much the surface faces it, the rest of the way from the sky light, halved and
// added as bytes.
struct OutdoorLight {
    std::array<float, 3> color{0, 0, 0};
    float strength = 0;
    std::array<float, 3> direction{0, 0, 1};  // toward the light, normalized
};
struct Outdoor {
    std::vector<OutdoorLight> lights;
    std::array<float, 3> skylight{0, 0, 0};   // colour times strength
    float strength = 1;                        // terrainStrength
};
walllight::Pixel outdoorAdd(const Outdoor& outdoor, const std::array<float, 3>& normal, float sun = 1);
// What a floor texel outdoors faces and how much sun reaches it. `alpha` is what the game keeps
// in the texel's alpha (how much sun, 255 for all).
struct OutdoorTexel {
    std::array<float, 3> normal{0, 0, 1};
    float sun = 1;
    std::uint8_t alpha = 255;
};
// Texel i (along x) of row k (along y) of tile (x, y).
using OutdoorAt = std::function<OutdoorTexel(int tileX, int tileY, int i, int k)>;
// Adds it to every texel of the floor tiles (or diagonal halves) that are outdoors. Without
// `at` they face up in full sun.
void addOutdoorLight(FloorImage& image, const floorlight::Grid& grid, const Outdoor& outdoor, const OutdoorAt& at = nullptr);

} // namespace ts2::scene::lightmaptexels
