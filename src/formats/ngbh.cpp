#include "formats/ngbh.hpp"

#include "formats/binary_reader.hpp"

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

} // namespace ts2::ngbh
