#pragma once

#include "formats/datatables.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace ts2::sim {

// A value kept in a script data table: a number (single-precision, whole or unsigned whole), a
// boolean, a string, or a nested table. Scripts only make the first two kinds of number; saved
// tables may hold the third.
struct DataValue {
    enum class Kind : std::uint8_t { Float, Int, Unsigned, Bool, String, Table };
    using Table = std::map<DataValue, DataValue>;

    Kind kind = Kind::Int;
    float f = 0.0f;
    std::int32_t i = 0;
    std::uint32_t u = 0;
    bool b = false;
    std::string s;
    std::shared_ptr<const Table> table;

    static DataValue ofFloat(float v) { DataValue d; d.kind = Kind::Float; d.f = v; return d; }
    static DataValue ofInt(std::int32_t v) { DataValue d; d.kind = Kind::Int; d.i = v; return d; }
    static DataValue ofBool(bool v) { DataValue d; d.kind = Kind::Bool; d.b = v; return d; }
    static DataValue ofString(std::string v) { DataValue d; d.kind = Kind::String; d.s = std::move(v); return d; }
    static DataValue ofTable(Table v) {
        DataValue d;
        d.kind = Kind::Table;
        d.table = std::make_shared<const Table>(std::move(v));
        return d;
    }

    // Orders keys: by kind, then by value (nested tables by identity).
    friend bool operator<(const DataValue& a, const DataValue& b) {
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        switch (a.kind) {
        case Kind::Float: return a.f < b.f;
        case Kind::Int: return a.i < b.i;
        case Kind::Unsigned: return a.u < b.u;
        case Kind::Bool: return a.b < b.b;
        case Kind::String: return a.s < b.s;
        case Kind::Table: return a.table < b.table;
        }
        return false;
    }
};

// The named key-value tables scripts keep on an object or a neighbor, by table id.
struct DataTable {
    std::string name;
    DataValue::Table values;
};
using DataTables = std::map<std::uint32_t, DataTable>;

inline DataValue toDataValue(const datatables::Value& saved) {
    using Type = datatables::Value::Type;
    DataValue v;
    switch (saved.type) {
    case Type::Float: return DataValue::ofFloat(saved.f);
    case Type::Int: return DataValue::ofInt(saved.i);
    case Type::Unsigned: v.kind = DataValue::Kind::Unsigned; v.u = saved.u; return v;
    case Type::Bool: return DataValue::ofBool(saved.b);
    case Type::String: return DataValue::ofString(saved.s);
    case Type::Table: {
        DataValue::Table table;
        for (const auto& [key, value] : saved.table) {
            table[toDataValue(key)] = toDataValue(value);
        }
        return DataValue::ofTable(std::move(table));
    }
    }
    return v;
}

inline DataTables toDataTables(const datatables::DataTables& saved) {
    DataTables tables;
    for (const auto& t : saved.tables) {
        DataTable& table = tables[t.id];
        table.name = t.name;
        for (const auto& [key, value] : t.values) {
            table.values[toDataValue(key)] = toDataValue(value);
        }
    }
    return tables;
}

} // namespace ts2::sim
