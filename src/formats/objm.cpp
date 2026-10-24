#include "formats/objm.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>
#include <string>

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

ObjectGroups parseGroups(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(64);
    ObjectGroups groups;
    groups.version = r.u32();
    if (groups.version != 1 && groups.version != 2) {
        throw std::runtime_error("ObjM groups: unknown version " + std::to_string(groups.version));
    }
    // Each entry takes at least its key and count.
    const std::size_t count = r.u32Count(8);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t key = r.u32();
        std::vector<std::uint16_t> objects(r.u32Count(2));
        for (auto& object : objects) {
            object = r.u16();
        }
        groups.objectsByKey[key] = std::move(objects);
    }
    if (groups.version == 1) {
        const std::size_t extra = r.u32Count(4);
        r.skip(extra * 4);
    }
    return groups;
}

} // namespace ts2::objm
