#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::scene {

// The normal map the game makes from a bump map (texture format 6, a height byte per pixel, rows from
// the top) on load, texel for texel: RGBA with x, y, z in red, green, blue (128 for 0) and alpha 255.
// Slopes come from the four diagonal neighbours, wrapping at edges, against 1020 / `amplitude` (the
// texture's own float, 10 in the game's files). Empty for a wrong `heights` size or amplitude <= 0.
std::vector<std::uint8_t> bumpNormals(std::span<const std::uint8_t> heights, std::uint32_t width, std::uint32_t height,
                                      float amplitude = 10);

} // namespace ts2::scene
