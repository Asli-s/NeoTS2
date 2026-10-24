#include "sim/lua_internals.hpp"

namespace ts2::sim::lua_detail {

namespace {

// Tables nest at most as deep as saved data tables may (a table holding itself stops here too).
constexpr int kMaxTableDepth = 32;

// One value read from a script holds at most this many values in all, counting a table each time
// it is reached (a limit of ours): a table holding the same table twice, 30 levels down, would
// otherwise copy to billions of values outside the script memory limit.
constexpr std::size_t kMaxCopiedValues = 1 << 20;

// Room on the Lua stack for one more level of a nested table, or the script stops.
void needTableLevel(lua_State* L, int depth) {
    if (depth >= kMaxTableDepth || lua_checkstack(L, 3) == 0) {
        scriptCheckError(L, "Data table nested too deep.");
    }
}

DataValue toDataValue(lua_State* L, int index, bool whole, int depth, std::size_t& valuesLeft) {
    if (index < 0) {
        index = lua_gettop(L) + index + 1;
    }
    if (valuesLeft == 0) {
        scriptCheckError(L, "Data table too large.");
    }
    --valuesLeft;
    switch (lua_type(L, index)) {
    case LUA_TNUMBER:
        return whole ? DataValue::ofInt(argInt(L, index))
                     : DataValue::ofFloat(singleOf(lua_tonumber(L, index)));
    case LUA_TBOOLEAN:
        return DataValue::ofBool(lua_toboolean(L, index) != 0);
    case LUA_TSTRING:
        return DataValue::ofString(lua_tostring(L, index));
    case LUA_TTABLE: {
        needTableLevel(L, depth);
        DataValue::Table table;
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            DataValue value = toDataValue(L, -1, false, depth + 1, valuesLeft);
            table[toDataValue(L, -2, true, depth + 1, valuesLeft)] = std::move(value);
            lua_pop(L, 1);
        }
        return DataValue::ofTable(std::move(table));
    }
    default:
        scriptCheckError(L, "Unsupported type for data table! Index: %d == %s.", index,
                         lua_typename(L, lua_type(L, index)));
    }
}

// Reads a Lua value for a table. Numbers become whole numbers (truncated) when `whole` is set and
// single-precision ones otherwise. Inside a nested table keys are whole and values are not.
DataValue toDataValue(lua_State* L, int index, bool whole) {
    std::size_t valuesLeft = kMaxCopiedValues;
    return toDataValue(L, index, whole, 0, valuesLeft);
}

void pushDataValue(lua_State* L, const DataValue& v, int depth = 0) {
    switch (v.kind) {
    case DataValue::Kind::Float: lua_pushnumber(L, v.f); break;
    case DataValue::Kind::Int: lua_pushnumber(L, v.i); break;
    case DataValue::Kind::Unsigned: lua_pushnumber(L, static_cast<lua_Number>(v.u)); break;
    case DataValue::Kind::Bool: lua_pushboolean(L, v.b); break;
    case DataValue::Kind::String: lua_pushstring(L, v.s.c_str()); break;
    case DataValue::Kind::Table:
        needTableLevel(L, depth);
        lua_newtable(L);
        for (const auto& [key, value] : *v.table) {
            pushDataValue(L, key, depth + 1);
            pushDataValue(L, value, depth + 1);
            lua_settable(L, -3);
        }
        break;
    }
}

DataTables& objectTables(lua_State* L, std::int16_t id) {
    ScriptCall& c = call(L);
    SimObject* o = c.world ? c.world->find(id) : nullptr;
    if (o == nullptr) {
        scriptCheckError(L, "Could not find object: %d", id);
    }
    return o->dataTables;
}

DataTables& neighborTables(lua_State* L, std::int16_t id) {
    ScriptCall& c = call(L);
    if (c.world == nullptr || c.world->neighbor(id) == nullptr) {
        gameError(L, kErrorMissingNeighbor);
    }
    return c.world->neighborDataTables(id);
}

// The tables of the instance a method is called on (self, argument 1).
DataTables& selfTables(lua_State* L) {
    const auto* self = selfInstance(L);
    if (self == nullptr) {
        scriptCheckError(L, "Data table method called without its table");
    }
    const auto id = static_cast<std::int16_t>(self->id);
    return self->neighbor ? neighborTables(L, id) : objectTables(L, id);
}

std::uint32_t tableIdArg(lua_State* L) {
    return static_cast<std::uint32_t>(argInt(L, 2));
}

DataTable& existingTable(lua_State* L) {
    DataTables& tables = selfTables(L);
    const std::uint32_t id = tableIdArg(L);
    const auto it = tables.find(id);
    if (it == tables.end()) {
        scriptCheckError(L, "Table with id %d does not exist", static_cast<int>(id));
    }
    return it->second;
}

// getTableKeys / getTableValues: an array of the table's keys or values.
int dataTableList(lua_State* L, bool keys) {
    const DataTable& table = existingTable(L);
    lua_newtable(L);
    int n = 0;
    for (const auto& [key, value] : table.values) {
        lua_pushnumber(L, ++n);
        pushDataValue(L, keys ? key : value);
        lua_settable(L, -3);
    }
    return 1;
}

// A value of one of these tables, or nil when the table or key is missing.
int pushTableValue(lua_State* L, const DataTables& tables) {
    const auto it = tables.find(tableIdArg(L));
    const DataValue key = toDataValue(L, 3, true);
    const auto value = it != tables.end() ? it->second.values.find(key) : DataValue::Table::const_iterator{};
    if (it == tables.end() || value == it->second.values.end()) {
        lua_pushnil(L);
    } else {
        pushDataValue(L, value->second);
    }
    return 1;
}

int dataTableSet(lua_State* L, bool whole) {
    DataValue key = toDataValue(L, 3, true);
    DataValue value = toDataValue(L, 4, whole);
    existingTable(L).values[std::move(key)] = std::move(value);
    return 0;
}

} // namespace

int newObjectDataTable(lua_State* L) {
    const std::int16_t id = argShort(L, 1);
    objectTables(L, id);
    return pushInstance(L, "ObjectDataTable", id);
}

int newNeighborDataTable(lua_State* L) {
    const std::int16_t id = argShort(L, 1);
    neighborTables(L, id);
    pushInstance(L, "NeighborDataTable", id);
    static_cast<Instance*>(lua_touserdata(L, -1))->neighbor = true;
    return 1;
}

int dataTableNew(lua_State* L) {
    DataTables& tables = selfTables(L);
    const std::uint32_t id = tableIdArg(L);
    const char* name = lua_tostring(L, 3);
    if (!tables.try_emplace(id, DataTable{name ? name : "", {}}).second) {
        return scriptCheckError(L, "Unable to create a new data table with id %d--table already exists!",
                                static_cast<int>(id));
    }
    return 0;
}

int dataTableRemove(lua_State* L) {
    lua_pushboolean(L, selfTables(L).erase(tableIdArg(L)) != 0);
    return 1;
}

int dataTableClear(lua_State* L) {
    DataTables& tables = selfTables(L);
    const auto it = tables.find(tableIdArg(L));
    if (it != tables.end()) {
        it->second.values.clear();
    }
    lua_pushboolean(L, it != tables.end());
    return 1;
}

int dataTableHas(lua_State* L) {
    lua_pushboolean(L, selfTables(L).contains(tableIdArg(L)));
    return 1;
}

int dataTableRemoveAll(lua_State* L) {
    selfTables(L).clear();
    return 0;
}

int dataTableIds(lua_State* L) {
    const DataTables& tables = selfTables(L);
    lua_newtable(L);
    int n = 0;
    for (const auto& entry : tables) {
        lua_pushnumber(L, ++n);
        lua_pushnumber(L, static_cast<std::int32_t>(entry.first));
        lua_settable(L, -3);
    }
    return 1;
}

int dataTableKeys(lua_State* L) { return dataTableList(L, true); }

int dataTableValues(lua_State* L) { return dataTableList(L, false); }

int dataTableCopy(lua_State* L) {
    const DataTable& table = existingTable(L);
    lua_newtable(L);
    for (const auto& [key, value] : table.values) {
        pushDataValue(L, key);
        pushDataValue(L, value);
        lua_settable(L, -3);
    }
    return 1;
}

int dataTableSetFromCopy(lua_State* L) {
    const DataValue copy = toDataValue(L, 3, false);
    if (copy.kind != DataValue::Kind::Table) {
        return scriptCheckError(L, "Unsupported type for data table copy");
    }
    existingTable(L).values = *copy.table;
    return 0;
}

int dataTableGetValue(lua_State* L) { return pushTableValue(L, selfTables(L)); }

int dataTableSetValue(lua_State* L) { return dataTableSet(L, false); }

int dataTableSetValueAsInt(lua_State* L) { return dataTableSet(L, true); }

int dataTableRemoveValue(lua_State* L) {
    const DataValue key = toDataValue(L, 3, true);
    existingTable(L).values.erase(key);
    return 0;
}

int dataTableHasValue(lua_State* L) {
    const DataTables& tables = selfTables(L);
    const auto it = tables.find(tableIdArg(L));
    const DataValue key = toDataValue(L, 3, true);
    lua_pushboolean(L, it != tables.end() && it->second.values.contains(key));
    return 1;
}

int dataTableGetName(lua_State* L) {
    lua_pushstring(L, existingTable(L).name.c_str());
    return 1;
}

int dataTableSetName(lua_State* L) {
    const char* name = lua_tostring(L, 3);
    existingTable(L).name = name ? name : "";
    return 0;
}

// nDataTable.GetObjectTableValue(objectId, table, key) / GetNeighborTableValue(neighborId, table, key).
int getObjectTableValue(lua_State* L) { return pushTableValue(L, objectTables(L, argShort(L, 1))); }

int getNeighborTableValue(lua_State* L) { return pushTableValue(L, neighborTables(L, argShort(L, 1))); }

} // namespace ts2::sim::lua_detail
