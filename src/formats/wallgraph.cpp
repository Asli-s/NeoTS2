#include "formats/wallgraph.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::wallgraph {

const Point* WallGraph::point(std::int32_t id) const {
    for (const auto& p : points) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

PointIndex::PointIndex(const WallGraph& graph) {
    points_.reserve(graph.points.size());
    for (const auto& p : graph.points) {
        points_.emplace(p.id, &p);
    }
}

const Point* PointIndex::operator()(std::int32_t id) const {
    const auto it = points_.find(id);
    return it == points_.end() ? nullptr : it->second;
}

WallGraph parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(64);
    WallGraph g;
    g.classId = r.u32();
    g.version = r.u32();
    const auto name = r.bytes(r.u8());
    g.className.assign(name.begin(), name.end());
    if (g.className != "cWallGraph") {
        throw std::runtime_error("cWallGraph: unexpected class name");
    }
    for (auto& v : g.unknown) {
        v = r.i32();
    }
    g.firstLevel = r.i32();
    g.width = r.i32();
    g.height = r.i32();
    g.levels = r.i32();
    for (auto& v : g.counters) {
        v = r.i32();
    }

    g.points.resize(r.i32Count(16));
    for (auto& p : g.points) {
        p.id = r.i32();
        p.x = r.f32();
        p.y = r.f32();
        p.level = r.i32();
    }
    g.rooms.resize(r.i32Count(4));
    for (auto& room : g.rooms) {
        room = r.i32();
    }
    g.walls.resize(r.i32Count(20));
    for (auto& w : g.walls) {
        w.id = r.i32();
        w.from = r.i32();
        w.leftRoom = r.i32();
        w.to = r.i32();
        w.rightRoom = r.i32();
    }
    if (r.remaining() != 0) {
        throw std::runtime_error("cWallGraph: data left after the walls");
    }
    return g;
}

} // namespace ts2::wallgraph
