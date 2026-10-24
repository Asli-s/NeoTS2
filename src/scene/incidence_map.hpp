#pragma once

#include "scene/scene.hpp"

namespace ts2::scene {

// The incidence map the game makes of a floor or wall light map for bump-mapped floors and walls:
// half the light map's size, each texel from a 2 x 2 block of light-map texels: the direction the
// light grows brighter, as red (x, along the columns) and green (y, down the rows) around 128, with
// blue and alpha 255. `scale` is Lighting.txt's incidenceMapScale, `heightClamp` its
// incidenceMapHeightClamp for floors, 0.8 for walls. Empty when the light map has no whole block.
Image incidenceMap(const Image& lightMap, float scale, float heightClamp);

} // namespace ts2::scene
