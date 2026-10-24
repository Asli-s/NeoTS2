#pragma once

#include "scene/scene.hpp"

#include <cstdint>

namespace ts2::scene::textureblend {

// How strongly a layer pixel with this alpha covers the one under it, out of 256 (255 -> 256).
int weight(std::uint8_t alpha);

// One channel of a layer pixel drawn over the one under it, as the game's compositor does it.
std::uint8_t blend(std::uint8_t under, std::uint8_t over, int weight);

// `layer` drawn over `under` by the layer's alpha, pixel by pixel. Both must be the same size.
// The colour channels follow the game. `under` keeps its own alpha.
void drawOver(Image& under, const Image& layer);

} // namespace ts2::scene::textureblend
