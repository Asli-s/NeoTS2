#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ts2::array3d {

// c3DArray (resource type 0x2A51171B): a grid of fixed-size elements per tile and level.
// Elements are stored level by level, then by x, then by y.
struct Array3D {
    std::uint32_t classId = 0;
    std::uint32_t version = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 0;
    std::size_t elementSize = 0;
    std::vector<std::uint8_t> elements;

    bool contains(std::int32_t level, std::int32_t x, std::int32_t y) const;
    std::span<const std::uint8_t> at(std::int32_t level, std::int32_t x, std::int32_t y) const;
    std::int16_t i16(std::int32_t level, std::int32_t x, std::int32_t y, std::size_t index) const;
};

Array3D parse(std::span<const std::uint8_t> data);

} // namespace ts2::array3d
