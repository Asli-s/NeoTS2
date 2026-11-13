#include "formats/objm.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <exception>
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
    // Object ids are 16-bit, so a longer table is broken. A map cut short before it has none.
    constexpr std::uint32_t kMaxWaitEntries = 0x7FFF;
    if (r.remaining() >= 4) {
        const std::uint32_t waits = r.u32();
        if (waits > kMaxWaitEntries || waits > r.remaining() / 4) {
            throw std::runtime_error("ObjM: wait table exceeds data");
        }
        map.waitTicks.resize(waits);
        for (auto& ticks : map.waitTicks) {
            ticks = r.i16();
            r.u16();
        }
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

namespace {

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void putI32(std::vector<std::uint8_t>& out, std::int32_t value) {
    putU32(out, static_cast<std::uint32_t>(value));
}

void putVarString(std::vector<std::uint8_t>& out, const std::string& text) {
    std::size_t length = text.size();
    do {
        const auto low = static_cast<std::uint8_t>(length & 0x7F);
        length >>= 7;
        out.push_back(static_cast<std::uint8_t>(low | (length != 0 ? 0x80 : 0)));
    } while (length != 0);
    out.insert(out.end(), text.begin(), text.end());
}

// Reads a text and checks its length was written the shortest way, so it writes back the same.
bool readVarString(BinaryReader& r, std::string& text) {
    const std::size_t before = r.remaining();
    text = r.varString();
    std::vector<std::uint8_t> written;
    putVarString(written, text);
    return before - r.remaining() == written.size();
}

template <std::size_t N>
void readHeader(BinaryReader& r, std::array<std::uint8_t, N>& header) {
    const auto bytes = r.bytes(N);
    std::copy(bytes.begin(), bytes.end(), header.begin());
}

} // namespace

std::optional<MapRecord> readMapRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        MapRecord out;
        readHeader(r, out.header);
        if (r.u32() != 0 || r.u32() != kWrittenVersion || r.u32() != kMagic) {
            return std::nullopt;
        }
        out.objects.resize(r.u32Count(8));
        for (auto& [id, type] : out.objects) {
            id = r.i32();
            type = r.i32();
        }
        out.waits.resize(r.u32Count(4));
        for (std::uint32_t& wait : out.waits) {
            wait = r.u32();
        }
        out.waitsWithObject = r.i32();
        out.buildBuyBlockers.resize(r.u32Count(4));
        for (std::int32_t& id : out.buildBuyBlockers) {
            id = r.i32();
        }
        if (r.u32() != 0 || !readVarString(r, out.text)) {
            return std::nullopt;
        }
        // Each subscription takes at least its message and count, each subscriber 5 bytes.
        out.subscriptions.resize(r.u32Count(8));
        for (Subscription& s : out.subscriptions) {
            s.message = r.u32();
            s.subscribers.resize(r.u32Count(5));
            for (Subscriber& sub : s.subscribers) {
                sub.object = r.i32();
                if (!readVarString(r, sub.tree)) {
                    return std::nullopt;
                }
            }
        }
        if (r.remaining() != 0) {
            return std::nullopt;
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeMapRecord(const MapRecord& record) {
    std::vector<std::uint8_t> out(record.header.begin(), record.header.end());
    putU32(out, 0);
    putU32(out, kWrittenVersion);
    putU32(out, kMagic);
    putU32(out, static_cast<std::uint32_t>(record.objects.size()));
    for (const auto& [id, type] : record.objects) {
        putI32(out, id);
        putI32(out, type);
    }
    putU32(out, static_cast<std::uint32_t>(record.waits.size()));
    for (const std::uint32_t wait : record.waits) {
        putU32(out, wait);
    }
    putI32(out, record.waitsWithObject);
    putU32(out, static_cast<std::uint32_t>(record.buildBuyBlockers.size()));
    for (const std::int32_t id : record.buildBuyBlockers) {
        putI32(out, id);
    }
    putU32(out, 0);
    putVarString(out, record.text);
    putU32(out, static_cast<std::uint32_t>(record.subscriptions.size()));
    for (const Subscription& s : record.subscriptions) {
        putU32(out, s.message);
        putU32(out, static_cast<std::uint32_t>(s.subscribers.size()));
        for (const Subscriber& sub : s.subscribers) {
            putI32(out, sub.object);
            putVarString(out, sub.tree);
        }
    }
    return out;
}

std::optional<GroupsRecord> readGroupsRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        GroupsRecord out;
        readHeader(r, out.header);
        if (r.u32() != kGroupsWrittenVersion) {
            return std::nullopt;
        }
        out.groups.resize(r.u32Count(8));
        for (auto& [key, objects] : out.groups) {
            key = r.i32();
            objects.resize(r.u32Count(2));
            for (std::uint16_t& object : objects) {
                object = r.u16();
            }
        }
        if (r.remaining() != 0) {
            return std::nullopt;
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeGroupsRecord(const GroupsRecord& record) {
    std::vector<std::uint8_t> out(record.header.begin(), record.header.end());
    putU32(out, kGroupsWrittenVersion);
    putU32(out, static_cast<std::uint32_t>(record.groups.size()));
    for (const auto& [key, objects] : record.groups) {
        putI32(out, key);
        putU32(out, static_cast<std::uint32_t>(objects.size()));
        for (const std::uint16_t object : objects) {
            out.push_back(static_cast<std::uint8_t>(object));
            out.push_back(static_cast<std::uint8_t>(object >> 8));
        }
    }
    return out;
}

} // namespace ts2::objm
