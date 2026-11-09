#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace ts2::dxt {

// Compresses an 8-bit RGBA image (rows from the top) to DXT5, 16 bytes per 4 x 4 block, rows of
// blocks from the top. Sides that aren't whole blocks repeat their last row and column. Empty when
// the pixels don't fill the image.
std::vector<std::uint8_t> compressDxt5(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height);

} // namespace ts2::dxt
