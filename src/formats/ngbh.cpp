#include "formats/ngbh.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <stdexcept>
#include <string>

namespace ts2::ngbh {

namespace {

constexpr std::uint32_t kMaxCount = 100000;

std::size_t count(BinaryReader& r) {
    const std::int32_t n = r.i32();
    if (n < 0 || static_cast<std::uint32_t>(n) > kMaxCount) {
        throw std::runtime_error("ngbh: bad count " + std::to_string(n));
    }
    return static_cast<std::size_t>(n);
}

std::vector<xobj::Token> readTokens(BinaryReader& r, std::uint32_t version) {
    std::vector<xobj::Token> tokens(count(r));
    for (auto& t : tokens) {
        t = xobj::readToken(r, version);
    }
    return tokens;
}

Inventory readInventory(BinaryReader& r, std::uint32_t version) {
    Inventory inv;
    if (version > 0xBD) {
        inv.version = r.u32();
    }
    inv.counted = readTokens(r, version);
    // The game gives counted tokens at least two properties: a count of 1 and a 0.
    for (auto& t : inv.counted) {
        if (t.properties.empty()) {
            t.properties.push_back(1);
        }
        if (t.properties.size() < 2) {
            t.properties.push_back(0);
        }
    }
    inv.singular = readTokens(r, version);
    return inv;
}

std::map<std::int32_t, Inventory> readInventories(BinaryReader& r, std::uint32_t version) {
    std::map<std::int32_t, Inventory> inventories;
    const std::size_t n = count(r);
    for (std::size_t i = 0; i < n; ++i) {
        const std::int32_t id = r.i32();
        inventories[id] = readInventory(r, version);
    }
    return inventories;
}

} // namespace

Neighborhood parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Neighborhood hood;
    r.u32();  // the type, 'NGBH'
    hood.version = r.u32();
    r.u32();
    if (hood.version > 0x6A) {
        r.u32();
        r.u32();
    }
    if (hood.version > 0x6D) {
        const std::size_t length = r.u32();
        const auto bytes = r.bytes(length);
        hood.terrain.assign(bytes.begin(), bytes.end());
    }
    const std::size_t valueCount = hood.version < 0x14 ? 1 : 16;
    for (std::size_t i = 0; i < valueCount; ++i) {
        hood.values.push_back(r.i16());
    }
    if (hood.version > 0x65) {
        hood.global = readInventory(r, hood.version);
        hood.lots = readInventories(r, hood.version);
        hood.families = readInventories(r, hood.version);
        hood.neighbors = readInventories(r, hood.version);
    }
    if (hood.version > 0x6E) {
        r.u8();
    }
    if (hood.version >= 0x70) {
        r.u32();
    }
    hood.extraBytes = r.remaining();
    return hood;
}

namespace {

// An inventory as stored, with no properties added.
std::optional<Inventory> readStoredInventory(BinaryReader& r) {
    Inventory inv;
    inv.version = r.u32();
    if (inv.version != kInventoryVersion) {
        return std::nullopt;
    }
    for (auto* list : {&inv.counted, &inv.singular}) {
        // A token takes at least 18 bytes.
        list->resize(r.u32Count(18));
        for (auto& t : *list) {
            t = xobj::readToken(r, kWrittenVersion);
        }
    }
    return inv;
}

bool readKeyed(BinaryReader& r, std::vector<std::pair<std::int16_t, Inventory>>& out) {
    // An entry takes at least 16 bytes: its key, then an empty inventory.
    for (std::size_t n = r.u32Count(16); n > 0; --n) {
        const std::int32_t key = r.i32();
        // The game keeps the key in 16 bits and writes it widened.
        if (key != static_cast<std::int16_t>(key)) {
            return false;
        }
        auto inv = readStoredInventory(r);
        if (!inv) {
            return false;
        }
        out.emplace_back(static_cast<std::int16_t>(key), std::move(*inv));
    }
    return true;
}

void writeInventory(BinaryWriter& w, const Inventory& inv) {
    w.u32(kInventoryVersion);
    for (const auto* list : {&inv.counted, &inv.singular}) {
        w.count(list->size());
        for (const auto& t : *list) {
            w.bytes(xobj::writeToken(t));
        }
    }
}

void writeKeyed(BinaryWriter& w, const std::vector<std::pair<std::int16_t, Inventory>>& list) {
    w.count(list.size());
    for (const auto& [key, inv] : list) {
        w.i32(key);
        writeInventory(w, inv);
    }
}

} // namespace

std::optional<Record> readRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.u32() != kType || r.u32() != kWrittenVersion) {
            return std::nullopt;
        }
        Record record;
        record.value0 = r.u32();
        record.value1 = r.u32();
        record.value2 = r.u32();
        const auto terrain = r.bytes(r.u32());
        record.terrain.assign(terrain.begin(), terrain.end());
        for (std::int16_t& v : record.values) {
            v = r.i16();
        }
        auto global = readStoredInventory(r);
        if (!global || !readKeyed(r, record.lots) || !readKeyed(r, record.families) || !readKeyed(r, record.neighbors)) {
            return std::nullopt;
        }
        record.global = std::move(*global);
        record.flag = r.u8();
        record.value3 = r.u32();
        if (r.remaining() != 0) {
            return std::nullopt;
        }
        return record;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeRecord(const Record& record) {
    BinaryWriter w;
    w.u32(kType);
    w.u32(kWrittenVersion);
    w.u32(record.value0);
    w.u32(record.value1);
    w.u32(record.value2);
    w.fullText(record.terrain);
    for (const std::int16_t v : record.values) {
        w.u16(static_cast<std::uint16_t>(v));
    }
    writeInventory(w, record.global);
    writeKeyed(w, record.lots);
    writeKeyed(w, record.families);
    writeKeyed(w, record.neighbors);
    w.u8(record.flag);
    w.u32(record.value3);
    return w.take();
}

} // namespace ts2::ngbh
