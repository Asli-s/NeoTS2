#include "formats/datatables.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>

namespace ts2::datatables {

namespace {

constexpr std::uint32_t kMaxCount = 100000;
constexpr int kMaxDepth = 32;  // nested tables, against bad data (the game sets no limit)

// A count of elements that each take at least `minBytes`.
std::size_t count(BinaryReader& r, std::size_t minBytes) {
    const std::size_t n = r.u32Count(minBytes);
    if (n > kMaxCount) {
        throw std::runtime_error("data tables: bad count " + std::to_string(n));
    }
    return n;
}

std::vector<Entry> readPairs(BinaryReader& r, int depth);

Value readValue(BinaryReader& r, int depth) {
    Value v;
    const std::uint8_t type = r.u8();
    switch (type) {
    case 0: v.type = Value::Type::Float; v.f = std::bit_cast<float>(r.u32()); break;
    case 1: v.type = Value::Type::Int; v.i = r.i32(); break;
    case 2: v.type = Value::Type::Unsigned; v.u = r.u32(); break;
    case 3: v.type = Value::Type::Bool; v.b = r.u32() != 0; break;
    case 4: v.type = Value::Type::String; v.s = r.text(count(r, 1)); break;
    case 5:
        if (depth >= kMaxDepth) {
            throw std::runtime_error("data tables: tables nested too deeply");
        }
        v.type = Value::Type::Table;
        v.table = readPairs(r, depth + 1);
        break;
    default:
        throw std::runtime_error("data tables: unknown value type " + std::to_string(type));
    }
    return v;
}

std::vector<Entry> readPairs(BinaryReader& r, int depth) {
    // Grown as pairs are read, so memory follows the bytes actually there: a nested table can
    // claim a large count while its parent's count is still being read.
    const std::size_t n = count(r, 10);  // two values of 5 bytes or more
    std::vector<Entry> pairs;
    pairs.reserve(std::min<std::size_t>(n, 256));
    for (std::size_t i = 0; i < n; ++i) {
        Value key = readValue(r, depth);
        Value value = readValue(r, depth);
        pairs.push_back({std::move(key), std::move(value)});
    }
    return pairs;
}

} // namespace

DataTables parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    DataTables d;
    d.version = r.u32();
    d.tables.resize(count(r, 12));  // id, name length, value count
    for (auto& t : d.tables) {
        t.id = r.u32();
        t.name = r.text(count(r, 1));
        t.values = readPairs(r, 0);
    }
    d.extraBytes = r.remaining();
    return d;
}

} // namespace ts2::datatables
