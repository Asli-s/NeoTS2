#pragma once

#include "formats/tga.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::jpeg {

// Decodes a baseline JPEG to the game's own pixel values (it uses Intel's ijl15.dll on SSE processors):
// AP-922 integer inverse DCT, chroma repeated over the pixels it covers and integer YCbCr to RGB. Only
// what the game's pictures use is read: baseline Huffman, 8-bit, grey or YCbCr in one scan, sampling
// factors 1 or 2. Anything else, broken data or a side over `maxSide` gives nullopt. Alpha is 255.
[[nodiscard]] std::optional<tga::Pixels> decode(std::span<const std::uint8_t> data, std::uint32_t maxSide = tga::kMaxSide);

// Encodes RGB as the game's saved pictures are laid out: baseline JFIF 1.1, the IJG tables for
// `quality` (1 to 100), chroma at half size each way and the standard Huffman tables. `extraApp0`,
// when given, goes in a second APP0 segment after the JFIF one. Alpha is not stored. The game's own
// encoder rounds differently, so the bytes are not the game's. Empty when a side is over 65535,
// the pixels are short or `extraApp0` is over one segment.
[[nodiscard]] std::vector<std::uint8_t> encode(const tga::Pixels& image, int quality, std::span<const std::uint8_t> extraApp0 = {});

} // namespace ts2::jpeg
