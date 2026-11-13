#include "formats/roofs.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <bit>
#include <stdexcept>
#include <string>

namespace ts2::roofs {

std::vector<Roof> parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(64);
    r.u32();  // type id
    const std::uint32_t version = r.u32();
    if (version < 1 || version > 3) {
        throw std::runtime_error("roofs: version " + std::to_string(version) + " not supported");
    }
    // Version 2 adds the slope, version 3 the last field.
    const std::size_t entrySize = version == 3 ? 44 : version == 2 ? 40 : 36;
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
        if (version >= 2) {
            f.slope = std::bit_cast<float>(r.u32());
        }
        if (version >= 3) {
            f.extra = r.u32();
        }
    }
    return roofs;
}

std::vector<std::uint8_t> write(const std::vector<Roof>& roofs) {
    BinaryWriter w;
    w.zeros(64);
    w.u32(kType);
    w.u32(kWrittenVersion);
    w.count(roofs.size());
    for (const Roof& f : roofs) {
        w.u32(f.id);
        w.f32(f.x0);
        w.f32(f.y0);
        w.i32(f.level0);
        w.f32(f.x1);
        w.f32(f.y1);
        w.i32(f.level1);
        w.u32(f.type);
        w.u32(f.pattern);
        w.f32(f.slope);
        w.u32(f.extra);
    }
    return w.take();
}

} // namespace ts2::roofs
