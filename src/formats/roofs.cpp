#include "formats/roofs.hpp"

#include "formats/binary_reader.hpp"

#include <bit>
#include <stdexcept>
#include <string>

namespace ts2::roofs {

std::vector<Roof> parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(64);
    r.u32();  // type id
    const std::uint32_t version = r.u32();
    if (version != 1 && version != 3) {
        throw std::runtime_error("roofs: version " + std::to_string(version) + " not supported");
    }
    const std::size_t entrySize = version == 3 ? 44 : 36;
    const std::uint32_t count = r.u32();
    if (count > r.remaining() / entrySize) {
        throw std::runtime_error("roofs: too many entries");
    }
    std::vector<Roof> roofs(count);
    for (auto& f : roofs) {
        f.id = r.u32();
        f.x0 = std::bit_cast<float>(r.u32());
        f.y0 = std::bit_cast<float>(r.u32());
        f.level0 = static_cast<std::int32_t>(r.u32());
        f.x1 = std::bit_cast<float>(r.u32());
        f.y1 = std::bit_cast<float>(r.u32());
        f.level1 = static_cast<std::int32_t>(r.u32());
        f.type = r.u32();
        f.pattern = r.u32();
        if (version == 3) {
            f.slope = std::bit_cast<float>(r.u32());
            f.extra = r.u32();
        }
    }
    return roofs;
}

} // namespace ts2::roofs
