#pragma once

#include "formats/tga.hpp"

#include <optional>
#include <span>

namespace ts2::image {

// An image resource (type 0x856DDBAC): Targa, PNG, JPEG or bitmap, as RGBA rows from the top. A
// JPEG takes its alpha from the game's "ALFA" segment when it has one. Nullopt when a side is over
// `maxSide`, checked before any pixels are decoded.
[[nodiscard]] std::optional<tga::Pixels> decode(std::span<const std::uint8_t> data, std::uint32_t maxSide = tga::kMaxSide);

} // namespace ts2::image
