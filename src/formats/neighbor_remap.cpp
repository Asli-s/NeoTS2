#include "formats/neighbor_remap.hpp"

#include "formats/binary_reader.hpp"

namespace ts2::neighbor_remap {

namespace {

void skipPairs(BinaryReader& r) {
    r.bytes(r.u32Count(8) * 8);
}

void skipList(BinaryReader& r) {
    r.bytes(r.u32Count(4) * 4);
}

} // namespace

NeighborRemap parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    NeighborRemap remap;
    remap.version = r.u32();
    if (remap.version < 1) {
        return remap;
    }
    skipPairs(r);
    if (remap.version < 2) {
        return remap;
    }
    skipPairs(r);
    skipList(r);
    skipList(r);
    skipPairs(r);
    const std::size_t count = r.u32Count(8);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t from = r.u32();
        remap.neighborIds[from] = r.u32();
    }
    skipPairs(r);
    return remap;
}

} // namespace ts2::neighbor_remap
