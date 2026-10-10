#include "formats/nhood_terrain.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::nhood {

namespace {

constexpr std::uint32_t kArray2D = 0x6B943B43;

// A c2DArray block: id, version, its class name, then columns x rows values of `size` bytes.
template <class T>
std::vector<T> readArray(BinaryReader& r, std::uint32_t& columns, std::uint32_t& rows) {
    if (r.u32() != kArray2D) {
        throw std::runtime_error("neighborhood terrain: expected a c2DArray");
    }
    r.u32();  // version
    r.skip(r.u32());  // "c2DArray"
    columns = r.u32();
    rows = r.u32();
    const std::size_t count = static_cast<std::size_t>(columns) * rows;
    if (count > r.remaining() / sizeof(T)) {
        throw std::runtime_error("neighborhood terrain: array too large");
    }
    std::vector<T> values(count);
    for (auto& v : values) {
        if constexpr (sizeof(T) == 4) {
            v = r.f32();
        } else {
            v = r.u8();
        }
    }
    return values;
}

} // namespace

Terrain parseTerrain(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    if (r.u32() != kTerrainType) {
        throw std::runtime_error("neighborhood terrain: wrong type id");
    }
    Terrain t;
    t.version = r.u32();
    t.width = r.u32();
    t.height = r.u32();
    t.seaLevel = r.f32();
    const std::uint32_t n = r.u32();
    if (n > r.remaining()) {
        throw std::runtime_error("neighborhood terrain: bad type name");
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        t.type += static_cast<char>(r.u8());
    }
    t.heights = readArray<float>(r, t.columns, t.rows);
    std::uint32_t c = 0, rr = 0;
    t.extra = readArray<std::uint8_t>(r, c, rr);
    return t;
}

std::optional<SavedCamera> parseSavedCamera(std::span<const std::uint8_t> data) {
    if (data.size() != 115) {
        return std::nullopt;
    }
    BinaryReader r(data);
    SavedCamera c;
    r.skip(0x48);
    c.distance = r.f32();
    r.skip(5);
    c.yaw = r.f32();
    r.f32();  // not known
    c.target[0] = r.f32();
    c.target[1] = r.f32();
    c.target[2] = r.f32();
    return c;
}

} // namespace ts2::nhood
