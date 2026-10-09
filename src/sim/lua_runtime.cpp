#include "sim/lua_runtime.hpp"

#include "sim/code_source.hpp"
#include "sim/world.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ts2::sim {

namespace {

// Registry keys of the ScriptCall the running script works on and of the runtime itself.
char kCallKey;
char kRuntimeKey;

constexpr std::int16_t kErrorTempOutOfRange = 2;
constexpr std::int16_t kErrorParamOutOfRange = 8;
constexpr std::int16_t kErrorLocalOutOfRange = 0x33;
constexpr std::int16_t kErrorScriptCheck = 0x9B;

// The script functions below stop a script by throwing ScriptStop; guarded() catches it and only
// then calls lua_error (a longjmp), once no C++ object is left alive to be skipped over.
struct ScriptStop {
    char message[256];
};

[[noreturn]] void stopScript(const char* message) {
    ScriptStop stop{};
    std::snprintf(stop.message, sizeof stop.message, "%s", message);
    throw stop;
}

int guarded(lua_State* L) {
    const auto function = reinterpret_cast<lua_CFunction>(lua_touserdata(L, lua_upvalueindex(1)));
    char message[sizeof(ScriptStop::message)];
    try {
        return function(L);
    } catch (const ScriptStop& stop) {
        std::memcpy(message, stop.message, sizeof message);
    }
    lua_pushstring(L, message);
    return lua_error(L);
}

void pushGuarded(lua_State* L, lua_CFunction function) {
    lua_pushlightuserdata(L, reinterpret_cast<void*>(function));
    lua_pushcclosure(L, guarded, 1);
}

ScriptCall& call(lua_State* L) {
    lua_pushlightuserdata(L, &kCallKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* c = static_cast<ScriptCall*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *c;
}

// Lua numbers as the game's script functions read them.
std::int16_t argShort(lua_State* L, int i) {
    return static_cast<std::int16_t>(static_cast<int>(lua_tonumber(L, i)));
}
std::uint32_t argUnsigned(lua_State* L, int i) {
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(lua_tonumber(L, i)));
}
void pushUnsigned(lua_State* L, std::uint32_t value) {
    lua_pushnumber(L, static_cast<lua_Number>(value));
}

int gameError(lua_State* L, std::int16_t code, const char* message = "Object Error running script!") {
    call(L).errorCode = code;
    stopScript(message);
}

// Stops the script at a script function (or a use of one) the engine doesn't have yet.
int stopUnsupported(lua_State* L, const char* what) {
    call(L).unsupported = what;
    stopScript("unsupported script function");
}

int setScriptReturnValue(lua_State* L) {
    call(L).returnValue = lua_toboolean(L, 1) != 0;
    return 0;
}

int getStackObjectId(lua_State* L) {
    lua_pushnumber(L, call(L).stackObject);
    return 1;
}

int getPrimitiveParameter(lua_State* L) {
    ScriptCall& c = call(L);
    const std::int16_t i = argShort(L, 1);
    if (i < 0 || i > 2) {
        return gameError(L, kErrorScriptCheck, "Tried to access primitive parameter outside of range [0,3]!");
    }
    if (!c.hasParams) {
        return gameError(L, kErrorScriptCheck, "Did not specify any parameters to pass in primitive!");
    }
    lua_pushnumber(L, c.params[static_cast<std::size_t>(i)]);
    return 1;
}

int getTemp(lua_State* L) {
    ScriptCall& c = call(L);
    const std::int16_t i = argShort(L, 1);
    if (i < 0 || i > 7) {
        return gameError(L, kErrorTempOutOfRange);
    }
    lua_pushnumber(L, c.temps ? (*c.temps)[static_cast<std::size_t>(i)] : 0);
    return 1;
}

int setTemp(lua_State* L) {
    ScriptCall& c = call(L);
    const std::int16_t i = argShort(L, 1);
    if (i < 0 || i > 7) {
        return gameError(L, kErrorTempOutOfRange);
    }
    if (c.temps) {
        (*c.temps)[static_cast<std::size_t>(i)] = argShort(L, 2);
    }
    return 0;
}

// Locals and tree parameters of the frame running the primitive.
int getFrameValue(lua_State* L, std::vector<std::int16_t>* values, std::int16_t error) {
    const std::int16_t i = argShort(L, 1);
    if (values == nullptr) {
        lua_pushnumber(L, 0);
        return 1;
    }
    if (i < 0 || static_cast<std::size_t>(i) >= values->size()) {
        return gameError(L, error);
    }
    lua_pushnumber(L, (*values)[static_cast<std::size_t>(i)]);
    return 1;
}

int setFrameValue(lua_State* L, std::vector<std::int16_t>* values, std::int16_t error) {
    const std::int16_t i = argShort(L, 1);
    if (values == nullptr) {
        return 0;
    }
    if (i < 0 || static_cast<std::size_t>(i) >= values->size()) {
        return gameError(L, error);
    }
    (*values)[static_cast<std::size_t>(i)] = argShort(L, 2);
    return 0;
}

int getLocal(lua_State* L) { return getFrameValue(L, call(L).locals, kErrorLocalOutOfRange); }
int setLocal(lua_State* L) { return setFrameValue(L, call(L).locals, kErrorLocalOutOfRange); }
int getTreeParameter(lua_State* L) { return getFrameValue(L, call(L).treeParams, kErrorParamOutOfRange); }
int setTreeParameter(lua_State* L) { return setFrameValue(L, call(L).treeParams, kErrorParamOutOfRange); }

int doNothing(lua_State*) { return 0; }

// Bits are numbered from 1; values are unsigned 32-bit.
int isBitSet(lua_State* L) {
    const std::uint32_t bit = (argUnsigned(L, 2) - 1) & 0x1F;
    lua_pushboolean(L, (argUnsigned(L, 1) >> bit) & 1);
    return 1;
}
int setBit(lua_State* L) {
    pushUnsigned(L, argUnsigned(L, 1) | 1u << ((argUnsigned(L, 2) - 1) & 0x1F));
    return 1;
}
int clearBit(lua_State* L) {
    pushUnsigned(L, argUnsigned(L, 1) & ~(1u << ((argUnsigned(L, 2) - 1) & 0x1F)));
    return 1;
}
int bitwiseAnd(lua_State* L) { pushUnsigned(L, argUnsigned(L, 1) & argUnsigned(L, 2)); return 1; }
int bitwiseOr(lua_State* L) { pushUnsigned(L, argUnsigned(L, 1) | argUnsigned(L, 2)); return 1; }
int bitwiseXOr(lua_State* L) { pushUnsigned(L, argUnsigned(L, 1) ^ argUnsigned(L, 2)); return 1; }
int bitShiftLeft(lua_State* L) { pushUnsigned(L, argUnsigned(L, 1) << (argUnsigned(L, 2) & 0x1F)); return 1; }
int bitShiftRight(lua_State* L) { pushUnsigned(L, argUnsigned(L, 1) >> (argUnsigned(L, 2) & 0x1F)); return 1; }

int createObjectGuid(lua_State* L) {
    pushUnsigned(L, argUnsigned(L, 1) << 16 | (argUnsigned(L, 2) & 0xFFFF));
    return 1;
}
int splitObjectGuid(lua_State* L) {
    const std::uint32_t guid = argUnsigned(L, 1);
    lua_pushnumber(L, guid >> 16);
    lua_pushnumber(L, guid & 0xFFFF);
    return 2;
}

// Rounds to the nearest whole number (halves to even) in single precision, as the game does.
int roundFloat(lua_State* L) {
    const float shifted = static_cast<float>(lua_tonumber(L, 1)) + 12582912.0f;
    std::int32_t bits = 0;
    std::memcpy(&bits, &shifted, sizeof(bits));
    lua_pushnumber(L, bits - 0x4B400000);
    return 1;
}

LuaRuntime& runtime(lua_State* L) {
    lua_pushlightuserdata(L, &kRuntimeKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* r = static_cast<LuaRuntime*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *r;
}

// nGameState.GetProperty(name): the property's value, nil when it isn't set.
int getProperty(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    auto& properties = runtime(L).properties;
    const auto it = name ? properties.find(name) : properties.end();
    if (it == properties.end()) {
        lua_pushnil(L);
        return 1;
    }
    if (const auto* b = std::get_if<bool>(&it->second)) lua_pushboolean(L, *b);
    else if (const auto* i = std::get_if<std::int32_t>(&it->second)) lua_pushnumber(L, *i);
    else if (const auto* u = std::get_if<std::uint32_t>(&it->second)) lua_pushnumber(L, *u);
    else if (const auto* f = std::get_if<float>(&it->second)) lua_pushnumber(L, *f);
    else lua_pushstring(L, std::get<std::string>(it->second).c_str());
    return 1;
}

// nGameState.SetProperty(name, value): a boolean or string as such; a number keeps the type of the
// property it replaces (float or unsigned), and is otherwise a whole number.
int setProperty(lua_State* L) {
    if (lua_gettop(L) != 2) {
        return gameError(L, kErrorScriptCheck, "Invalid number of parameters to nGameState.SetProperty()!");
    }
    if (!lua_isstring(L, 1) || (lua_type(L, 2) != LUA_TBOOLEAN && lua_type(L, 2) != LUA_TNUMBER &&
                                lua_type(L, 2) != LUA_TSTRING)) {
        return gameError(L, kErrorScriptCheck, "Could not retrieve valid type for value!");
    }
    LuaRuntime::Property value;
    auto& properties = runtime(L).properties;
    const std::string name = lua_tostring(L, 1);
    switch (lua_type(L, 2)) {
    case LUA_TBOOLEAN:
        value = lua_toboolean(L, 2) != 0;
        break;
    case LUA_TSTRING:
        value = std::string(lua_tostring(L, 2));
        break;
    default: {
        const lua_Number n = lua_tonumber(L, 2);
        const auto it = properties.find(name);
        if (it != properties.end() && std::holds_alternative<float>(it->second)) {
            value = static_cast<float>(n);
        } else if (it != properties.end() && std::holds_alternative<std::uint32_t>(it->second)) {
            value = static_cast<std::uint32_t>(static_cast<std::int64_t>(n));
        } else {
            value = static_cast<std::int32_t>(n);
        }
    }
    }
    properties[name] = std::move(value);
    return 0;
}

int installedPacksOrStop(lua_State* L, std::uint32_t& mask) {
    mask = runtime(L).installedPacks;
    if (mask == 0) {
        return stopUnsupported(L, "nGameState.IsEPInstalled without the install");
    }
    return 0;
}

// nGameState.IsEPInstalled(n): whether bit n of the installed packs is set.
int isEpInstalled(lua_State* L) {
    std::uint32_t mask = 0;
    installedPacksOrStop(L, mask);
    const std::uint32_t bit = 1u << (argUnsigned(L, 1) & 0x1F);
    lua_pushboolean(L, (mask & bit) == bit);
    return 1;
}

int getInstalledEps(lua_State* L) {
    std::uint32_t mask = 0;
    installedPacksOrStop(L, mask);
    pushUnsigned(L, mask);
    return 1;
}

// A function the game provides that the engine doesn't have yet: stops the script and says which.
// Called by Lua directly (not guarded); it keeps nothing alive across lua_error.
int unsupported(lua_State* L) {
    call(L).unsupported = lua_tostring(L, lua_upvalueindex(1));
    lua_pushstring(L, "unsupported script function");
    return lua_error(L);
}

struct Function {
    const char* name;
    lua_CFunction function;
};

// Defined with the world-facing functions further down.
int getRunningObjectId(lua_State* L);
int getRunningOnObjectId(lua_State* L);
int getObjectName(lua_State* L);
int getObjectGroupId(lua_State* L);
int getObjectAttribute(lua_State* L);
int setObjectAttribute(lua_State* L);
int getObjectSemiAttribute(lua_State* L);
int setObjectSemiAttribute(lua_State* L);
int getObjectIdInSlot(lua_State* L);
int getObjectDefinitionField(lua_State* L);
int getPersonMotive(lua_State* L);
int setPersonMotive(lua_State* L);
int getNeighborPersonData(lua_State* L);
int setNeighborPersonData(lua_State* L);
int setSimulatorGlobal(lua_State* L);
int getNumObjectArrays(lua_State* L);
int getObjectArrayCopy(lua_State* L);
int setObjectArray(lua_State* L);
int getSimulatorGlobal(lua_State* L);
int getConstant(lua_State* L);
int getNumConstants(lua_State* L);
int getConstantWithGuid(lua_State* L);
int getNumConstantsWithGuid(lua_State* L);
int getConstantsTableByGuid(lua_State* L);
int getObjectData(lua_State* L);
int setObjectData(lua_State* L);
int getPersonData(lua_State* L);
int setPersonData(lua_State* L);

constexpr Function kFunctions[] = {
    {"SetScriptReturnValue", setScriptReturnValue},
    {"GetStackObjectId", getStackObjectId},
    {"GetPrimitiveParameter", getPrimitiveParameter},
    {"GetTemp", getTemp},
    {"SetTemp", setTemp},
    {"GetLocal", getLocal},
    {"SetLocal", setLocal},
    {"GetTreeParameter", getTreeParameter},
    {"SetTreeParameter", setTreeParameter},
    {"print", doNothing},
    {"DumpLuaStack", doNothing},
    {"DebugPrint", doNothing},
    {"DebugPrintIf", doNothing},
    {"printif", doNothing},
    {"IsBitSet", isBitSet},
    {"SetBit", setBit},
    {"ClearBit", clearBit},
    {"IsFlagSet", isBitSet},
    {"SetFlag", setBit},
    {"ClearFlag", clearBit},
    {"BitwiseAnd", bitwiseAnd},
    {"BitwiseOr", bitwiseOr},
    {"BitwiseXOr", bitwiseXOr},
    {"BitShiftLeft", bitShiftLeft},
    {"BitShiftRight", bitShiftRight},
    {"CreateObjectGUID", createObjectGuid},
    {"SplitObjectGUID", splitObjectGuid},
    {"RoundFloat", roundFloat},
    {"GetSimulatorGlobal", getSimulatorGlobal},
    {"GetNumConstantsWithGuid", getNumConstantsWithGuid},
    {"GetConstant", getConstant},
    {"GetNumConstants", getNumConstants},
    {"GetConstantWithGuid", getConstantWithGuid},
    {"GetConstantsTableByGuid", getConstantsTableByGuid},
    {"GetObjectData", getObjectData},
    {"SetObjectData", setObjectData},
    {"GetPersonData", getPersonData},
    {"SetPersonData", setPersonData},
    {"GetRunningObjectId", getRunningObjectId},
    {"GetRunningOnObjectId", getRunningOnObjectId},
    {"GetObjectName", getObjectName},
    {"GetObjectGroupID", getObjectGroupId},
    {"GetObjectAttribute", getObjectAttribute},
    {"SetObjectAttribute", setObjectAttribute},
    {"GetObjectSemiAttribute", getObjectSemiAttribute},
    {"SetObjectSemiAttribute", setObjectSemiAttribute},
    {"GetObjectIdInSlot", getObjectIdInSlot},
    {"GetObjectDefinitionField", getObjectDefinitionField},
    {"GetPersonMotive", getPersonMotive},
    {"SetPersonMotive", setPersonMotive},
    {"GetNeighborPersonData", getNeighborPersonData},
    {"SetNeighborPersonData", setNeighborPersonData},
    {"SetSimulatorGlobal", setSimulatorGlobal},
    {"GetNumObjectArrays", getNumObjectArrays},
    {"GetObjectArrayCopy", getObjectArrayCopy},
    {"SetObjectArray", setObjectArray},
};

// Functions the game registers that the engine doesn't implement yet.
constexpr const char* kNotYetImplemented[] = {
    "SetCacheable", "SetCacheableTemps", "RunTree", "RunTreeWithGuid", "MultiplyAdValues",
    "KissingPointsUnlock", "KissingPointsNumLeft", "ObjectError", "ObjectAssert", "GetString",
    "GetStringBySelector", "SetObjectDefinitionField", "GetCatalogName", "GetCatalogDescription",
    "GetNeighborData", "GetNeighborRelationship", "SetNeighborRelationship", "GetMasterSelector",
    "OverrideTreeTable", "GetCustomStationNames", "StartSound",
};

// The world-facing parts of the API.

constexpr std::int16_t kErrorNotAPerson = 0x1C;
constexpr std::int16_t kErrorInvalidConstant = 0x2E;
constexpr std::int16_t kErrorMissingNeighbor = 0x37;
constexpr std::int16_t kErrorGlobalOutOfRange = 0x3F;
constexpr std::int16_t kErrorBadGuid = 0x7A;

// An instance of a script class: what its constructor bound (an object or neighbor id).
struct Instance {
    std::int32_t id = 0;
    bool neighbor = false;  // for data tables: a neighbor's tables rather than an object's
    std::int32_t kind = 0;  // for inventories: which inventory (InventoryKind)
};

int pushInstance(lua_State* L, const char* className, std::int32_t id) {
    new (lua_newuserdata(L, sizeof(Instance))) Instance{id};
    luaL_getmetatable(L, className);
    lua_setmetatable(L, -2);
    return 1;
}

// The Sim with this object id on the lot, or null.
const SimObject* personOnLot(ScriptCall& c, std::int16_t id) {
    const SimObject* o = c.world ? c.world->find(id) : nullptr;
    return o != nullptr && o->isPerson() ? o : nullptr;
}

SimObject& objectOrError(lua_State* L, std::int16_t id);
SimObject& personOrError(lua_State* L, std::int16_t id);

// ObjectManager methods (called as GlobalObjManager:method(...), so arguments start at 2).

constexpr std::size_t kSearchSkipFlagsField = 0x38;
constexpr std::size_t kSpeciesPersonData = 0xBA;
constexpr std::int16_t kErrorDataOutOfRange = 7;

// Pushes a Lua array (counted from 1) of the objects that match, in id order.
template <typename Match>
int pushObjectIds(lua_State* L, Match matches) {
    ScriptCall& c = call(L);
    lua_newtable(L);
    int n = 0;
    if (c.world != nullptr) {
        for (const SimObject* o = c.world->nextAfter(std::numeric_limits<std::int16_t>::min(), matches); o != nullptr;
             o = c.world->nextAfter(o->id, matches)) {
            lua_pushnumber(L, ++n);
            lua_pushnumber(L, o->id);
            lua_settable(L, -3);
        }
    }
    return 1;
}

bool searchable(const SimObject& o, bool includeSkipped) {
    return includeSkipped || (o.data[kSearchSkipFlagsField] & 0x05) == 0;
}

int objectManagerVerifyObjectId(lua_State* L) {
    ScriptCall& c = call(L);
    lua_pushboolean(L, c.world != nullptr && c.world->find(argShort(L, 2)) != nullptr);
    return 1;
}

int objectManagerVerifyNeighborId(lua_State* L) {
    ScriptCall& c = call(L);
    lua_pushboolean(L, c.world != nullptr && c.world->neighbor(argShort(L, 2)) != nullptr);
    return 1;
}

int objectManagerIsValidObjectGuid(lua_State* L) {
    ScriptCall& c = call(L);
    lua_pushboolean(L, c.code != nullptr && c.code->objectType(argUnsigned(L, 2)) != nullptr);
    return 1;
}

int objectManagerGetObjectGuidFromObjectId(lua_State* L) {
    const SimObject& o = objectOrError(L, argShort(L, 2));
    lua_pushnumber(L, static_cast<std::int32_t>(o.definition.guid()));
    return 1;
}

// The first object (Sim, for getPersonIdFromGUID) of a type, 0 when there is none; an unknown
// type is error 0x7A.
int firstOfType(lua_State* L, bool person) {
    ScriptCall& c = call(L);
    const std::uint32_t guid = argUnsigned(L, 2);
    if (c.code == nullptr || c.code->objectType(guid) == nullptr) {
        return gameError(L, kErrorBadGuid);
    }
    const SimObject* o = c.world ? c.world->nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& s) {
        return s.definition.guid() == guid && (!person || s.isPerson());
    }) : nullptr;
    lua_pushnumber(L, o ? o->id : 0);
    return 1;
}
int objectManagerGetObjectIdFromGuid(lua_State* L) { return firstOfType(L, false); }
int objectManagerGetPersonIdFromGuid(lua_State* L) { return firstOfType(L, true); }

// getPersonIds([includeSkipped], [speciesMask]): the Sims on the lot; a species mask keeps those
// whose bit 1 << species is set.
int objectManagerGetPersonIds(lua_State* L) {
    const bool includeSkipped = lua_gettop(L) >= 2 && lua_type(L, 2) == LUA_TBOOLEAN && lua_toboolean(L, 2);
    const std::uint32_t mask = lua_gettop(L) >= 3 && lua_type(L, 3) == LUA_TNUMBER ? argUnsigned(L, 3) : 0;
    return pushObjectIds(L, [&](const SimObject& s) {
        return s.isPerson() && searchable(s, includeSkipped) &&
               (mask == 0 || (mask & (1u << (s.personData[kSpeciesPersonData] & 0x1F))) != 0);
    });
}

// getObjectIds(query, values..., [includeSkipped]): kQuery_All, kQuery_ByGUID (values: the GUID)
// or kQuery_ByObjectDataField (values: field, value).
int objectManagerGetObjectIds(lua_State* L) {
    const int top = lua_gettop(L);
    const int query = top >= 2 ? static_cast<int>(lua_tonumber(L, 2)) : 0;
    std::int64_t values[2] = {0, 0};
    int count = 0;
    bool includeSkipped = false;
    for (int i = 3; i <= top; ++i) {
        if (lua_type(L, i) == LUA_TNUMBER && count < 2) {
            values[count++] = static_cast<std::int64_t>(lua_tonumber(L, i));
        } else if (lua_type(L, i) == LUA_TBOOLEAN) {
            includeSkipped = lua_toboolean(L, i) != 0;
            break;
        }
    }
    switch (query) {
    case 0:
        return pushObjectIds(L, [&](const SimObject& s) { return searchable(s, includeSkipped); });
    case 1: {
        const auto guid = static_cast<std::uint32_t>(values[0]);
        return pushObjectIds(L, [&](const SimObject& s) { return s.definition.guid() == guid && searchable(s, includeSkipped); });
    }
    case 3: {
        if (values[0] > 0x71 || values[0] < 0) {
            return gameError(L, kErrorDataOutOfRange);
        }
        const auto field = static_cast<std::size_t>(values[0]);
        const auto value = static_cast<std::int16_t>(values[1]);
        return pushObjectIds(L, [&](const SimObject& s) { return s.data[field] == value && searchable(s, includeSkipped); });
    }
    default:
        return stopUnsupported(L, query == 2 ? "ObjectManager:getObjectIds(kQuery_ForPartsofMultiTileObject)"
                                         : "ObjectManager:getObjectIds(kQuery_ByAccessibleRooms)");
    }
}

// getNeighborIds(): every neighbor id of the neighborhood, as an array.
int objectManagerGetNeighborIds(lua_State* L) {
    ScriptCall& c = call(L);
    if (lua_gettop(L) >= 2) {
        return stopUnsupported(L, "ObjectManager:getNeighborIds with arguments");
    }
    lua_newtable(L);
    int n = 0;
    if (const auto* ids = c.world ? c.world->neighbors() : nullptr) {
        for (const std::int16_t id : *ids) {
            lua_pushnumber(L, ++n);
            lua_pushnumber(L, id);
            lua_settable(L, -3);
        }
    }
    return 1;
}

// Data tables (ObjectDataTable, NeighborDataTable, nDataTable).

template <typename... Args>
int scriptCheckError(lua_State* L, const char* format, Args... args) {
    char message[256];
    std::snprintf(message, sizeof message, format, args...);
    return gameError(L, kErrorScriptCheck, message);
}

// Reads a Lua value for a table. Numbers become whole numbers (truncated) when `whole` is set and
// single-precision ones otherwise; inside a nested table keys are whole and values are not.
DataValue toDataValue(lua_State* L, int index, bool whole) {
    if (index < 0) {
        index = lua_gettop(L) + index + 1;
    }
    switch (lua_type(L, index)) {
    case LUA_TNUMBER:
        return whole ? DataValue::ofInt(static_cast<std::int32_t>(lua_tonumber(L, index)))
                     : DataValue::ofFloat(static_cast<float>(lua_tonumber(L, index)));
    case LUA_TBOOLEAN:
        return DataValue::ofBool(lua_toboolean(L, index) != 0);
    case LUA_TSTRING:
        return DataValue::ofString(lua_tostring(L, index));
    case LUA_TTABLE: {
        DataValue::Table table;
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            DataValue value = toDataValue(L, -1, false);
            table[toDataValue(L, -2, true)] = std::move(value);
            lua_pop(L, 1);
        }
        return DataValue::ofTable(std::move(table));
    }
    default:
        scriptCheckError(L, "Unsupported type for data table! Index: %d == %s.", index,
                         lua_typename(L, lua_type(L, index)));
        return {};
    }
}

void pushDataValue(lua_State* L, const DataValue& v) {
    switch (v.kind) {
    case DataValue::Kind::Float: lua_pushnumber(L, v.f); break;
    case DataValue::Kind::Int: lua_pushnumber(L, v.i); break;
    case DataValue::Kind::Unsigned: lua_pushnumber(L, static_cast<lua_Number>(v.u)); break;
    case DataValue::Kind::Bool: lua_pushboolean(L, v.b); break;
    case DataValue::Kind::String: lua_pushstring(L, v.s.c_str()); break;
    case DataValue::Kind::Table:
        lua_newtable(L);
        for (const auto& [key, value] : *v.table) {
            pushDataValue(L, key);
            pushDataValue(L, value);
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

// The tables of the instance a method is called on (self, argument 1).
DataTables& selfTables(lua_State* L) {
    const auto* self = static_cast<const Instance*>(lua_touserdata(L, 1));
    if (self == nullptr) {
        scriptCheckError(L, "Data table method called without its table");
    }
    const auto id = static_cast<std::int16_t>(self->id);
    return self->neighbor ? neighborTables(L, id) : objectTables(L, id);
}

std::uint32_t tableIdArg(lua_State* L) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(lua_tonumber(L, 2)));
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

int dataTableGetValue(lua_State* L) { return pushTableValue(L, selfTables(L)); }

int dataTableSet(lua_State* L, bool whole) {
    DataValue key = toDataValue(L, 3, true);
    DataValue value = toDataValue(L, 4, whole);
    existingTable(L).values[std::move(key)] = std::move(value);
    return 0;
}
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

// Object, Sim and neighbor values.

constexpr std::int16_t kErrorAttributeOutOfRange = 6;
constexpr std::int16_t kErrorMotiveOutOfRange = 0x1A;
constexpr std::int16_t kErrorSlotOutOfRange = 0x1B;
constexpr std::int16_t kErrorPersonDataOutOfRange = 0x21;
constexpr std::int16_t kErrorDefinitionFieldOutOfRange = 0x23;
constexpr std::int16_t kErrorArrayOutOfRange = 0x6C;

int getRunningObjectId(lua_State* L) {
    lua_pushnumber(L, call(L).runningObject);
    return 1;
}
int getRunningOnObjectId(lua_State* L) {
    lua_pushnumber(L, call(L).runningOnObject);
    return 1;
}

// GetObjectName(id): the object's name (for a Sim, the Sim's name).
int getObjectName(lua_State* L) {
    lua_pushstring(L, objectOrError(L, argShort(L, 1)).definition.name.c_str());
    return 1;
}

int getObjectGroupId(lua_State* L) {
    lua_pushnumber(L, static_cast<std::int32_t>(objectOrError(L, argShort(L, 1)).group));
    return 1;
}

// An element of a fixed-size value array, or the error code when the index is outside it.
template <typename Array>
auto& element(lua_State* L, Array& values, int index, std::int16_t error) {
    if (index < 0 || static_cast<std::size_t>(index) >= values.size()) {
        gameError(L, error);
    }
    return values[static_cast<std::size_t>(index)];
}

// Get/SetObjectAttribute(id, index[, value]) and the semi-attribute versions.
int getObjectAttribute(lua_State* L) {
    SimObject& o = objectOrError(L, argShort(L, 1));
    lua_pushnumber(L, element(L, o.attributes, argShort(L, 2), kErrorAttributeOutOfRange));
    return 1;
}
int setObjectAttribute(lua_State* L) {
    SimObject& o = objectOrError(L, argShort(L, 1));
    element(L, o.attributes, argShort(L, 2), kErrorAttributeOutOfRange) = argShort(L, 3);
    return 0;
}
int getObjectSemiAttribute(lua_State* L) {
    SimObject& o = objectOrError(L, argShort(L, 1));
    lua_pushnumber(L, element(L, o.semiAttributes, argShort(L, 2), kErrorAttributeOutOfRange));
    return 1;
}
int setObjectSemiAttribute(lua_State* L) {
    SimObject& o = objectOrError(L, argShort(L, 1));
    element(L, o.semiAttributes, argShort(L, 2), kErrorAttributeOutOfRange) = argShort(L, 3);
    return 0;
}

// GetObjectIdInSlot(id, slot): the object in a container slot, 0 when it is empty.
int getObjectIdInSlot(lua_State* L) {
    SimObject& o = objectOrError(L, argShort(L, 1));
    lua_pushnumber(L, element(L, o.slots, argShort(L, 2), kErrorSlotOutOfRange));
    return 1;
}

// GetObjectDefinitionField(guid, field): a field of an object type's definition.
int getObjectDefinitionField(lua_State* L) {
    ScriptCall& c = call(L);
    const ObjectType* type = c.code ? c.code->objectType(argUnsigned(L, 1)) : nullptr;
    if (type == nullptr) {
        return gameError(L, kErrorBadGuid);
    }
    const int field = static_cast<int>(lua_tonumber(L, 2));
    lua_pushnumber(L, element(L, type->definition.fields, field, kErrorDefinitionFieldOutOfRange));
    return 1;
}

// Get/SetPersonMotive(id, motive[, value]): a Sim's motive 0-15, read as a whole number.
int getPersonMotive(lua_State* L) {
    SimObject& o = personOrError(L, argShort(L, 1));
    lua_pushnumber(L, static_cast<int>(element(L, o.motives, static_cast<int>(lua_tonumber(L, 2)), kErrorMotiveOutOfRange)));
    return 1;
}
int setPersonMotive(lua_State* L) {
    SimObject& o = personOrError(L, argShort(L, 1));
    element(L, o.motives, static_cast<int>(lua_tonumber(L, 2)), kErrorMotiveOutOfRange) =
        static_cast<float>(static_cast<int>(lua_tonumber(L, 3)));
    return 0;
}

// Get/SetNeighborPersonData(neighborId, index[, value]): a neighbor's saved person data.
std::vector<std::int16_t>& neighborPersonDataOrError(lua_State* L, std::int16_t id) {
    ScriptCall& c = call(L);
    std::vector<std::int16_t>* values = c.world ? c.world->neighborPersonData(id) : nullptr;
    if (values == nullptr) {
        gameError(L, kErrorMissingNeighbor);
    }
    return *values;
}
int getNeighborPersonData(lua_State* L) {
    auto& values = neighborPersonDataOrError(L, argShort(L, 1));
    lua_pushnumber(L, element(L, values, argShort(L, 2), kErrorPersonDataOutOfRange));
    return 1;
}
int setNeighborPersonData(lua_State* L) {
    auto& values = neighborPersonDataOrError(L, argShort(L, 1));
    element(L, values, argShort(L, 2), kErrorPersonDataOutOfRange) = argShort(L, 3);
    return 0;
}

// SetSimulatorGlobal(index, value): one of the 60 simulator globals; otherwise error 0x3F.
int setSimulatorGlobal(lua_State* L) {
    ScriptCall& c = call(L);
    const int i = static_cast<int>(lua_tonumber(L, 1));
    if (i < 0 || i > 0x3B) {
        return gameError(L, kErrorGlobalOutOfRange);
    }
    if (c.globals != nullptr && static_cast<std::size_t>(i) < c.globalCount) {
        c.globals[i] = argShort(L, 2);
    }
    return 0;
}

// Object arrays: GetNumObjectArrays(id), GetObjectArrayCopy(id, array) as a Lua array, and
// SetObjectArray(id, array, values), which replaces the array with the table's values in the
// order Lua's next() gives them.
int getNumObjectArrays(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(objectOrError(L, argShort(L, 1)).arrays.size()));
    return 1;
}
int getObjectArrayCopy(lua_State* L) {
    SimObject& o = objectOrError(L, argShort(L, 1));
    const auto& values = element(L, o.arrays, static_cast<int>(lua_tonumber(L, 2)), kErrorArrayOutOfRange);
    lua_newtable(L);
    for (std::size_t i = 0; i < values.size(); ++i) {
        lua_pushnumber(L, static_cast<lua_Number>(i + 1));
        lua_pushnumber(L, values[i]);
        lua_settable(L, -3);
    }
    return 1;
}
int setObjectArray(lua_State* L) {
    SimObject& o = objectOrError(L, argShort(L, 1));
    auto& values = element(L, o.arrays, static_cast<int>(lua_tonumber(L, 2)), kErrorArrayOutOfRange);
    values.clear();
    if (lua_type(L, 3) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {
            values.push_back(static_cast<std::int16_t>(static_cast<int>(lua_tonumber(L, -1))));
            lua_pop(L, 1);
        }
    }
    return 0;
}

// nWorld.GetObjectTile(id) / GetObjectLocation(id): three values, the position's second and first
// coordinates (whole tiles, or tile units as numbers) and the level counted from 0.
int getObjectTile(lua_State* L) {
    const SimObject& o = objectOrError(L, argShort(L, 1));
    lua_pushnumber(L, o.y >> 16);
    lua_pushnumber(L, o.x >> 16);
    lua_pushnumber(L, o.level - 1);
    return 3;
}
int getObjectLocation(lua_State* L) {
    const SimObject& o = objectOrError(L, argShort(L, 1));
    lua_pushnumber(L, static_cast<float>(o.y) / 65536.0f);
    lua_pushnumber(L, static_cast<float>(o.x) / 65536.0f);
    lua_pushnumber(L, static_cast<float>(o.level - 1));
    return 3;
}

// Tokens come only from an inventory.
int newInventoryToken(lua_State* L) {
    return gameError(L, kErrorScriptCheck, "Cannot directly create an InventoryToken--use Inventory class to access this wrapped class.");
}

// Inventories (Inventory) and their tokens (InventoryToken).

constexpr std::int16_t kErrorInventoryMissing = 0x57;
constexpr std::int16_t kErrorPropertyOutOfRange = 0x58;
constexpr std::int16_t kErrorInventoryType = 0x59;
constexpr std::uint16_t kTokenMarkedForDeletion = 0x04;

using TokenRef = std::shared_ptr<InventoryToken>;

struct TokenHandle {
    TokenRef token;
};

int pushToken(lua_State* L, TokenRef token) {
    new (lua_newuserdata(L, sizeof(TokenHandle))) TokenHandle{std::move(token)};
    luaL_getmetatable(L, "InventoryToken");
    lua_setmetatable(L, -2);
    return 1;
}

int releaseToken(lua_State* L) {
    static_cast<TokenHandle*>(lua_touserdata(L, 1))->~TokenHandle();
    return 0;
}

InventoryToken& selfToken(lua_State* L) {
    auto* handle = static_cast<TokenHandle*>(lua_touserdata(L, 1));
    if (handle == nullptr || !handle->token) {
        scriptCheckError(L, "InventoryToken method called without its token");
    }
    return *handle->token;
}

int tokenGuid(lua_State* L) {
    lua_pushnumber(L, static_cast<std::int32_t>(selfToken(L).guid));
    return 1;
}
int tokenFlags(lua_State* L) {
    lua_pushnumber(L, static_cast<std::int16_t>(selfToken(L).flags));
    return 1;
}
// setFlag, testFlag and clearFlag take a mask (InventoryToken.kFlag...).
int tokenSetFlag(lua_State* L) {
    selfToken(L).flags |= static_cast<std::uint16_t>(static_cast<int>(lua_tonumber(L, 2)));
    return 0;
}
int tokenTestFlag(lua_State* L) {
    lua_pushboolean(L, (selfToken(L).flags & static_cast<std::uint16_t>(static_cast<int>(lua_tonumber(L, 2)))) != 0);
    return 1;
}
int tokenClearFlag(lua_State* L) {
    selfToken(L).flags &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(static_cast<int>(lua_tonumber(L, 2))));
    return 0;
}
int tokenCategory(lua_State* L) {
    lua_pushnumber(L, selfToken(L).category);
    return 1;
}
int tokenSetCategory(lua_State* L) {
    selfToken(L).category = static_cast<std::uint16_t>(static_cast<int>(lua_tonumber(L, 2)));
    return 0;
}
int tokenCountProperties(lua_State* L) {
    lua_pushnumber(L, static_cast<std::int16_t>(selfToken(L).properties.size()));
    return 1;
}
int tokenClearProperties(lua_State* L) {
    selfToken(L).properties.clear();
    return 0;
}
// Properties are counted from 1; outside the list is error 0x58.
std::int16_t& tokenProperty(lua_State* L, InventoryToken& token) {
    const int index = static_cast<int>(lua_tonumber(L, 2)) - 1;
    if (index < 0 || static_cast<std::size_t>(index) >= token.properties.size()) {
        gameError(L, kErrorPropertyOutOfRange);
    }
    return token.properties[static_cast<std::size_t>(index)];
}
int tokenSetProperty(lua_State* L) {
    InventoryToken& token = selfToken(L);
    tokenProperty(L, token) = argShort(L, 3);
    lua_pushboolean(L, true);
    return 1;
}
int tokenGetProperty(lua_State* L) {
    InventoryToken& token = selfToken(L);
    lua_pushnumber(L, tokenProperty(L, token));
    return 1;
}
int tokenPushProperty(lua_State* L) {
    selfToken(L).properties.push_back(argShort(L, 2));
    return 0;
}
// popProperty(): true and the last value, or false and 0 when there is none.
int tokenPopProperty(lua_State* L) {
    InventoryToken& token = selfToken(L);
    std::int16_t value = 0;
    const bool popped = !token.properties.empty();
    if (popped) {
        value = token.properties.back();
        token.properties.pop_back();
    }
    lua_pushboolean(L, popped);
    lua_pushnumber(L, value);
    return 2;
}
int tokenPropertiesCopy(lua_State* L) {
    const InventoryToken& token = selfToken(L);
    lua_newtable(L);
    for (std::size_t i = 0; i < token.properties.size(); ++i) {
        lua_pushnumber(L, static_cast<lua_Number>(i + 1));
        lua_pushnumber(L, token.properties[i]);
        lua_settable(L, -3);
    }
    return 1;
}

// Appends values to a token's properties: the values of a table (in next() order) when argument
// `first` is a table, otherwise the arguments from `first` on.
void appendProperties(lua_State* L, InventoryToken& token, int first) {
    const int top = lua_gettop(L);
    if (top == first && lua_type(L, first) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, first) != 0) {
            token.properties.push_back(static_cast<std::int16_t>(static_cast<int>(lua_tonumber(L, -1))));
            lua_pop(L, 1);
        }
        return;
    }
    for (int i = first; i <= top; ++i) {
        token.properties.push_back(argShort(L, i));
    }
}

int tokenSetProperties(lua_State* L) {
    InventoryToken& token = selfToken(L);
    token.properties.clear();
    appendProperties(L, token, 2);
    return 0;
}

// Inventory.new(kind, id): kTypeGlobal, kTypeLot (lot id), kTypeFamily (family id),
// kTypeNeighbor (neighbor id) or kTypeGameWide. An unknown kind is error 0x59; an inventory the
// neighborhood doesn't have is 0x57.
int newInventory(lua_State* L) {
    ScriptCall& c = call(L);
    const int kind = static_cast<int>(lua_tonumber(L, 1));
    if (kind < 0 || kind > static_cast<int>(InventoryKind::GameWide)) {
        return gameError(L, kErrorInventoryType);
    }
    const auto id = static_cast<std::int32_t>(lua_tonumber(L, 2));
    if (c.world == nullptr || c.world->inventory(static_cast<InventoryKind>(kind), id) == nullptr) {
        return gameError(L, kErrorInventoryMissing);
    }
    pushInstance(L, "Inventory", id);
    static_cast<Instance*>(lua_touserdata(L, -1))->kind = kind;
    return 1;
}

Inventory& selfInventory(lua_State* L) {
    const auto* self = static_cast<const Instance*>(lua_touserdata(L, 1));
    ScriptCall& c = call(L);
    Inventory* inventory = self && c.world ? c.world->inventory(static_cast<InventoryKind>(self->kind), self->id) : nullptr;
    if (inventory == nullptr) {
        gameError(L, kErrorInventoryMissing);
    }
    return *inventory;
}

// addToken(guid, values...) and addTokenWithCategory(guid, category, values...): a new singular
// token; the values may also come as one table.
int addTokenFrom(lua_State* L, bool withCategory) {
    Inventory& inventory = selfInventory(L);
    auto token = std::make_shared<InventoryToken>();
    token->guid = argUnsigned(L, 2);
    if (withCategory) {
        token->category = static_cast<std::uint16_t>(argUnsigned(L, 3));
    }
    appendProperties(L, *token, withCategory ? 4 : 3);
    inventory.singular.push_back(std::move(token));
    lua_pushboolean(L, true);
    return 1;
}
int inventoryAddToken(lua_State* L) { return addTokenFrom(L, false); }
int inventoryAddTokenWithCategory(lua_State* L) { return addTokenFrom(L, true); }

int inventoryRemoveMarked(lua_State* L) {
    auto& tokens = selfInventory(L).singular;
    std::erase_if(tokens, [](const TokenRef& t) { return (t->flags & kTokenMarkedForDeletion) != 0; });
    return 0;
}

// Which tokens a list or count takes: -1 for any GUID or flags; the singular lists also need the
// category to match (0 unless asked for).
struct TokenFilter {
    std::int64_t guid = -1;
    std::int32_t flags = -1;
    std::optional<std::uint16_t> category;
};

bool matches(const InventoryToken& t, const TokenFilter& f) {
    return (f.guid == -1 || static_cast<std::int32_t>(t.guid) == static_cast<std::int32_t>(f.guid)) &&
           (f.flags == -1 || static_cast<std::int16_t>(t.flags) == f.flags) &&
           (!f.category || t.category == *f.category);
}

// A table of the matching tokens, each under its position in the whole list (counted from 1).
int pushTokens(lua_State* L, const std::vector<TokenRef>& tokens, const TokenFilter& filter) {
    lua_newtable(L);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] && matches(*tokens[i], filter)) {
            lua_pushnumber(L, static_cast<lua_Number>(i + 1));
            pushToken(L, tokens[i]);
            lua_settable(L, -3);
        }
    }
    return 1;
}

TokenFilter guidFilter(lua_State* L) {
    TokenFilter f;
    f.guid = static_cast<std::int32_t>(lua_tonumber(L, 2));
    if (lua_gettop(L) >= 3) {
        f.flags = static_cast<std::int32_t>(lua_tonumber(L, 3));
    }
    return f;
}
TokenFilter flagsFilter(lua_State* L) {
    TokenFilter f;
    f.flags = static_cast<std::int32_t>(lua_tonumber(L, 2));
    return f;
}
TokenFilter withCategory(TokenFilter f, std::uint16_t category = 0) {
    f.category = category;
    return f;
}

int inventoryTokens(lua_State* L) { return pushTokens(L, selfInventory(L).singular, withCategory({})); }
int inventoryTokensByGuid(lua_State* L) { return pushTokens(L, selfInventory(L).singular, withCategory(guidFilter(L))); }
int inventoryTokensByFlags(lua_State* L) { return pushTokens(L, selfInventory(L).singular, withCategory(flagsFilter(L))); }
int inventoryTokensByCategory(lua_State* L) {
    return pushTokens(L, selfInventory(L).singular, withCategory({}, static_cast<std::uint16_t>(argUnsigned(L, 2))));
}
int inventoryCountedTokens(lua_State* L) { return pushTokens(L, selfInventory(L).counted, {}); }
int inventoryCountedTokensByGuid(lua_State* L) { return pushTokens(L, selfInventory(L).counted, guidFilter(L)); }
int inventoryCountedTokensByFlags(lua_State* L) { return pushTokens(L, selfInventory(L).counted, flagsFilter(L)); }

// countTokens...: how many singular tokens match, whatever their category.
int countTokens(lua_State* L, const TokenFilter& filter) {
    const auto& tokens = selfInventory(L).singular;
    lua_pushnumber(L, static_cast<lua_Number>(std::count_if(tokens.begin(), tokens.end(), [&](const TokenRef& t) {
        return t && matches(*t, filter);
    })));
    return 1;
}
int inventoryCountTokens(lua_State* L) { return countTokens(L, {}); }
int inventoryCountTokensByGuid(lua_State* L) { return countTokens(L, guidFilter(L)); }
int inventoryCountTokensByFlags(lua_State* L) { return countTokens(L, flagsFilter(L)); }

// findToken(guid, [flagsValue], [flagsMask], [backward]): the first unmarked singular token of the
// GUID with category 0 whose flags match ((flags & mask) == (value & mask); with only the GUID any
// flags match, with a value and no mask all bits must match), searching from the end when asked;
// nil when there is none.
int inventoryFindToken(lua_State* L) {
    const auto& tokens = selfInventory(L).singular;
    const int args = lua_gettop(L) - 1;
    const auto guid = argUnsigned(L, 2);
    std::uint16_t value = 0, mask = 0;
    if (args > 1) {
        value = static_cast<std::uint16_t>(argShort(L, 3));
        mask = 0xFFFF;
    }
    if (args > 2) {
        mask = static_cast<std::uint16_t>(argShort(L, 4));
    }
    const bool backward = args > 3 && lua_toboolean(L, 5) != 0;
    auto matches = [&](const InventoryToken& t) {
        return t.guid == guid && (t.flags & kTokenMarkedForDeletion) == 0 && (value & mask) == (t.flags & mask) &&
               t.category == 0;
    };
    for (std::size_t n = 0; n < tokens.size(); ++n) {
        const auto& t = tokens[backward ? tokens.size() - 1 - n : n];
        if (t && matches(*t)) {
            return pushToken(L, t);
        }
    }
    lua_pushnil(L);
    return 1;
}

// getTokenAtIndex(index): the singular token at that position (from 1), or nil.
int inventoryTokenAtIndex(lua_State* L) {
    const auto& tokens = selfInventory(L).singular;
    const int index = static_cast<int>(lua_tonumber(L, 2)) - 1;
    if (index < 0 || static_cast<std::size_t>(index) >= tokens.size()) {
        lua_pushnil(L);
        return 1;
    }
    return pushToken(L, tokens[static_cast<std::size_t>(index)]);
}

// Classes whose constructors take no arguments.
int newNeighborhood(lua_State* L) { return pushInstance(L, "Neighborhood", 0); }
int newObjectManager(lua_State* L) { return pushInstance(L, "ObjectManager", 0); }

// CachedNeighbor.new(neighborId): an unknown neighbor is error 0x37.
int newCachedNeighbor(lua_State* L) {
    ScriptCall& c = call(L);
    const std::int16_t id = argShort(L, 1);
    if (c.world == nullptr || c.world->neighbor(id) == nullptr) {
        return gameError(L, kErrorMissingNeighbor);
    }
    return pushInstance(L, "CachedNeighbor", id);
}

// CachedPerson.new(objectId): a Sim on the lot; anything else is error 0x1C.
int newCachedPerson(lua_State* L) {
    const std::int16_t id = argShort(L, 1);
    if (personOnLot(call(L), id) == nullptr) {
        return gameError(L, kErrorNotAPerson);
    }
    return pushInstance(L, "CachedPerson", id);
}

// Neighborhood:isRoommate(objectId): a Sim on the lot whose person data 0x20 ("Person Type") is 0
// and whose person data 0x54 has bit 0x800 set.
int neighborhoodIsRoommate(lua_State* L) {
    const SimObject* sim = personOnLot(call(L), argShort(L, 2));
    lua_pushboolean(L, sim != nullptr && sim->personData[0x20] == 0 && (sim->personData[0x54] & 0x800) != 0);
    return 1;
}

// Neighborhood:isValidLot(lotId): whether the neighborhood has that lot.
int neighborhoodIsValidLot(lua_State* L) {
    ScriptCall& c = call(L);
    const auto* lots = c.world ? c.world->lotIds() : nullptr;
    lua_pushboolean(L, lots != nullptr && lots->contains(static_cast<std::int32_t>(lua_tonumber(L, 2))));
    return 1;
}

// GetSimulatorGlobal(index): one of the 60 simulator globals; otherwise error 0x3F.
int getSimulatorGlobal(lua_State* L) {
    ScriptCall& c = call(L);
    const int i = static_cast<int>(lua_tonumber(L, 1));
    if (i < 0 || i > 0x3B) {
        return gameError(L, kErrorGlobalOutOfRange);
    }
    lua_pushnumber(L, c.globals && static_cast<std::size_t>(i) < c.globalCount ? c.globals[i] : 0);
    return 1;
}

// A constants table (BCON) by id, from the ranges the VM uses: below 0x1000 global, below 0x2000
// the owner's own, otherwise the owner's semi-global group. Missing is error 0x2E.
const std::vector<std::int16_t>& constantsTable(lua_State* L, std::uint32_t owner, std::uint16_t table) {
    ScriptCall& c = call(L);
    const std::vector<std::int16_t>* constants = nullptr;
    if (c.code != nullptr) {
        const std::uint32_t group = table < 0x1000 ? kGlobalGroup : table < 0x2000 ? owner : c.code->semiGlobalGroup(owner);
        constants = c.code->constants(group, table);
    }
    if (constants == nullptr) {
        gameError(L, kErrorInvalidConstant);
    }
    return *constants;
}

std::uint32_t typeGroup(lua_State* L, std::uint32_t guid) {
    ScriptCall& c = call(L);
    const ObjectType* type = c.code ? c.code->objectType(guid) : nullptr;
    if (type == nullptr) {
        gameError(L, kErrorBadGuid);
    }
    return type->group;
}

int pushConstant(lua_State* L, const std::vector<std::int16_t>& constants, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= constants.size()) {
        return gameError(L, kErrorInvalidConstant);
    }
    lua_pushnumber(L, constants[static_cast<std::size_t>(index)]);
    return 1;
}

// GetConstant(table, index) / GetNumConstants(table): the running tree's constants.
int getConstant(lua_State* L) {
    return pushConstant(L, constantsTable(L, call(L).codeOwner, static_cast<std::uint16_t>(argShort(L, 1))),
                        static_cast<int>(lua_tonumber(L, 2)));
}
int getNumConstants(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(
                          constantsTable(L, call(L).codeOwner, static_cast<std::uint16_t>(argShort(L, 1))).size()));
    return 1;
}

// The same for an object type by GUID; an unknown GUID is error 0x7A.
int getConstantWithGuid(lua_State* L) {
    const std::uint32_t group = typeGroup(L, argUnsigned(L, 1));
    return pushConstant(L, constantsTable(L, group, static_cast<std::uint16_t>(argShort(L, 2))),
                        static_cast<int>(lua_tonumber(L, 3)));
}
int getNumConstantsWithGuid(lua_State* L) {
    const std::uint32_t group = typeGroup(L, argUnsigned(L, 1));
    lua_pushnumber(L, static_cast<lua_Number>(constantsTable(L, group, static_cast<std::uint16_t>(argShort(L, 2))).size()));
    return 1;
}
// A whole table as a Lua table, counted from 1.
int getConstantsTableByGuid(lua_State* L) {
    const std::uint32_t group = typeGroup(L, argUnsigned(L, 1));
    const auto& constants = constantsTable(L, group, static_cast<std::uint16_t>(argShort(L, 2)));
    lua_newtable(L);
    for (std::size_t i = 0; i < constants.size(); ++i) {
        lua_pushnumber(L, static_cast<lua_Number>(i + 1));
        lua_pushnumber(L, constants[i]);
        lua_settable(L, -3);
    }
    return 1;
}

SimObject& objectOrError(lua_State* L, std::int16_t id) {
    ScriptCall& c = call(L);
    SimObject* o = c.world ? c.world->find(id) : nullptr;
    if (o == nullptr) {
        gameError(L, kErrorScriptCheck, "Could not find object!");
    }
    return *o;
}

SimObject& personOrError(lua_State* L, std::int16_t id) {
    ScriptCall& c = call(L);
    SimObject* o = c.world ? c.world->find(id) : nullptr;
    if (o == nullptr || !o->isPerson()) {
        gameError(L, kErrorNotAPerson);
    }
    return *o;
}

// GetObjectData(id, field) / SetObjectData(id, field, value); fields outside the object's read
// as 0 and aren't written.
int getObjectData(lua_State* L) {
    SimObject& o = objectOrError(L, argShort(L, 1));
    const std::int16_t field = argShort(L, 2);
    lua_pushnumber(L, field >= 0 && static_cast<std::size_t>(field) < o.data.size() ? o.data[static_cast<std::size_t>(field)] : 0);
    return 1;
}
int setObjectData(lua_State* L) {
    SimObject& o = objectOrError(L, argShort(L, 1));
    const std::int16_t field = argShort(L, 2);
    if (field >= 0 && static_cast<std::size_t>(field) < o.data.size()) {
        o.data[static_cast<std::size_t>(field)] = argShort(L, 3);
    }
    return 0;
}

// GetPersonData(id, index) / SetPersonData(id, index, value): a Sim on the lot (else error 0x1C).
int getPersonData(lua_State* L) {
    SimObject& o = personOrError(L, argShort(L, 1));
    const std::int16_t i = argShort(L, 2);
    lua_pushnumber(L, i >= 0 && static_cast<std::size_t>(i) < o.personData.size() ? o.personData[static_cast<std::size_t>(i)] : 0);
    return 1;
}
int setPersonData(lua_State* L) {
    SimObject& o = personOrError(L, argShort(L, 1));
    const std::int16_t i = argShort(L, 2);
    if (i >= 0 && static_cast<std::size_t>(i) < o.personData.size()) {
        o.personData[static_cast<std::size_t>(i)] = argShort(L, 3);
    }
    return 0;
}

struct Constant {
    const char* name;
    int value;
};

struct ScriptClass {
    const char* name;
    bool isClass;
    lua_CFunction constructor;  // null: new() is not implemented yet
    std::vector<Function> methods;
    std::vector<const char*> notYetImplemented;
    std::vector<Constant> constants;
    lua_CFunction finalizer = nullptr;  // __gc for instances that own something
};

// The game's class-style script API: classes (with new()) and plain tables, their methods and
// constants. Methods the engine doesn't implement yet stop the script and say which.
const ScriptClass kClasses[] = {
    {"nGameState", false, nullptr,
     {{"GetProperty", getProperty}, {"SetProperty", setProperty}, {"IsEPInstalled", isEpInstalled},
      {"GetInstalledEps", getInstalledEps}},
     {"GetLotTripInfo", "GetTransitionsEnabled", "GetSaveLotEnabled", "GetCurrentState"},
     {}},
    {"nBusiness", false, nullptr,
     {},
     {"LotHasBusiness", "CreateBusiness", "RemoveBusiness"},
     {}},
    {"CachedFamily", true, nullptr,
     {},
     {"getId", "getLotId", "getCurrentLotId", "getVacationLotId", "setVacationLotId", "getName", "getMembers", "getNumMembers", "getFunds", "getLastSavedFunds", "isNeighborInFamily", "addMemberClothing", "IsAFamilyInThisLot"},
     {}},
    {"ObjectDefinition", true, nullptr,
     {},
     {"getField", "get32BitField"},
     {}},
    {"LightingManager", true, nullptr,
     {},
     {"setPlacementRoomLightPercent"},
     {}},
    {"WeatherInfo", true, nullptr,
     {},
     {"getSeason", "setSeason", "getSeasonQuadrant", "setSeasonQuadrant", "getSeasonDaysStandard", "setSeasonDaysStandard", "getSeasonDaysAdditional", "setSeasonDaysAdditional", "getPrecipitationType", "setPrecipitationType", "getPrecipitationIntensity", "setPrecipitationIntensity", "getAccumulationLevel", "setAccumulationLevel", "getAccumulationType", "setAccumulationType", "getTemperature", "setTemperature", "getSkyClarity", "setSkyClarity", "getPondFrozenState", "setPondFrozenState", "isCameraIndoors"},
     {}},
    {"nUI", false, nullptr,
     {},
     {"ShowTextNotification", "DebugTNS", "InvalidateSimThumbnail", "OpenProgressDialog", "CloseProgressDialog", "ShowProgressDialogControl", "UpdateSimChooserUI", "AddActionString", "UpdateSkewerAndPortrait"},
     {}},
    {"nMessaging", false, nullptr,
     {},
     {"SubscribeMessage", "UnsubscribeMessage", "PostMessage"},
     {}},
    {"InventoryToken", true, newInventoryToken,
     {{"getGUID", tokenGuid}, {"getFlags", tokenFlags}, {"setFlag", tokenSetFlag}, {"testFlag", tokenTestFlag},
      {"clearFlag", tokenClearFlag}, {"getCategory", tokenCategory}, {"setCategory", tokenSetCategory},
      {"countProperties", tokenCountProperties}, {"clearProperties", tokenClearProperties},
      {"setProperty", tokenSetProperty}, {"getProperty", tokenGetProperty}, {"pushProperty", tokenPushProperty},
      {"popProperty", tokenPopProperty}, {"getPropertiesCopy", tokenPropertiesCopy},
      {"setProperties", tokenSetProperties}},
     {},
     {{"kFlagVisible", 1}, {"kFlagMemory", 2}, {"kFlagMarkedForDeletion", 4}, {"kFlagShopping", 8}},
     releaseToken},
    {"Inventory", true, newInventory,
     {{"addToken", inventoryAddToken}, {"addTokenWithCategory", inventoryAddTokenWithCategory},
      {"removeTokensMarkedForDeletion", inventoryRemoveMarked}, {"getTokens", inventoryTokens},
      {"getTokensByGUID", inventoryTokensByGuid}, {"getTokensByFlags", inventoryTokensByFlags},
      {"getTokenAtIndex", inventoryTokenAtIndex}, {"countTokens", inventoryCountTokens},
      {"countTokensByGUID", inventoryCountTokensByGuid}, {"countTokensByFlags", inventoryCountTokensByFlags},
      {"getSingularTokens", inventoryTokens}, {"getSingularTokensByGUID", inventoryTokensByGuid},
      {"getSingularTokensByFlags", inventoryTokensByFlags}, {"getSingularTokensByCategory", inventoryTokensByCategory},
      {"getCountedTokens", inventoryCountedTokens}, {"getCountedTokensByGUID", inventoryCountedTokensByGuid},
      {"getCountedTokensByFlags", inventoryCountedTokensByFlags}, {"findToken", inventoryFindToken}},
     {"removeTokensOfGUID", "copyToken", "copyCountedToken", "getTokenArchiveGUIDS",
      "createTokenFromObject", "createObjectFromToken"},
     {{"kTypeGlobal", 0}, {"kTypeLot", 1}, {"kTypeFamily", 2}, {"kTypeNeighbor", 3}, {"kTypeGameWide", 4}}},
    {"Neighborhood", true, newNeighborhood,
     {{"isRoommate", neighborhoodIsRoommate}, {"isValidLot", neighborhoodIsValidLot}},
     {"getLotType", "getLotOwner", "setLotOwner", "getLotValue", "getLotName", "getLotIds", "getLotIdsInAllNeighborhoods", "getNeighborhoodIdFromLotId", "isHobbyFlagSet", "getApartmentLinkedFamilies", "getApartmentLinkedLots", "getApartmentLinkedControlRooms", "setAptControlRoom", "getAptBaseLot", "setAptMaxRent", "setAptMinRent", "setMaxApartments", "getWitchLotType", "getRoommateId", "setRoommateId", "updateLotClass", "isApartmentResident", "isApartmentDoor", "isNeighborsApartment", "getNIDsApartment"},
     {}},
    {"BusinessInfo", true, nullptr,
     {},
     {"getRank", "setRank", "getHighestRank", "setHighestRank", "getRemoteIncomeDecay", "setRemoteIncomeDecay", "getCurrentPnLData", "getAllPnLData", "getTotalProfit", "getTotals", "getAllCustomerData", "getCustomerIds", "getCustomerDataById", "isCustomer", "addCustomer", "removeCustomer", "getAllEmployeeData", "getEmployeeIds", "getEmployeeDataById", "isEmployee", "addEmployee", "removeEmployee", "getWholesaleMultiplier", "setWholesaleMultiplier", "getBusinessFlags", "setBusinessFlags"},
     {}},
    {"CachedNeighbor", true, newCachedNeighbor,
     {},
     {"getPersonData", "setPersonData", "getGUID", "getOutfitProperty", "getName", "setName", "getSurname", "setSurname", "getFullName", "getBio", "setBio"},
     {}},
    {"nWorld", false, nullptr,
     {{"GetObjectTile", getObjectTile}, {"GetObjectLocation", getObjectLocation}},
     {"GetMinX", "GetMaxX", "GetMinY", "GetMaxY", "GetLotSizeX", "GetLotSizeY", "IsOnLot", "HasSolidFloor", "HasSolidGround", "HasRoof", "IsOutsideForWeather", "IsLocked", "HasDriveway", "HasStage", "HasStageAny", "IsLoadBearing", "GetMinTileX", "GetMinTileY", "GetMaxTileX", "GetMaxTileY", "GetNumTilesX", "GetNumTilesY", "GetMinLevel", "GetMaxLevel", "GetNumLevels", "GetObjectTiles", "GetObjectsOnTile", "FindLocationForObject", "GetTerrainElevationForObject", "GetTerrainElevationForXY", "GetWaterElevationForObject", "GetWaterElevationForXY", "MoveObjectToLocation", "IsPoolOnTile", "IsWaterOnTile", "IsDeckOnTile", "IsWaterOnEntireTile", "GetSnowAccumulationLevelOnTile", "IsWallBetweenPoints", "IsBeachLocation", "SetLotImposterDirty", "MoveObjectToWallLocation", "CanMoveObjectToLocation", "GetRoomOnSideOfWall", "GetNumWallSections", "GetTileOnSideOfWall", "GetOrientationAwayFromWallSide", "GetRoomAtLocation", "GetLoadBearingRoomAtLocation", "IsLoadWallBetweenPoints"},
     {}},
    {"NeighborhoodManager", true, nullptr,
     {},
     {"getNumNeighborhoodsOfType", "getCurrentNeighborhoodType", "getNeighborhoodType", "getNeighborhoodIds", "isValidNeighborhood", "getLotIdsByNeighborhood", "getLotIdsInAllNeighborhoods", "getNeighborhoodIdFromLotId", "getSeasonOfQuadrant"},
     {}},
    {"CachedPerson", true, newCachedPerson,
     {},
     {"getPersonData", "setPersonData", "getMotive", "setMotive", "getGUID", "getNumQueuedInteractions", "getQueuedInteractionId", "getQueuedInteractionObjectId", "cancelQueuedInteraction", "getNumSubQueuedInteractions", "getQueuedSubInteractionId", "getQueuedSubInteractionObjectId", "unlockVacationCollectible", "getFriendsRequiredForPromtion", "isAccessibleRoom", "isQueuedObjAdvertisingForX"},
     {}},
    {"ObjectDataTable", true, newObjectDataTable,
     {{"newTable", dataTableNew}, {"removeTable", dataTableRemove}, {"clearTable", dataTableClear},
      {"hasTable", dataTableHas}, {"removeAll", dataTableRemoveAll}, {"getTableIds", dataTableIds},
      {"getTableKeys", dataTableKeys}, {"getTableValues", dataTableValues}, {"getTableCopy", dataTableCopy},
      {"setTableFromCopy", dataTableSetFromCopy}, {"getTableValue", dataTableGetValue},
      {"setTableValue", dataTableSetValue}, {"setTableValueAsInt", dataTableSetValueAsInt},
      {"removeTableValue", dataTableRemoveValue}, {"hasTableValue", dataTableHasValue},
      {"getTableName", dataTableGetName}, {"setTableName", dataTableSetName}},
     {},
     {}},
    {"NeighborDataTable", true, newNeighborDataTable,
     {{"newTable", dataTableNew}, {"removeTable", dataTableRemove}, {"clearTable", dataTableClear},
      {"hasTable", dataTableHas}, {"removeAll", dataTableRemoveAll}, {"getTableIds", dataTableIds},
      {"getTableKeys", dataTableKeys}, {"getTableValues", dataTableValues}, {"getTableCopy", dataTableCopy},
      {"setTableFromCopy", dataTableSetFromCopy}, {"getTableValue", dataTableGetValue},
      {"setTableValue", dataTableSetValue}, {"setTableValueAsInt", dataTableSetValueAsInt},
      {"removeTableValue", dataTableRemoveValue}, {"hasTableValue", dataTableHasValue},
      {"getTableName", dataTableGetName}, {"setTableName", dataTableSetName}},
     {},
     {}},
    {"nDataTable", false, nullptr,
     {{"GetObjectTableValue", getObjectTableValue}, {"GetNeighborTableValue", getNeighborTableValue}},
     {},
     {}},
    {"ObjectManager", true, newObjectManager,
     {{"verifyObjectId", objectManagerVerifyObjectId},
      {"verifyNeighborId", objectManagerVerifyNeighborId},
      {"isValidObjectGUID", objectManagerIsValidObjectGuid},
      {"getObjectGUIDFromObjectId", objectManagerGetObjectGuidFromObjectId},
      {"getObjectIdFromGUID", objectManagerGetObjectIdFromGuid},
      {"getPersonIdFromGUID", objectManagerGetPersonIdFromGuid},
      {"getPersonIds", objectManagerGetPersonIds},
      {"getObjectIds", objectManagerGetObjectIds},
      {"getNeighborIds", objectManagerGetNeighborIds}},
     {"getDistanceBetweenObjects", "getDirectionToObject", "isWallBetweenObjects", "getObjectDefinitionGUIDs",
      "createNewObject", "removeObjectInstance", "getNeighborsWithRelationship", "getLoadBearingRoom"},
     {{"kQuery_All", 0}, {"kQuery_ByGUID", 1}, {"kQuery_ForPartsofMultiTileObject", 2}, {"kQuery_ByObjectDataField", 3}, {"kQuery_ByAccessibleRooms", 4}, {"kQuery_ByAccessableRooms", 4}, {"kSpeciesFlag_Human", 1}, {"kSpeciesFlag_DogLarge", 2}, {"kSpeciesFlag_DogSmall", 4}, {"kSpeciesFlag_Cat", 8}, {"kSpeciesMask_All", 0}}},
};

// The game removes these from its state.
constexpr const char* kRemovedGlobals[] = {"dofile", "loadfile", "loadlib", "loadstring", "require"};

// Loads a game chunk (rewritten for this build) as a function on the stack; false with the
// message on the stack when it can't.
bool loadChunk(lua_State* L, const luascript::Script& script, std::string& error) {
    try {
        const auto chunk = luascript::nativeChunk(script.chunk);
        if (luaL_loadbuffer(L, reinterpret_cast<const char*>(chunk.data()), chunk.size(), script.name.c_str()) != 0) {
            error = lua_tostring(L, -1);
            lua_pop(L, 1);
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

} // namespace

LuaRuntime::LuaRuntime() : L_(lua_open()) {
    luaopen_base(L_);
    luaopen_table(L_);
    luaopen_string(L_);
    luaopen_math(L_);
    lua_settop(L_, 0);
    for (const char* name : kRemovedGlobals) {
        lua_pushstring(L_, name);
        lua_pushnil(L_);
        lua_settable(L_, LUA_GLOBALSINDEX);
    }
    for (const auto& f : kFunctions) {
        lua_pushstring(L_, f.name);
        pushGuarded(L_, f.function);
        lua_settable(L_, LUA_GLOBALSINDEX);
    }
    for (const char* name : kNotYetImplemented) {
        lua_pushstring(L_, name);
        lua_pushstring(L_, name);
        lua_pushcclosure(L_, unsupported, 1);
        lua_settable(L_, LUA_GLOBALSINDEX);
    }
    // Classes: a global table of methods and constants, which is also the instances' __index.
    for (const auto& cls : kClasses) {
        const char* separator = cls.isClass ? ":" : ".";
        lua_newtable(L_);
        const int methods = lua_gettop(L_);
        for (const auto& f : cls.methods) {
            lua_pushstring(L_, f.name);
            pushGuarded(L_, f.function);
            lua_settable(L_, methods);
        }
        for (const char* name : cls.notYetImplemented) {
            const std::string qualified = std::string(cls.name) + separator + name;
            lua_pushstring(L_, name);
            lua_pushstring(L_, qualified.c_str());
            lua_pushcclosure(L_, unsupported, 1);
            lua_settable(L_, methods);
        }
        for (const auto& k : cls.constants) {
            lua_pushstring(L_, k.name);
            lua_pushnumber(L_, k.value);
            lua_settable(L_, methods);
        }
        if (cls.isClass) {
            lua_pushstring(L_, "new");
            if (cls.constructor) {
                pushGuarded(L_, cls.constructor);
            } else {
                const std::string qualified = std::string(cls.name) + ".new";
                lua_pushstring(L_, qualified.c_str());
                lua_pushcclosure(L_, unsupported, 1);
            }
            lua_settable(L_, methods);
            luaL_newmetatable(L_, cls.name);
            lua_pushstring(L_, "__index");
            lua_pushvalue(L_, methods);
            lua_settable(L_, -3);
            if (cls.finalizer) {
                lua_pushstring(L_, "__gc");
                lua_pushcfunction(L_, cls.finalizer);
                lua_settable(L_, -3);
            }
            lua_pop(L_, 1);
        }
        lua_pushstring(L_, cls.name);
        lua_insert(L_, -2);
        lua_settable(L_, LUA_GLOBALSINDEX);
    }
    lua_pushlightuserdata(L_, &kRuntimeKey);
    lua_pushlightuserdata(L_, this);
    lua_rawset(L_, LUA_REGISTRYINDEX);
}

LuaRuntime::~LuaRuntime() {
    lua_close(L_);
}

void LuaRuntime::load(const std::vector<luascript::Script>& modules, const std::vector<luascript::Script>& scripts,
                      CodeSource* code) {
    ScriptCall setup;
    setup.code = code;
    lua_pushlightuserdata(L_, &kCallKey);
    lua_pushlightuserdata(L_, &setup);
    lua_rawset(L_, LUA_REGISTRYINDEX);
    std::string error;
    for (const auto& m : modules) {
        if (!loadChunk(L_, m, error)) {
            problems.push_back("module " + m.name + ": " + error);
            continue;
        }
        setup.unsupported.clear();
        if (lua_pcall(L_, 0, 0, 0) != 0) {
            const std::string why = setup.unsupported.empty() ? lua_tostring(L_, -1) : "needs " + setup.unsupported;
            problems.push_back("module " + m.name + ": " + why);
            lua_pop(L_, 1);
        }
    }
    for (const auto& s : scripts) {
        if (!loadChunk(L_, s, error)) {
            problems.push_back("script " + s.name + ": " + error);
            continue;
        }
        lua_pushstring(L_, s.name.c_str());
        lua_insert(L_, -2);
        lua_settable(L_, LUA_GLOBALSINDEX);
    }
    lua_settop(L_, 0);
}

bool LuaRuntime::hasScript(const std::string& name) {
    lua_pushstring(L_, name.c_str());
    lua_gettable(L_, LUA_GLOBALSINDEX);
    const bool found = lua_isfunction(L_, -1);
    lua_pop(L_, 1);
    return found;
}

bool LuaRuntime::run(const std::string& name, ScriptCall& c) {
    lua_pushlightuserdata(L_, &kCallKey);
    lua_pushlightuserdata(L_, &c);
    lua_rawset(L_, LUA_REGISTRYINDEX);
    lua_pushstring(L_, name.c_str());
    lua_gettable(L_, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        c.luaError = "no script named " + name;
        return false;
    }
    if (lua_pcall(L_, 0, 0, 0) != 0) {
        if (c.unsupported.empty() && c.errorCode == 0) {
            c.luaError = lua_tostring(L_, -1);
        }
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

} // namespace ts2::sim
