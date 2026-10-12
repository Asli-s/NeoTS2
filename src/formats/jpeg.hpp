#pragma once

#include "formats/tga.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace ts2::jpeg {

// A baseline JPEG decoded the way the game decodes its pictures (it uses Intel's JPEG library,
// ijl15.dll, on processors with SSE), to the same pixel values: its AP-922 integer inverse DCT,
// chroma samples repeated over the pixels they cover, and its integer YCbCr to RGB. Only what the
// game's own pictures use is read: baseline Huffman coding, 8-bit samples, one (grey) or three
// (YCbCr) components in one scan, sampling factors 1 or 2. Anything else (progressive, arithmetic
// coded, 12-bit, CMYK) gives nullopt, as does broken data or a side over `maxSide`. Alpha is 255
// everywhere.
std::optional<tga::Pixels> decode(std::span<const std::uint8_t> data, std::uint32_t maxSide = tga::kMaxSide);

} // namespace ts2::jpeg
