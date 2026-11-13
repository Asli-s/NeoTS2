#include "formats/nhood_terrain.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <cmath>
#include <stdexcept>
#include <string_view>

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
            // A height that isn't a number (a broken file) reads as 0.
            const float h = r.f32();
            v = std::isfinite(h) ? h : 0.0f;
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

namespace {

constexpr std::uint32_t kArrayVersion = 1;
constexpr std::string_view kArrayName = "c2DArray";

// The c2DArray header the game writes, false when it is another.
bool readArrayHeader(BinaryReader& r, std::uint32_t& columns, std::uint32_t& rows, std::size_t valueSize) {
    if (r.u32() != kArray2D || r.u32() != kArrayVersion) {
        return false;
    }
    const auto name = r.bytes(r.u32());
    if (std::string_view(reinterpret_cast<const char*>(name.data()), name.size()) != kArrayName) {
        return false;
    }
    columns = r.u32();
    rows = r.u32();
    return static_cast<std::uint64_t>(columns) * rows <= r.remaining() / valueSize;
}

void writeArrayHeader(BinaryWriter& w, std::uint32_t columns, std::uint32_t rows, std::size_t values) {
    if (static_cast<std::uint64_t>(columns) * rows != values) {
        throw std::runtime_error("neighborhood terrain: a grid whose values do not match its size");
    }
    w.u32(kArray2D);
    w.u32(kArrayVersion);
    w.fullText(kArrayName);
    w.u32(columns);
    w.u32(rows);
}

} // namespace

std::optional<TerrainRecord> readTerrainRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.u32() != kTerrainType || r.u32() != kTerrainWrittenVersion) {
            return std::nullopt;
        }
        TerrainRecord t;
        t.width = r.u32();
        t.height = r.u32();
        t.seaLevel = r.f32();
        const auto type = r.bytes(r.u32());
        t.type.assign(type.begin(), type.end());
        if (!readArrayHeader(r, t.heightColumns, t.heightRows, 4)) {
            return std::nullopt;
        }
        t.heights.resize(static_cast<std::size_t>(t.heightColumns) * t.heightRows);
        for (float& h : t.heights) {
            h = r.f32();
        }
        if (!readArrayHeader(r, t.extraColumns, t.extraRows, 1)) {
            return std::nullopt;
        }
        const auto extra = r.bytes(static_cast<std::size_t>(t.extraColumns) * t.extraRows);
        t.extra.assign(extra.begin(), extra.end());
        if (r.remaining() != 0) {
            return std::nullopt;
        }
        return t;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeTerrainRecord(const TerrainRecord& t) {
    BinaryWriter w;
    w.u32(kTerrainType);
    w.u32(kTerrainWrittenVersion);
    w.u32(t.width);
    w.u32(t.height);
    w.f32(t.seaLevel);
    w.fullText(t.type);
    writeArrayHeader(w, t.heightColumns, t.heightRows, t.heights.size());
    for (const float h : t.heights) {
        w.f32(h);
    }
    writeArrayHeader(w, t.extraColumns, t.extraRows, t.extra.size());
    w.bytes(t.extra);
    return w.take();
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
