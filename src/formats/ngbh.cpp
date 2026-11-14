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

// An inventory's tokens as saved.
Inventory readInventory(BinaryReader& r, std::uint32_t version) {
    Inventory inv;
    if (version > 0xBD) {
        inv.version = r.u32();
    }
    inv.counted = readTokens(r, version);
    inv.singular = readTokens(r, version);
    return inv;
}

std::vector<std::pair<std::int32_t, Inventory>> readInventories(BinaryReader& r, std::uint32_t version) {
    std::vector<std::pair<std::int32_t, Inventory>> inventories;
    const std::size_t n = count(r);
    for (std::size_t i = 0; i < n; ++i) {
        const std::int32_t id = r.i32();
        inventories.emplace_back(id, readInventory(r, version));
    }
    return inventories;
}

void writeTokens(BinaryWriter& w, const std::vector<xobj::Token>& tokens, std::uint32_t version) {
    w.i32(static_cast<std::int32_t>(tokens.size()));
    for (const auto& t : tokens) {
        xobj::writeToken(w, t, version);
    }
}

void writeInventory(BinaryWriter& w, const Inventory& inv, std::uint32_t version) {
    if (version > 0xBD) {
        w.u32(inv.version);
    }
    writeTokens(w, inv.counted, version);
    writeTokens(w, inv.singular, version);
}

void writeInventories(BinaryWriter& w, const std::vector<std::pair<std::int32_t, Inventory>>& inventories, std::uint32_t version) {
    w.i32(static_cast<std::int32_t>(inventories.size()));
    for (const auto& [id, inv] : inventories) {
        w.i32(id);
        writeInventory(w, inv, version);
    }
}

// An inventory as the game uses it: counted tokens have at least two properties, a count of 1 and a 0.
Inventory used(Inventory inv) {
    for (auto& t : inv.counted) {
        if (t.properties.empty()) {
            t.properties.push_back(1);
        }
        if (t.properties.size() < 2) {
            t.properties.push_back(0);
        }
    }
    return inv;
}

// By id, a later inventory of an id replacing an earlier one, as the game's reading does.
std::map<std::int32_t, Inventory> byId(const std::vector<std::pair<std::int32_t, Inventory>>& inventories) {
    std::map<std::int32_t, Inventory> out;
    for (const auto& [id, inv] : inventories) {
        out[id] = used(inv);
    }
    return out;
}

} // namespace

Raw readRaw(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Raw raw;
    raw.type = r.u32();
    raw.version = r.u32();
    raw.value0 = r.u32();
    if (raw.version > 0x6A) {
        raw.value1 = r.u32();
        raw.value2 = r.u32();
    }
    if (raw.version > 0x6D) {
        const std::size_t length = r.u32();
        const auto bytes = r.bytes(length);
        raw.terrain.assign(bytes.begin(), bytes.end());
    }
    const std::size_t valueCount = raw.version < 0x14 ? 1 : 16;
    for (std::size_t i = 0; i < valueCount; ++i) {
        raw.values.push_back(r.i16());
    }
    if (raw.version > 0x65) {
        raw.global = readInventory(r, raw.version);
        raw.lots = readInventories(r, raw.version);
        raw.families = readInventories(r, raw.version);
        raw.neighbors = readInventories(r, raw.version);
    }
    if (raw.version > 0x6E) {
        raw.lastByte = r.u8();
    }
    if (raw.version >= 0x70) {
        raw.lastValue = r.u32();
    }
    const auto tail = r.bytes(r.remaining());
    raw.tail.assign(tail.begin(), tail.end());
    return raw;
}

std::vector<std::uint8_t> encode(const Raw& raw) {
    BinaryWriter w;
    w.u32(raw.type);
    w.u32(raw.version);
    w.u32(raw.value0);
    if (raw.version > 0x6A) {
        w.u32(raw.value1);
        w.u32(raw.value2);
    }
    if (raw.version > 0x6D) {
        w.u32(static_cast<std::uint32_t>(raw.terrain.size()));
        w.text(raw.terrain);
    }
    const std::size_t valueCount = raw.version < 0x14 ? 1 : 16;
    for (std::size_t i = 0; i < valueCount; ++i) {
        w.i16(i < raw.values.size() ? raw.values[i] : std::int16_t{0});
    }
    if (raw.version > 0x65) {
        writeInventory(w, raw.global, raw.version);
        writeInventories(w, raw.lots, raw.version);
        writeInventories(w, raw.families, raw.version);
        writeInventories(w, raw.neighbors, raw.version);
    }
    if (raw.version > 0x6E) {
        w.u8(raw.lastByte);
    }
    if (raw.version >= 0x70) {
        w.u32(raw.lastValue);
    }
    w.bytes(raw.tail);
    return w.take();
}

Neighborhood parse(std::span<const std::uint8_t> data) {
    const Raw raw = readRaw(data);
    Neighborhood hood;
    hood.version = raw.version;
    hood.terrain = raw.terrain;
    hood.values = raw.values;
    if (raw.version > 0x65) {
        hood.global = used(raw.global);
        hood.lots = byId(raw.lots);
        hood.families = byId(raw.families);
        hood.neighbors = byId(raw.neighbors);
    }
    hood.extraBytes = raw.tail.size();
    return hood;
}

} // namespace ts2::ngbh
