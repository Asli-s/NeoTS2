#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ts2::datatables {

struct Entry;

// A value in a script data table, as saved: a type byte, then the value.
struct Value {
    enum class Type : std::uint8_t { Float = 0, Int = 1, Unsigned = 2, Bool = 3, String = 4, Table = 5 };
    Type type = Type::Int;
    float f = 0.0f;
    std::int32_t i = 0;
    std::uint32_t u = 0;
    bool b = false;
    std::string s;
    std::vector<Entry> table;
};

// A key and its value. Not a std::pair: that needs both types complete, and Value holds these.
struct Entry {
    Value key;
    Value value;
};

struct Table {
    std::uint32_t id = 0;
    std::string name;
    std::vector<Entry> values;
};

// The script data tables of one object (saved in its lot, instance = object id) or one neighbor
// (saved in the neighborhood, instance = neighbor id).
struct DataTables {
    std::uint32_t version = 0;
    std::vector<Table> tables;
    std::size_t extraBytes = 0;
};

DataTables parse(std::span<const std::uint8_t> data);

} // namespace ts2::datatables
