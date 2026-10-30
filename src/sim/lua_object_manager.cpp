#include "sim/lua_internals.hpp"

namespace ts2::sim::lua_detail {

namespace {

SimObject& personOrError(lua_State* L, std::int16_t id);

// ObjectManager methods (called as GlobalObjManager:method(...), so arguments start at 2).

constexpr std::int16_t kErrorDataOutOfRange = 7;

bool searchable(const SimObject& o, bool includeSkipped) {
    return includeSkipped || (o.data[kSearchSkipFlagsField] & 0x05) == 0;
}

// The parts of the multi-tile object that object `id` belongs to (they share its lead's id in object
// data 0x13), in falling id order as Set to Next walks them.
int pushMultiTileParts(lua_State* L, std::int16_t id, bool includeSkipped) {
    constexpr std::size_t kLeadTileObjectField = 0x13;
    ScriptCall& c = call(L);
    const SimObject* o = c.world != nullptr ? c.world->find(id) : nullptr;
    if (o == nullptr) {
        return scriptCheckError(L, "Could not find object: %d", static_cast<int>(id));
    }
    if (o->definition.field(objd::field::kMultiTileMasterId) == 0) {
        return scriptCheckError(L, "Object is not a multi-tile object.");
    }
    const std::int16_t lead = o->data[kLeadTileObjectField];
    std::vector<std::int16_t> parts;
    c.world->nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& s) {
        const bool part = s.id == id || (lead > 0 && s.data[kLeadTileObjectField] == lead &&
                                         s.definition.field(objd::field::kMultiTileMasterId) != 0);
        if (part && searchable(s, includeSkipped)) {
            parts.push_back(s.id);
        }
        return false;
    });
    lua_newtable(L);
    int n = 0;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        lua_pushnumber(L, ++n);
        lua_pushnumber(L, *it);
        lua_settable(L, -3);
    }
    return 1;
}

// The first object (Sim, for getPersonIdFromGUID) of a type, 0 when there is none. An unknown
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

// Data tables (ObjectDataTable, NeighborDataTable, nDataTable).

} // namespace

// Whether the value at index is a userdata whose metatable is the class's own. Scripts can't make
// a userdata or reach a class's metatable, so this tells the C++ type apart.
bool isClassUserdata(lua_State* L, int index, const char* className) {
    if (lua_type(L, index) != LUA_TUSERDATA || lua_getmetatable(L, index) == 0) {
        return false;
    }
    luaL_getmetatable(L, className);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same;
}

// The Instance a method is called on (self, argument 1), null when argument 1 is not one.
const Instance* selfInstance(lua_State* L) {
    if (lua_type(L, 1) != LUA_TUSERDATA || isClassUserdata(L, 1, kTokenClass) || lua_getmetatable(L, 1) == 0) {
        return nullptr;
    }
    lua_pushstring(L, kInstanceMarker);
    lua_rawget(L, -2);
    const bool instance = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    return instance ? static_cast<const Instance*>(lua_touserdata(L, 1)) : nullptr;
}

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

int objectManagerGetObjectIdFromGuid(lua_State* L) { return firstOfType(L, false); }

int objectManagerGetPersonIdFromGuid(lua_State* L) { return firstOfType(L, true); }

// getPersonIds([includeSkipped], [speciesMask]): the Sims on the lot. A species mask keeps those
// whose bit 1 << species is set.
int objectManagerGetPersonIds(lua_State* L) {
    const bool includeSkipped = lua_gettop(L) >= 2 && lua_type(L, 2) == LUA_TBOOLEAN && lua_toboolean(L, 2);
    const std::uint32_t mask = lua_gettop(L) >= 3 && lua_type(L, 3) == LUA_TNUMBER ? argUnsigned(L, 3) : 0;
    return pushObjectIds(L, [&](const SimObject& s) {
        return s.isPerson() && searchable(s, includeSkipped) &&
               (mask == 0 || (mask & (1u << (s.personData[kSpeciesPersonData] & 0x1F))) != 0);
    });
}

// getObjectIds(query, values..., [includeSkipped]): kQuery_All, kQuery_ByGUID (values: the GUID),
// kQuery_ForPartsofMultiTileObject (values: an object id) or kQuery_ByObjectDataField (values: field,
// value).
int objectManagerGetObjectIds(lua_State* L) {
    const int top = lua_gettop(L);
    const int query = top >= 2 ? argInt(L, 2) : 0;
    std::int64_t values[2] = {0, 0};
    int count = 0;
    bool includeSkipped = false;
    for (int i = 3; i <= top; ++i) {
        if (lua_type(L, i) == LUA_TNUMBER && count < 2) {
            values[count++] = argInteger(L, i);
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
    case 2:
        if (count == 0) {
            return scriptCheckError(L, "Missing object id parameter searching for parts of multi-tile object.");
        }
        return pushMultiTileParts(L, static_cast<std::int16_t>(values[0]), includeSkipped);
    case 3: {
        if (values[0] > 0x71 || values[0] < 0) {
            return gameError(L, kErrorDataOutOfRange);
        }
        const auto field = static_cast<std::size_t>(values[0]);
        const auto value = static_cast<std::int16_t>(values[1]);
        return pushObjectIds(L, [&](const SimObject& s) { return s.data[field] == value && searchable(s, includeSkipped); });
    }
    default:
        return stopUnsupported(L, "ObjectManager:getObjectIds(kQuery_ByAccessibleRooms)");
    }
}

// Whether a neighbor's object type is 0x11, a Sim who exists only in family trees. Scripts listing
// neighbors leave these out.
bool isFamilyTreeOnly(const ScriptCall& c, const SavedNeighbor& neighbor) {
    constexpr std::uint16_t kFamilyTreeOnlyType = 0x11;
    const ObjectType* type = c.code != nullptr ? c.code->objectType(neighbor.guid) : nullptr;
    return type != nullptr && type->definition.field(objd::field::kType) == kFamilyTreeOnlyType;
}

// getNeighborIds([species mask]): the neighborhood's neighbor ids as an array, in the order the
// game lists them. Family-tree-only neighbors are left out. A number argument is a mask of species (person data 0xBA, bit 1 << species).
// Anything else, or 0, takes every species.
int objectManagerGetNeighborIds(lua_State* L) {
    ScriptCall& c = call(L);
    std::int32_t mask = 0;
    if (lua_gettop(L) >= 2 && lua_type(L, 2) == LUA_TNUMBER) {
        const double value = lua_tonumber(L, 2);
        mask = value > -2147483649.0 && value < 2147483648.0 ? static_cast<std::int32_t>(value)
                                                                : std::numeric_limits<std::int32_t>::min();
    }
    lua_newtable(L);
    int n = 0;
    if (const auto* ids = c.world ? c.world->neighbors() : nullptr) {
        for (const std::int16_t id : *ids) {
            const SavedNeighbor* saved = c.world->neighbor(id);
            if (saved != nullptr && isFamilyTreeOnly(c, *saved)) {
                continue;
            }
            const auto species = static_cast<std::uint32_t>(c.world->neighborPersonValue(id, kSpeciesPersonData)) & 31;
            if (mask != 0 && (static_cast<std::uint32_t>(mask) & (1u << species)) == 0) {
                continue;
            }
            lua_pushnumber(L, ++n);
            lua_pushnumber(L, id);
            lua_settable(L, -3);
        }
    }
    return 1;
}

int newObjectManager(lua_State* L) { return pushInstance(L, "ObjectManager", 0); }

} // namespace ts2::sim::lua_detail
