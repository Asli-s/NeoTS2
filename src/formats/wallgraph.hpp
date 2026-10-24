#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
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
    // The lot's lowest level: -1 when it has a level below the ground storey, else 0. Its grids
    // start at this level.
    std::int32_t firstLevel = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t levels = 0;
    std::array<std::int32_t, 2> counters{};
    std::vector<Point> points;
    std::vector<std::int32_t> rooms;
    std::vector<Wall> walls;

    // The first point with this id, or null. It searches all points: to look up many, use PointIndex.
    const Point* point(std::int32_t id) const;
};

// A graph's points by id, the first of any repeated id as WallGraph::point finds it. It points into
// the graph, so it is valid while the graph's points are unchanged.
class PointIndex {
public:
    explicit PointIndex(const WallGraph& graph);
    const Point* operator()(std::int32_t id) const;

private:
    std::unordered_map<std::int32_t, const Point*> points_;
};

WallGraph parse(std::span<const std::uint8_t> data);

} // namespace ts2::wallgraph
