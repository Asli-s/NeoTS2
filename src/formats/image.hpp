#pragma once

#include "formats/tga.hpp"

#include <optional>
#include <span>

namespace ts2::image {

// An image resource (type 0x856DDBAC): Targa, PNG, JPEG or bitmap, as RGBA rows from the top.
std::optional<tga::Pixels> decode(std::span<const std::uint8_t> data);

} // namespace ts2::image
