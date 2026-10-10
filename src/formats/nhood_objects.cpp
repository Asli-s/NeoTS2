#include "formats/nhood_objects.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::nhood {

namespace {

constexpr std::size_t kRoadBytes = 124;
// Plants saved before list version 3 used a smaller tile.
constexpr float kOldFloraScale = 1.25f;

} // namespace

Objects parseObjects(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Objects out;
    out.version = r.u32();
    // Plants: a list version, a count, then records whose fields depend on the version.
    const std::uint16_t listVersion = r.u16();
    const std::uint32_t flora = r.u32();
    if (flora > r.remaining() / 12) {
        throw std::runtime_error("neighborhood objects: bad plant count");
    }
    out.flora.resize(flora);
    for (auto& f : out.flora) {
        std::uint16_t version = listVersion;
        if (listVersion < 6) {
            for (auto& v : f.position) {
                v = r.f32();
            }
        } else {
            const std::uint8_t placement = r.u8();
            for (auto& v : f.position) {
                v = r.f32();
            }
            for (auto& v : f.bounds) {
                v = r.f32();
            }
            if (placement < 2) {
                f.bounds = {};
            }
            version = r.u8();
        }
        if (version >= 7) {
            f.angle = r.f32();
        }
        if (version >= 8) {
            f.type = r.u32();
        }
        if (version < 3) {
            f.position[0] *= kOldFloraScale;
            f.position[1] *= kOldFloraScale;
        }
        if (version < 6 && version > 3) {
            r.skip(16);
        }
    }
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
