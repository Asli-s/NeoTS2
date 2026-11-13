#include "formats/objt.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

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
        e.name = r.varString();
        e.flag = r.u32();
        e.secondGuid = r.u32();
        table.entries.push_back(std::move(e));
    }
    return table;
}

std::vector<std::uint8_t> write(const ObjectTypeTable& table) {
    BinaryWriter w;
    w.zeros(64);
    w.u32(kMagic);
    w.u32(kWrittenVersion);
    w.u32(table.unknown);
    for (const Entry& e : table.entries) {
        w.u32(e.guid);
        for (const std::uint16_t word : e.unknown) {
            w.u16(word);
        }
        w.u16(e.index);
        w.u16(e.kind);
        w.text(e.name);
        w.u32(e.flag);
        w.u32(e.secondGuid);
    }
    w.u32(0);
    return w.take();
}

} // namespace ts2::objt
