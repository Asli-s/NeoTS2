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
        const std::uint32_t type = r.u32();
        map.typeIndexByObject[object] = type;
        map.order.emplace_back(object, type);
    }
    // Object ids are 16-bit, so a longer table is broken. A map cut short before it has none.
    constexpr std::uint32_t kMaxWaitEntries = 0x7FFF;
    if (r.remaining() >= 4) {
        const std::uint32_t waits = r.u32();
        if (waits > kMaxWaitEntries || waits > r.remaining() / 4) {
            throw std::runtime_error("ObjM: wait table exceeds data");
        }
        map.hasWaits = true;
        map.waitTicks.resize(waits);
        map.waitSecond.resize(waits);
        for (std::size_t i = 0; i < waits; ++i) {
            map.waitTicks[i] = r.i16();
            map.waitSecond[i] = r.u16();
        }
    }
    const auto rest = r.bytes(r.remaining());
    map.rest.assign(rest.begin(), rest.end());
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

namespace ts2::objm {

std::vector<std::uint8_t> write(const ObjectMap& map, std::span<const std::uint8_t> name) {
    if (name.size() != 64) {
        throw std::runtime_error("ObjM: a name is 64 bytes");
    }
    std::vector<std::uint8_t> out(name.begin(), name.end());
    auto put = [&](std::uint32_t v, int bytes) {
        for (int s = 0; s < bytes * 8; s += 8) {
            out.push_back(static_cast<std::uint8_t>(v >> s));
        }
    };
    put(map.unknown, 4);
    put(map.version, 4);
    put(kMagic, 4);
    put(static_cast<std::uint32_t>(map.order.size()), 4);
    for (const auto& [object, type] : map.order) {
        put(object, 4);
        put(type, 4);
    }
    if (map.hasWaits) {
        put(static_cast<std::uint32_t>(map.waitTicks.size()), 4);
        for (std::size_t i = 0; i < map.waitTicks.size(); ++i) {
            put(static_cast<std::uint16_t>(map.waitTicks[i]), 2);
            put(i < map.waitSecond.size() ? map.waitSecond[i] : 0, 2);
        }
    }
    out.insert(out.end(), map.rest.begin(), map.rest.end());
    return out;
}

} // namespace ts2::objm

namespace ts2::objm {

std::vector<std::uint8_t> writeGroups(const ObjectGroups& groups, std::span<const std::uint8_t> name) {
    if (name.size() != 64 || groups.version != 2) {
        throw std::runtime_error("ObjM groups: only version 2 is written");
    }
    std::vector<std::uint8_t> out(name.begin(), name.end());
    auto put = [&](std::uint32_t v, int bytes) {
        for (int s = 0; s < bytes * 8; s += 8) {
            out.push_back(static_cast<std::uint8_t>(v >> s));
        }
    };
    put(groups.version, 4);
    put(static_cast<std::uint32_t>(groups.objectsByKey.size()), 4);
    for (const auto& [key, objects] : groups.objectsByKey) {
        put(key, 4);
        put(static_cast<std::uint32_t>(objects.size()), 4);
        for (const auto object : objects) {
            put(object, 2);
        }
    }
    return out;
}

} // namespace ts2::objm
