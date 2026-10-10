#include "formats/nhood_objects.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::nhood {

namespace {

constexpr std::size_t kFloraBytes = 38;  // a tree or decoration record
constexpr std::size_t kRoadBytes = 124;

} // namespace

Objects parseObjects(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Objects out;
    out.version = r.u32();
    // Trees and decorations: a list version, a count, then fixed-size records.
    r.u16();
    const std::uint32_t flora = r.u32();
    if (flora > r.remaining() / kFloraBytes) {
        throw std::runtime_error("neighborhood objects: bad tree count");
    }
    r.skip(static_cast<std::size_t>(flora) * kFloraBytes);
    // Roads.
    r.u16();
    const std::uint32_t roads = r.u32();
    if (roads > r.remaining() / kRoadBytes) {
        throw std::runtime_error("neighborhood objects: bad road count");
    }
    out.roads.resize(roads);
    for (auto& road : out.roads) {
        r.u8();  // record version
        for (auto& v : road.center) {
            v = r.f32();
        }
        for (auto& v : road.bounds) {
            v = r.f32();
        }
        r.u8();
        for (auto& corner : road.corners) {
            for (auto& v : corner) {
                v = r.f32();
            }
        }
        road.piece = r.u32();
        r.skip(10);  // the piece's connections and flags (not needed to draw it)
    }
    return out;
}

} // namespace ts2::nhood
