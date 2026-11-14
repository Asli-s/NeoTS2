#include "formats/wallgraph.hpp"

#include "formats/binary_reader.hpp"

#include <cstring>
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

namespace ts2::wallgraph {

namespace {

void put(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) {
        out.push_back(static_cast<std::uint8_t>(v >> s));
    }
}

void put(std::vector<std::uint8_t>& out, std::int32_t v) { put(out, static_cast<std::uint32_t>(v)); }

void put(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    put(out, bits);
}

} // namespace

std::vector<std::uint8_t> write(const WallGraph& graph, std::span<const std::uint8_t> name) {
    if (name.size() != 64 || graph.className.size() > 255) {
        throw std::runtime_error("cWallGraph: can't write that name");
    }
    std::vector<std::uint8_t> out(name.begin(), name.end());
    put(out, graph.classId);
    put(out, graph.version);
    out.push_back(static_cast<std::uint8_t>(graph.className.size()));
    out.insert(out.end(), graph.className.begin(), graph.className.end());
    for (const auto v : graph.unknown) {
        put(out, v);
    }
    for (const auto v : {graph.firstLevel, graph.width, graph.height, graph.levels, graph.counters[0], graph.counters[1]}) {
        put(out, v);
    }
    put(out, static_cast<std::int32_t>(graph.points.size()));
    for (const auto& p : graph.points) {
        put(out, p.id);
        put(out, p.x);
        put(out, p.y);
        put(out, p.level);
    }
    put(out, static_cast<std::int32_t>(graph.rooms.size()));
    for (const auto r : graph.rooms) {
        put(out, r);
    }
    put(out, static_cast<std::int32_t>(graph.walls.size()));
    for (const auto& w : graph.walls) {
        for (const auto v : {w.id, w.from, w.leftRoom, w.to, w.rightRoom}) {
            put(out, v);
        }
    }
    return out;
}

} // namespace ts2::wallgraph
