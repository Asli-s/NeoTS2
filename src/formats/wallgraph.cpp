#include "formats/wallgraph.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

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

std::vector<std::uint8_t> write(const WallGraph& graph) {
    BinaryWriter w;
    w.recordHeader(kType, kWrittenVersion, "cWallGraph");
    for (const std::int32_t v : graph.unknown) {
        w.i32(v);
    }
    w.i32(graph.firstLevel);
    w.i32(graph.width);
    w.i32(graph.height);
    w.i32(graph.levels);
    for (const std::int32_t v : graph.counters) {
        w.i32(v);
    }
    w.count(graph.points.size());
    for (const Point& p : graph.points) {
        w.i32(p.id);
        w.f32(p.x);
        w.f32(p.y);
        w.i32(p.level);
    }
    w.count(graph.rooms.size());
    for (const std::int32_t room : graph.rooms) {
        w.i32(room);
    }
    w.count(graph.walls.size());
    for (const Wall& wall : graph.walls) {
        w.i32(wall.id);
        w.i32(wall.from);
        w.i32(wall.leftRoom);
        w.i32(wall.to);
        w.i32(wall.rightRoom);
    }
    return w.take();
}

} // namespace ts2::wallgraph
