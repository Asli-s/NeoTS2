#pragma once

#include "formats/tga.hpp"

#include <optional>
#include <span>
#include <vector>

namespace ts2::image {

// An image resource (type 0x856DDBAC): Targa, PNG, JPEG or bitmap, as RGBA rows from the top. A
// JPEG takes its alpha from the game's "ALFA" segment when it has one. Nullopt when a side is over
// `maxSide`, checked before any pixels are decoded.
[[nodiscard]] std::optional<tga::Pixels> decode(std::span<const std::uint8_t> data, std::uint32_t maxSide = tga::kMaxSide);

// A JPEG holding the picture's alpha in an "ALFA" segment, as the game saves family pictures:
// quality 75, the alpha run-length coded as decode reads it (two or more equal bytes always make a
// run, up to 129, and the rest go in literals of up to 128). Empty when the coded alpha doesn't fit
// in one segment.
[[nodiscard]] std::vector<std::uint8_t> encodeJpegWithAlpha(const tga::Pixels& image);

// The run-length coded alpha of an "ALFA" segment, without the name.
[[nodiscard]] std::vector<std::uint8_t> packAlpha(std::span<const std::uint8_t> alpha);

} // namespace ts2::image
