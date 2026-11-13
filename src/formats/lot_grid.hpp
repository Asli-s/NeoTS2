#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The lot's grids (c2DArray 0x6B943B43 and c3DArray 0x2A51171B) as the game writes them, so they
// can be written back: 64 zero bytes, the resource type, the version 1, the class name, the width,
// the height (and for a 3D grid the levels), then the cells. Each instance holds its own kind of
// value per cell, written little-endian, or a count then that many numbers for the list grids.
// The cells are kept as their bytes.
namespace ts2::lotgrid {

constexpr std::uint32_t k2DType = 0x6B943B43;
constexpr std::uint32_t k3DType = 0x2A51171B;

struct Grid {
    bool threeD = true;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 1;  // 1 for a 2D grid
    std::vector<std::uint8_t> cells;
};

// Empty for another type, version or class name, a name that is not all zeros, a side larger than
// any lot, or bytes cut short.
std::optional<Grid> readGrid(std::span<const std::uint8_t> data);

// Throws std::runtime_error for a side or level count larger than any lot, or a 2D grid with levels.
std::vector<std::uint8_t> writeGrid(const Grid& grid);

} // namespace ts2::lotgrid
