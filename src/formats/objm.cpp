#include "formats/objm.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::objm {

namespace {
constexpr std::uint32_t kMagic = 0x4F626A4D;
}

ObjectMap parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(64);
    ObjectMap map;
    map.unknown = r.u32();
    map.version = r.u32();
    if (r.u32() != kMagic) {
        throw std::runtime_error("ObjM: bad magic");
    }
    const std::uint32_t count = r.u32();
    if (count > r.remaining() / 8) {
        throw std::runtime_error("ObjM: object count exceeds data");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t object = r.u32();
        map.typeIndexByObject[object] = r.u32();
    }
    return map;
}

} // namespace ts2::objm
