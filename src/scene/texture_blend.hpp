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

// How strongly a layer pixel covers the one under it through a mask, out of 256: the mask's green
// channel, scaled by the layer's amount (0 to 255) when it is below 255.
int maskWeight(std::uint8_t maskGreen, std::uint8_t amount);

// `layer` drawn over `under` through `mask`, as the game composites a masked layer (a pet's coat
// patches): a layer pixel that is all zero is left out, the others cover by maskWeight. All three
// must be the same size. `under` keeps its own alpha.
void drawThroughMask(Image& under, const Image& layer, const Image& mask, std::uint8_t amount);

} // namespace ts2::scene::textureblend
