#include "formats/objt.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::objt {

namespace {
constexpr std::uint32_t kMagic = 0x6F626A74;
}

const Entry* ObjectTypeTable::find(std::uint16_t index) const {
    for (const auto& e : entries) {
        if (e.index == index) {
            return &e;
        }
    }
    return nullptr;
}

ObjectTypeTable parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(64);
    if (r.u32() != kMagic) {
        throw std::runtime_error("objt: bad magic");
    }
    ObjectTypeTable table;
    table.version = r.u32();
    table.unknown = r.u32();

    while (true) {
        const std::uint32_t guid = r.u32();
        if (guid == 0 && r.remaining() == 0) {
            break;
        }
        Entry e;
        e.guid = guid;
        for (auto& word : e.unknown) {
            word = r.u16();
        }
        e.index = r.u16();
        e.kind = r.u16();
        const auto name = r.bytes(r.u8());
        e.name.assign(name.begin(), name.end());
        e.flag = r.u32();
        e.secondGuid = r.u32();
        table.entries.push_back(std::move(e));
    }
    return table;
}

} // namespace ts2::objt

namespace ts2::objt {

std::vector<std::uint8_t> write(const ObjectTypeTable& table, std::span<const std::uint8_t> name) {
    if (name.size() != 64) {
        throw std::runtime_error("objt: a name is 64 bytes");
    }
    std::vector<std::uint8_t> out(name.begin(), name.end());
    auto put = [&](std::uint32_t v, int bytes) {
        for (int s = 0; s < bytes * 8; s += 8) {
            out.push_back(static_cast<std::uint8_t>(v >> s));
        }
    };
    put(kMagic, 4);
    put(table.version, 4);
    put(table.unknown, 4);
    for (const auto& e : table.entries) {
        if (e.name.size() > 255) {
            throw std::runtime_error("objt: a type's name is too long");
        }
        put(e.guid, 4);
        for (const auto word : e.unknown) {
            put(word, 2);
        }
        put(e.index, 2);
        put(e.kind, 2);
        out.push_back(static_cast<std::uint8_t>(e.name.size()));
        out.insert(out.end(), e.name.begin(), e.name.end());
        put(e.flag, 4);
        put(e.secondGuid, 4);
    }
    put(0, 4);  // the table ends with a type 0
    return out;
}

} // namespace ts2::objt
