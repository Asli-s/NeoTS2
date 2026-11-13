#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::terrain {

// A lot's terrain textures (resource type 0x4B58975B): the lot's size in tiles, the canvas texture
// under everything (chosen by the neighborhood's terrain type) and the paint textures drawn over it.
struct LotTexture {
    std::uint32_t version = 0;
    std::uint32_t width = 0, height = 0;
    std::string canvas;
    std::vector<std::string> paints;
};

LotTexture parseLotTexture(std::span<const std::uint8_t> data);

constexpr std::uint32_t kLotTextureWrittenVersion = 7;

// The lot texture as the game writes it, at version 7. Throws std::runtime_error for a list too long
// to write.
std::vector<std::uint8_t> writeLotTexture(const LotTexture& texture);

// c2DArray (resource type 0x6B943B43): a grid of fixed-size elements, stored row by row.
struct Array2D {
    std::uint32_t version = 0;
    std::uint32_t width = 0, height = 0;
    std::size_t elementSize = 0;
    std::vector<std::uint8_t> elements;

    // The first byte of the element at (x, y), or 0 outside the grid.
    std::uint8_t byte(std::int32_t x, std::int32_t y) const;
    // The element at (x, y) read as a float, or none outside the grid or when elements aren't 4 bytes.
    std::optional<float> f32(std::int32_t x, std::int32_t y) const;
};

Array2D parseArray2D(std::span<const std::uint8_t> data);
// A c2DArray whose tiles each hold up to three bytes, saved as a count and then the bytes (lot grid
// 0x5CEE). Elements are three bytes, the unused ones 0xFF.
Array2D parseByteLists(std::span<const std::uint8_t> data);

} // namespace ts2::terrain
