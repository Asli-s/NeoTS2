#pragma once

#include "formats/tga.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace ts2::cursor {

// A mouse cursor from a Windows .cur file: its picture and the point of it that is the mouse's position.
struct Cursor {
    tga::Pixels pixels;
    std::uint32_t hotX = 0, hotY = 0;
};

// Cursors are small: anything wider or taller than this is a broken file.
constexpr std::uint32_t kMaxSide = 256;

// The first picture of a .cur file (1, 4, 8, 24 or 32 bits per pixel, uncompressed). A pixel the
// file's mask leaves out is transparent. Empty for other kinds or broken data.
[[nodiscard]] std::optional<Cursor> decode(std::span<const std::uint8_t> data);

} // namespace ts2::cursor
