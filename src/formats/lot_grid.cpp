#include "formats/lot_grid.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ts2::lotgrid {

namespace {

constexpr std::size_t kNameSize = 64;
constexpr std::uint32_t kVersion = 1;
// The same limits as the grids' readers: no lot is wider than this or has more levels.
constexpr std::uint32_t kMaxSide = 1024;
constexpr std::uint32_t kMaxLevels = 64;

const char* className(bool threeD) {
    return threeD ? "c3DArray" : "c2DArray";
}

void put(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(v >> shift));
    }
}

} // namespace

std::optional<Grid> readGrid(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        const auto name = r.bytes(kNameSize);
        if (std::any_of(name.begin(), name.end(), [](std::uint8_t b) { return b != 0; })) {
            return std::nullopt;
        }
        Grid g;
        const std::uint32_t type = r.u32();
        if (type != k2DType && type != k3DType) {
            return std::nullopt;
        }
        g.threeD = type == k3DType;
        if (r.u32() != kVersion) {
            return std::nullopt;
        }
        const std::string expected = className(g.threeD);
        // The class name's length is one byte, as the game writes names this short.
        const auto text = r.bytes(r.u8());
        if (std::string(text.begin(), text.end()) != expected) {
            return std::nullopt;
        }
        g.width = r.u32();
        g.height = r.u32();
        g.levels = g.threeD ? r.u32() : 1;
        if (g.width > kMaxSide || g.height > kMaxSide || g.levels > kMaxLevels) {
            return std::nullopt;
        }
        const auto rest = r.bytes(r.remaining());
        g.cells.assign(rest.begin(), rest.end());
        return g;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeGrid(const Grid& grid) {
    if (grid.width > kMaxSide || grid.height > kMaxSide || grid.levels > kMaxLevels || (!grid.threeD && grid.levels != 1)) {
        throw std::runtime_error("lot grid: bad size to write");
    }
    std::vector<std::uint8_t> out(kNameSize, 0);
    put(out, grid.threeD ? k3DType : k2DType);
    put(out, kVersion);
    const std::string name = className(grid.threeD);
    out.push_back(static_cast<std::uint8_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    put(out, grid.width);
    put(out, grid.height);
    if (grid.threeD) {
        put(out, grid.levels);
    }
    out.insert(out.end(), grid.cells.begin(), grid.cells.end());
    return out;
}

} // namespace ts2::lotgrid
