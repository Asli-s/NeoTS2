#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ts2::wallgraph {

struct Point {
    std::int32_t id = 0;
    float x = 0;
    float y = 0;
    std::int32_t level = 0;
};

// A wall segment between two points, with the room on each side. Walking from `from` to `to`,
// `leftRoom` is on the left when x grows to the right and y grows downward.
struct Wall {
    std::int32_t id = 0;
    std::int32_t from = 0;
    std::int32_t leftRoom = 0;
    std::int32_t to = 0;
    std::int32_t rightRoom = 0;
};

// cWallGraph (resource type 0x0A284D0B).
struct WallGraph {
    std::uint32_t classId = 0;
    std::uint32_t version = 0;
    std::string className;
    std::array<std::int32_t, 2> unknown{};
    // Lowest level the lot uses: -1 with a foundation, else 0 (saved grids start at this level).
    std::int32_t firstLevel = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t levels = 0;
    std::array<std::int32_t, 2> counters{};
    std::vector<Point> points;
    std::vector<std::int32_t> rooms;
    std::vector<Wall> walls;

    const Point* point(std::int32_t id) const;
};

WallGraph parse(std::span<const std::uint8_t> data);

} // namespace ts2::wallgraph
