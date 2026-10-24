#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::tga {

// Decoded pixels, rows from the top, 4 bytes (RGBA) each.
struct Pixels {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

// The widest or tallest image any decoder accepts: far larger than anything the game has, so a
// broken file can't ask for huge amounts of memory.
constexpr std::uint32_t kMaxSide = 16384;

// A Targa image: true color (24 or 32 bits) or grayscale (8 bits), plain or run-length encoded.
// Empty for other kinds, broken data, or a side over `maxSide`.
[[nodiscard]] std::optional<Pixels> decode(std::span<const std::uint8_t> data, std::uint32_t maxSide = kMaxSide);

} // namespace ts2::tga
