#include "formats/array3d.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::array3d {

namespace {
constexpr std::uint32_t kMaxSide = 1024;
constexpr std::uint32_t kMaxLevels = 64;
}

bool Array3D::contains(std::int32_t level, std::int32_t x, std::int32_t y) const {
    return level >= 0 && x >= 0 && y >= 0 && static_cast<std::uint32_t>(level) < levels &&
           static_cast<std::uint32_t>(x) < width && static_cast<std::uint32_t>(y) < height;
}

std::span<const std::uint8_t> Array3D::at(std::int32_t level, std::int32_t x, std::int32_t y) const {
    if (!contains(level, x, y)) {
        return {};
    }
    const std::size_t index = (static_cast<std::size_t>(level) * width + x) * height + y;
    // parse() makes the elements fill the grid, but a grid built by hand might not.
    if (elementSize == 0 || index >= elements.size() / elementSize) {
        return {};
    }
    return std::span<const std::uint8_t>(elements).subspan(index * elementSize, elementSize);
}

std::int16_t Array3D::i16(std::int32_t level, std::int32_t x, std::int32_t y, std::size_t index) const {
    const auto e = at(level, x, y);
    if (index >= e.size() / 2) {
        return 0;
    }
    return static_cast<std::int16_t>(e[index * 2] | e[index * 2 + 1] << 8);
}

namespace {

// The header both kinds share. An empty grid (any side 0) is allowed, as the game's readers allow it.
Array3D readHeader(BinaryReader& r) {
    r.skip(64);
    Array3D a;
    a.classId = r.u32();
    a.version = r.u32();
    const auto name = r.bytes(r.u8());
    if (std::string(name.begin(), name.end()) != "c3DArray") {
        throw std::runtime_error("c3DArray: unexpected class name");
    }
    a.width = r.u32();
    a.height = r.u32();
    a.levels = r.u32();
    if (a.width > kMaxSide || a.height > kMaxSide || a.levels > kMaxLevels) {
        throw std::runtime_error("c3DArray: bad size");
    }
    return a;
}

std::size_t cellCount(const Array3D& a) {
    return static_cast<std::size_t>(a.width) * a.height * a.levels;
}

} // namespace

Array3D parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Array3D a = readHeader(r);
    const std::size_t cells = cellCount(a);
    if (cells == 0) {
        if (r.remaining() != 0) {
            throw std::runtime_error("c3DArray: data in an empty grid");
        }
        return a;
    }
    if (r.remaining() == 0 || r.remaining() % cells != 0) {
        throw std::runtime_error("c3DArray: data does not fill the grid evenly");
    }
    a.elementSize = r.remaining() / cells;
    const auto rest = r.bytes(r.remaining());
    a.elements.assign(rest.begin(), rest.end());
    return a;
}

ListGrid parseLists(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    const Array3D header = readHeader(r);
    ListGrid g;
    g.width = header.width;
    g.height = header.height;
    g.levels = header.levels;
    const std::size_t cells = cellCount(header);
    // Each tile takes at least its 4-byte count.
    if (cells > r.remaining() / 4) {
        throw std::runtime_error("c3DArray: too short for its grid");
    }
    g.cells.resize(cells);
    for (auto& cell : g.cells) {
        const std::size_t count = r.u32Count(4);
        cell.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            cell.push_back(r.u32());
        }
    }
    if (r.remaining() != 0) {
        throw std::runtime_error("c3DArray: data left after the grid");
    }
    return g;
}

} // namespace ts2::array3d
