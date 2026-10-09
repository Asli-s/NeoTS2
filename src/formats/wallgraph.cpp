#include "formats/wallgraph.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::wallgraph {

namespace {

std::size_t readCount(BinaryReader& r, std::size_t elementSize) {
    const std::int32_t count = r.i32();
    if (count < 0 || static_cast<std::size_t>(count) * elementSize > r.remaining()) {
        throw std::runtime_error("cWallGraph: bad count");
    }
    return static_cast<std::size_t>(count);
}

} // namespace

const Point* WallGraph::point(std::int32_t id) const {
    for (const auto& p : points) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
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

    g.points.resize(readCount(r, 16));
    for (auto& p : g.points) {
        p.id = r.i32();
        p.x = r.f32();
        p.y = r.f32();
        p.level = r.i32();
    }
    g.rooms.resize(readCount(r, 4));
    for (auto& room : g.rooms) {
        room = r.i32();
    }
    g.walls.resize(readCount(r, 20));
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
