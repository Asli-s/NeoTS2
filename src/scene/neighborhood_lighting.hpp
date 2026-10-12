#pragma once

#include "scene/scene.hpp"

#include <array>
#include <vector>

namespace ts2::scene {

// Something that raises the neighborhood's shadows (a tree, a lot): a box in world units and its
// height.
struct ShadowBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float height = 0;
};

// What the neighborhood light map is made from: the lighting state's sun (normalized, toward the
// sun) and light map tint, the shadow strength (hmNHoodShadowStrength) and the light map image
// (a colour per normal direction).
struct NeighborhoodLight {
    std::array<float, 3> sun{0, 0, 1};
    std::array<float, 3> tint{1, 1, 1};
    float shadowStrength = 0.47f;   // hmNHoodShadowStrength
    float boxWidening = 0.05f;      // hmBlobWidthModifier
    float boxHeightScale = 0.09f;   // hmBlobHeightModifier
    Image lookup;
};

// The game's neighborhood light map ("nhoodLightMap"): a texel per tile, its colour looked up by
// the terrain's normal at the tile's corner, the normal bent away from the sun where the terrain
// lies in shadow. Heights and normals are per vertex (columns x rows), row by row.
Image neighborhoodLightMap(std::uint32_t columns, std::uint32_t rows, const std::vector<float>& heights,
                           const std::vector<float>& normals, float tileSize, const NeighborhoodLight& light,
                           const std::vector<ShadowBox>& boxes = {});

// The height of the sun's shadow over each point of a grid (columns x rows, `tile` apart, row by row)
// whose heights are `raised`, swept away from the sun one line at a time as the game does for its
// neighborhood and lot light maps; -1000 where nothing shades it. `sun` points toward the sun.
std::vector<float> sunShadow(const std::vector<float>& raised, std::uint32_t columns, std::uint32_t rows, const std::array<float, 3>& sun,
                             double tile);

} // namespace ts2::scene
