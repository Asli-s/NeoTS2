#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::scene {

// The normal map the game makes from a bump map's heights (a texture of format 6, a byte per
// pixel, rows from the top) when it loads it, texel for texel as its own converter:
// per pixel, RGBA with red the normal's x, green its y, blue its z (128 for 0) and alpha 255. The
// slopes come from the four diagonal neighbours, wrapping around the edges, against 1020 /
// `amplitude` (the texture's own float, 10 in the game's files). Empty when `heights` doesn't hold
// width x height bytes or the amplitude isn't positive.
std::vector<std::uint8_t> bumpNormals(std::span<const std::uint8_t> heights, std::uint32_t width, std::uint32_t height,
                                      float amplitude = 10);

} // namespace ts2::scene
