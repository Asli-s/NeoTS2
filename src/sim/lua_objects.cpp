#include "sim/lua_internals.hpp"

namespace ts2::sim::lua_detail {

namespace {

// Object, Sim and neighbor values.

constexpr std::int16_t kErrorAttributeOutOfRange = 6;

constexpr std::int16_t kErrorMotiveOutOfRange = 0x1A;

constexpr std::int16_t kErrorSlotOutOfRange = 0x1B;

constexpr std::int16_t kErrorPersonDataOutOfRange = 0x21;

constexpr std::int16_t kErrorDefinitionFieldOutOfRange = 0x23;

constexpr std::int16_t kErrorArrayOutOfRange = 0x6C;

// An element of a fixed-size value array, or the error code when the index is outside it.
template <typename Array>
auto& element(lua_State* L, Array& values, int index, std::int16_t error) {
    if (index < 0 || static_cast<std::size_t>(index) >= values.size()) {
        gameError(L, error);
    }
    return values[static_cast<std::size_t>(index)];
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

// GetNeighborData(neighborId, field): one of the neighbor's eleven "neighbor data" values (as the
// "neighbor in stack object" owner reads them). A field of 11 or more is error 0x14 (the game lets
// negative fields through to the lookup, which is not read: refused here too).
constexpr std::int16_t kErrorNeighborDataOutOfRange = 0x14;

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

SimObject& personOrError(lua_State* L, std::int16_t id) {
    ScriptCall& c = call(L);
    SimObject* o = c.world ? c.world->find(id) : nullptr;
    if (o == nullptr || !o->isPerson()) {
        gameError(L, kErrorNotAPerson);
    }
    return *o;
}

// CachedNeighbor:getPersonData(index) / setPersonData(index, value) work on the neighbor's own
// copy of its person data (not the lot Sim's). An index outside 0-230 reads 0 and writes nothing.
// getGUID() is the neighbor's GUID as a signed 32-bit number.
std::vector<std::int16_t>* cachedNeighborData(lua_State* L) {
    const auto* self = selfInstance(L);
    ScriptCall& c = call(L);
    return self != nullptr && c.world != nullptr ? c.world->neighborPersonData(static_cast<std::int16_t>(self->id)) : nullptr;
}

} // namespace

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
    const int field = argInt(L, 2);
    lua_pushnumber(L, element(L, type->definition.fields, field, kErrorDefinitionFieldOutOfRange));
    return 1;
}

// Get/SetPersonMotive(id, motive[, value]): a Sim's motive 0-15, read as a whole number.
int getPersonMotive(lua_State* L) {
    SimObject& o = personOrError(L, argShort(L, 1));
    lua_pushnumber(L, static_cast<int>(element(L, o.motives, argInt(L, 2), kErrorMotiveOutOfRange)));
    return 1;
}

int setPersonMotive(lua_State* L) {
    SimObject& o = personOrError(L, argShort(L, 1));
    element(L, o.motives, argInt(L, 2), kErrorMotiveOutOfRange) =
        static_cast<float>(argInt(L, 3));
    return 0;
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

int getNeighborData(lua_State* L) {
    ScriptCall& c = call(L);
    const std::int16_t field = argShort(L, 2);
    if (field >= World::kNeighborDataCount) {
        return gameError(L, kErrorNeighborDataOutOfRange);
    }
    if (field < 0) {  // the game errors only past the last field, but below the first there is no value
        lua_pushnumber(L, 0);
        return 1;
    }
    if (c.world == nullptr || c.world->neighbors() == nullptr) {
        return stopUnsupported(L, "GetNeighborData without a neighborhood");
    }
    const auto value = c.world->neighborData(argShort(L, 1), static_cast<std::uint16_t>(field));
    if (!value) {
        const std::string what = "GetNeighborData field " + std::to_string(field);
        return stopUnsupported(L, what.c_str());
    }
    lua_pushnumber(L, *value);
    return 1;
}

// SetSimulatorGlobal(index, value): one of the 60 simulator globals, otherwise error 0x3F.
int setSimulatorGlobal(lua_State* L) {
    ScriptCall& c = call(L);
    const int i = argInt(L, 1);
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
    const auto& values = element(L, o.arrays, argInt(L, 2), kErrorArrayOutOfRange);
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
    auto& values = element(L, o.arrays, argInt(L, 2), kErrorArrayOutOfRange);
    values.clear();
    if (lua_type(L, 3) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {
            if (values.size() >= kMaxScriptList) {
                return scriptCheckError(L, "Too many array values.");
            }
            values.push_back(static_cast<std::int16_t>(argInt(L, -1)));
            lua_pop(L, 1);
        }
    }
    return 0;
}

// Classes whose constructors take no arguments.
int newNeighborhood(lua_State* L) { return pushInstance(L, "Neighborhood", 0); }

// CachedNeighbor.new(neighborId): an unknown neighbor is error 0x37.
int newCachedNeighbor(lua_State* L) {
    ScriptCall& c = call(L);
    const std::int16_t id = argShort(L, 1);
    if (c.world == nullptr || c.world->neighbor(id) == nullptr) {
        return gameError(L, kErrorMissingNeighbor);
    }
    return pushInstance(L, "CachedNeighbor", id);
}

// CachedFamily.new(familyId): a family of the neighborhood (its FAMI instance). Anything else is
// error 0x9A.
int newCachedFamily(lua_State* L) {
    ScriptCall& c = call(L);
    const std::int32_t id = argInt(L, 1);
    if (c.world == nullptr || id < 0 || !c.world->hasFamily(static_cast<std::uint32_t>(id))) {
        return gameError(L, kErrorMissingFamily);
    }
    return pushInstance(L, "CachedFamily", id);
}

namespace {

std::optional<World::FamilyLotIds> selfFamilyLots(lua_State* L) {
    const auto* self = selfInstance(L);
    const ScriptCall& c = call(L);
    if (self == nullptr || c.world == nullptr || self->id < 0) {
        return std::nullopt;
    }
    return c.world->familyLotIds(static_cast<std::uint32_t>(self->id));
}

} // namespace

// CachedFamily:getId(), getLotId(), getCurrentLotId() and getVacationLotId(): the family's id and the
// three lot ids its FAMI saves (home lot, current lot, vacation lot), 0 for none.
int cachedFamilyId(lua_State* L) {
    const auto* self = selfInstance(L);
    lua_pushnumber(L, self != nullptr ? self->id : 0);
    return 1;
}

int cachedFamilyLotId(lua_State* L) {
    const auto lots = selfFamilyLots(L);
    lua_pushnumber(L, lots ? lots->home : 0);
    return 1;
}

int cachedFamilyCurrentLotId(lua_State* L) {
    const auto lots = selfFamilyLots(L);
    lua_pushnumber(L, lots ? lots->current : 0);
    return 1;
}

int cachedFamilyVacationLotId(lua_State* L) {
    const auto lots = selfFamilyLots(L);
    lua_pushnumber(L, lots ? lots->vacation : 0);
    return 1;
}

namespace {

std::span<const std::uint32_t> selfFamilyMembers(lua_State* L) {
    const auto* self = selfInstance(L);
    const ScriptCall& c = call(L);
    if (self == nullptr || c.world == nullptr || self->id < 0) {
        return {};
    }
    return c.world->familyMemberGuids(static_cast<std::uint32_t>(self->id));
}

} // namespace

// CachedFamily:getMembers(): an array of the neighbor ids of the family's Sims, in the family's
// order. A member with no neighbor, or a family-tree-only one, is left out.
int cachedFamilyMembers(lua_State* L) {
    const ScriptCall& c = call(L);
    lua_newtable(L);
    int n = 0;
    for (const std::uint32_t guid : selfFamilyMembers(L)) {
        const std::int16_t id = guid != 0 ? c.world->neighborIdWithGuid(guid) : 0;
        const SavedNeighbor* neighbor = id != 0 ? c.world->neighbor(id) : nullptr;
        if (neighbor == nullptr || isFamilyTreeOnly(c, *neighbor)) {
            continue;
        }
        lua_pushnumber(L, ++n);
        lua_pushnumber(L, id);
        lua_settable(L, -3);
    }
    return 1;
}

// CachedFamily:getNumMembers(): how many Sims the family lists, all of them counted.
int cachedFamilyMemberCount(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(selfFamilyMembers(L).size()));
    return 1;
}

// CachedPerson.new(objectId): a Sim on the lot. Anything else is error 0x1C.
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

// Neighborhood:isApartmentResident(objectId): on an apartment building or one of its apartments
// (lot types 8 and 9), whether the object is a Sim whose person data 0x20 ("Person Type") is 8.
// False on other lots.
int neighborhoodIsApartmentResident(lua_State* L) {
    ScriptCall& c = call(L);
    const std::int16_t id = argShort(L, 2);
    const std::uint8_t type = c.world ? c.world->lotType() : 0;
    if (type != kApartmentBuildingLot && type != kApartmentLot) {
        lua_pushboolean(L, false);
        return 1;
    }
    constexpr std::int16_t kApartmentResidentType = 8;
    const SimObject* sim = personOnLot(c, id);
    lua_pushboolean(L, sim != nullptr && sim->personData[0x20] == kApartmentResidentType);
    return 1;
}

// Neighborhood:isApartmentDoor(objectId): on an apartment building or one of its apartments (lot
// types 8 and 9), whether the object is a door (object data 0x3B, its category, is 0x1A) whose OBJD
// field 0x4B is 0xCC. Objects queued for delete still count. False on other lots.
int neighborhoodIsApartmentDoor(lua_State* L) {
    ScriptCall& c = call(L);
    const std::int16_t id = argShort(L, 2);
    const std::uint8_t type = c.world ? c.world->lotType() : 0;
    constexpr std::size_t kCategoryField = 0x3B, kApartmentDoorField = 0x4B;
    constexpr std::int16_t kDoorCategory = 0x1A;
    constexpr std::uint16_t kApartmentDoorValue = 0xCC;
    bool door = false;
    if (type == kApartmentBuildingLot || type == kApartmentLot) {
        const SimObject* o = c.world->findIncludingRemoved(id);
        door = o != nullptr && o->data[kCategoryField] == kDoorCategory &&
               o->definition.field(kApartmentDoorField) == kApartmentDoorValue;
    }
    lua_pushboolean(L, door);
    return 1;
}

// Neighborhood:isValidLot(lotId): whether the neighborhood has that lot.
int neighborhoodIsValidLot(lua_State* L) {
    ScriptCall& c = call(L);
    const auto* lots = c.world ? c.world->lotIds() : nullptr;
    lua_pushboolean(L, lots != nullptr && lots->contains(argInt(L, 2)));
    return 1;
}

// The game's error for a lot the neighborhood doesn't place.
[[noreturn]] int missingLot(lua_State* L, std::int32_t id) {
    char message[64];
    std::snprintf(message, sizeof message, "Lot with id %d does not exist!", static_cast<int>(id));
    gameError(L, kErrorScriptCheck, message);
}

// Neighborhood:getLotOwner(lotId): the neighbor id its description names as owner (0 for none).
int neighborhoodGetLotOwner(lua_State* L) {
    const ScriptCall& c = call(L);
    const std::int32_t id = argInt(L, 2);
    if (c.world == nullptr || !c.world->placedLotType(id)) {
        return missingLot(L, id);
    }
    lua_pushnumber(L, c.world->placedLotOwner(id));
    return 1;
}

// Neighborhood:getWitchLotType(): 0 unless the current lot is of type 10, whose answer is not read yet.
int neighborhoodGetWitchLotType(lua_State* L) {
    constexpr std::uint8_t kWitchLot = 10;
    const ScriptCall& c = call(L);
    const auto number = c.world ? c.world->lotNumber() : std::nullopt;
    const auto type = number ? c.world->placedLotType(*number) : std::nullopt;
    if (!type) {
        return missingLot(L, number.value_or(0));
    }
    if (*type == kWitchLot) {
        return stopUnsupported(L, "Neighborhood:getWitchLotType on a lot of type 10");
    }
    lua_pushnumber(L, 0);
    return 1;
}

// GetNeighborRelationship(a, b, index, [create]): relationship variable `index` neighbor a holds
// towards neighbor b, as an unsigned 32-bit number, or false past the end. With `create` a missing
// variable is first added as 0 (and nothing is returned for a negative index). A missing neighbor
// is error 0x37.
int getNeighborRelationship(lua_State* L) {
    ScriptCall& c = call(L);
    const std::int16_t a = argShort(L, 1);
    const std::int16_t b = argShort(L, 2);
    const std::int16_t index = argShort(L, 3);
    const bool create = lua_gettop(L) > 3 && lua_toboolean(L, 4) != 0;
    const auto* ids = c.world ? c.world->neighbors() : nullptr;
    if (ids == nullptr) {
        return stopUnsupported(L, "GetNeighborRelationship without a neighborhood");
    }
    if (!ids->contains(a) || !ids->contains(b)) {
        return gameError(L, kErrorMissingNeighbor);
    }
    if (create && (index < 0 || c.world->neighborRelationshipSize(a, b) <= static_cast<std::size_t>(index))) {
        if (index < 0) {
            return 0;
        }
        auto& values = c.world->neighborRelationship(a, b);
        values.resize(static_cast<std::size_t>(index) + 1, 0);
    }
    const auto* values = c.world->neighborRelationshipValues(a, b);
    if (values == nullptr || index < 0 || static_cast<std::size_t>(index) >= values->size()) {
        lua_pushboolean(L, false);
        return 1;
    }
    pushUnsigned(L, static_cast<std::uint32_t>((*values)[static_cast<std::size_t>(index)]));
    return 1;
}

// GetSimulatorGlobal(index): one of the 60 simulator globals, otherwise error 0x3F.
int getSimulatorGlobal(lua_State* L) {
    ScriptCall& c = call(L);
    const int i = argInt(L, 1);
    if (i < 0 || i > 0x3B) {
        return gameError(L, kErrorGlobalOutOfRange);
    }
    lua_pushnumber(L, c.globals && static_cast<std::size_t>(i) < c.globalCount ? c.globals[i] : 0);
    return 1;
}

// GetConstant(table, index) / GetNumConstants(table): the running tree's constants.
int getConstant(lua_State* L) {
    return pushConstant(L, constantsTable(L, call(L).codeOwner, static_cast<std::uint16_t>(argShort(L, 1))),
                        argInt(L, 2));
}

int getNumConstants(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(
                          constantsTable(L, call(L).codeOwner, static_cast<std::uint16_t>(argShort(L, 1))).size()));
    return 1;
}

// The same for an object type by GUID. An unknown GUID is error 0x7A.
int getConstantWithGuid(lua_State* L) {
    const std::uint32_t group = typeGroup(L, argUnsigned(L, 1));
    return pushConstant(L, constantsTable(L, group, static_cast<std::uint16_t>(argShort(L, 2))),
                        argInt(L, 3));
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

// GetObjectData(id, field) / SetObjectData(id, field, value). Fields outside the object's read
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

// CachedPerson:getPersonData(index) / setPersonData(index, value): the same for the Sim it holds.
int cachedPersonGetData(lua_State* L) {
    const auto* self = selfInstance(L);
    SimObject& o = personOrError(L, self ? static_cast<std::int16_t>(self->id) : 0);
    const std::int16_t i = argShort(L, 2);
    lua_pushnumber(L, i >= 0 && static_cast<std::size_t>(i) < o.personData.size() ? o.personData[static_cast<std::size_t>(i)] : 0);
    return 1;
}

// CachedPerson:isQueuedObjAdvertisingForX(motive): whether an interaction in the Sim's queue
// advertises the motive for the Sim's age group, tested as the game tests it: the interaction's line
// has a minimum or a range for that motive. Motives 2 and 10-12, and any outside 0-15, never do.
int cachedPersonQueuedAdvertising(lua_State* L) {
    constexpr int kMotives = 16;
    const auto* self = selfInstance(L);
    ScriptCall& c = call(L);
    const SimObject& sim = personOrError(L, self ? static_cast<std::int16_t>(self->id) : 0);
    const int motive = argInt(L, 2);
    if (sim.personData.size() > kSpeciesPersonData && sim.personData[kSpeciesPersonData] != 0) {
        return stopUnsupported(L, "CachedPerson:isQueuedObjAdvertisingForX for a pet");
    }
    bool advertises = false;
    if (motive >= 0 && motive < kMotives && motive != 2 && (motive < 10 || motive > 12) && c.code != nullptr && c.world != nullptr) {
        const std::size_t group = advertisementGroup(sim);
        for (const QueuedInteraction& q : sim.queue) {
            const SimObject* object = c.world->find(q.object);
            const auto table = object != nullptr ? interactionsOf(*c.code, *object).table : nullptr;
            const ttab::Interaction* line = table ? ttab::find(*table, q.index) : nullptr;
            if (line != nullptr && group < line->motives.size() && static_cast<std::size_t>(motive) < line->motives[group].size()) {
                const ttab::MotiveEffect& e = line->motives[group][static_cast<std::size_t>(motive)];
                if (e.min != 0 || e.delta != 0) {
                    advertises = true;
                    break;
                }
            }
        }
    }
    lua_pushboolean(L, advertises);
    return 1;
}

// CachedPerson:getNumQueuedInteractions(): how many interactions wait in the Sim's queue.
int cachedPersonQueueCount(lua_State* L) {
    const auto* self = selfInstance(L);
    const SimObject& o = personOrError(L, self ? static_cast<std::int16_t>(self->id) : 0);
    lua_pushnumber(L, static_cast<lua_Number>(static_cast<std::int16_t>(o.queue.size())));
    return 1;
}

// CachedPerson:getGUID(): the Sim's object GUID as a signed 32-bit number.
int cachedPersonGuid(lua_State* L) {
    const auto* self = selfInstance(L);
    const SimObject& o = personOrError(L, self ? static_cast<std::int16_t>(self->id) : 0);
    lua_pushnumber(L, static_cast<lua_Number>(static_cast<std::int32_t>(o.definition.guid())));
    return 1;
}

namespace {

// Tokens in a neighbor's inventory that lower or raise the family friends a promotion needs. A
// token counts only with no flags set and category 0.
constexpr std::uint32_t kFriendCreditToken = 0x53FEA775, kFriendDebitToken = 0x74BF3B69;

// Credits from a Sim's tokens against the friends a promotion needs. Each credit token whose first
// value is 0 or 1 is one. One whose first value is 0, 1 or 2 and whose next two name the Sim's
// career (low then high half of its GUID, both non-zero) is one more. Each debit token takes one away.
int friendCredits(const Inventory& inventory, std::int16_t careerLow, std::int16_t careerHigh) {
    int credits = 0;
    for (const auto& token : inventory.singular) {
        if (!token || token->flags != 0 || token->category != 0) {
            continue;
        }
        if (token->guid == kFriendDebitToken) {
            --credits;
            continue;
        }
        if (token->guid != kFriendCreditToken) {
            continue;
        }
        const auto value = [&](std::size_t i) { return i < token->properties.size() ? token->properties[i] : std::int16_t{0}; };
        const std::int16_t kind = value(0), low = value(1), high = value(2);
        if (kind < 0 || kind > 2) {
            continue;
        }
        credits += kind <= 1 ? 1 : 0;
        if (low != 0 && high != 0 && low == careerLow && high == careerHigh) {
            ++credits;
        }
    }
    return credits;
}

} // namespace

// CachedPerson:getFriendsRequiredForPromtion(): the family friends the Sim still needs for the next
// career level. Its career's table 0x1003 gives the friends for each level (0 past its end), less
// the credits from the Sim's neighbor inventory, never below 0. 0 without a career or the table.
int cachedPersonFriendsForPromotion(lua_State* L) {
    constexpr std::size_t kNeighborId = 0x1F, kJobLevel = 0x39, kCareerLow = 0x59, kCareerHigh = 0x5A;
    constexpr std::uint16_t kFriendsTable = 0x1003;
    const auto* self = selfInstance(L);
    ScriptCall& c = call(L);
    const SimObject& sim = personOrError(L, self ? static_cast<std::int16_t>(self->id) : 0);
    const auto data = [&](std::size_t i) { return i < sim.personData.size() ? sim.personData[i] : std::int16_t{0}; };
    const std::int16_t careerLow = data(kCareerLow), careerHigh = data(kCareerHigh);
    const std::uint32_t career = static_cast<std::uint32_t>(static_cast<std::uint16_t>(careerHigh)) << 16 | static_cast<std::uint16_t>(careerLow);
    const ObjectType* type = career != 0 && c.code != nullptr ? c.code->objectType(career) : nullptr;
    const std::vector<std::int16_t>* friends = type != nullptr ? c.code->constants(type->group, kFriendsTable) : nullptr;
    std::int16_t needed = 0;
    if (friends != nullptr) {
        const int next = data(kJobLevel) + 1;
        const std::int16_t perLevel = next >= 0 && static_cast<std::size_t>(next) < friends->size() ? (*friends)[static_cast<std::size_t>(next)] : 0;
        const Inventory* inventory = c.world != nullptr ? c.world->inventory(InventoryKind::Neighbor, data(kNeighborId)) : nullptr;
        const int credits = inventory != nullptr ? friendCredits(*inventory, careerLow, careerHigh) : 0;
        needed = static_cast<std::int16_t>(perLevel - credits);
    }
    lua_pushnumber(L, needed > 0 ? needed : 0);
    return 1;
}

int cachedPersonSetData(lua_State* L) {
    const auto* self = selfInstance(L);
    SimObject& o = personOrError(L, self ? static_cast<std::int16_t>(self->id) : 0);
    const std::int16_t i = argShort(L, 2);
    if (i >= 0 && static_cast<std::size_t>(i) < o.personData.size()) {
        o.personData[static_cast<std::size_t>(i)] = argShort(L, 3);
    }
    return 0;
}

int cachedNeighborGetData(lua_State* L) {
    const auto* data = cachedNeighborData(L);
    const std::int16_t i = argShort(L, 2);
    const bool inRange = data != nullptr && i >= 0 && static_cast<std::size_t>(i) < std::min(kNeighborPersonDataCount, data->size());
    lua_pushnumber(L, inRange ? (*data)[static_cast<std::size_t>(i)] : 0);
    return 1;
}

int cachedNeighborSetData(lua_State* L) {
    auto* data = cachedNeighborData(L);
    const std::int16_t i = argShort(L, 2);
    if (data != nullptr && i >= 0 && static_cast<std::size_t>(i) < std::min(kNeighborPersonDataCount, data->size())) {
        (*data)[static_cast<std::size_t>(i)] = argShort(L, 3);
    }
    return 0;
}

int cachedNeighborGuid(lua_State* L) {
    const auto* self = selfInstance(L);
    ScriptCall& c = call(L);
    const SavedNeighbor* n = self != nullptr && c.world != nullptr ? c.world->neighbor(static_cast<std::int16_t>(self->id)) : nullptr;
    lua_pushnumber(L, n != nullptr ? static_cast<std::int32_t>(n->guid) : 0);
    return 1;
}

// CachedNeighbor:getOutfitProperty(category, property): a small number the game works out from the
// neighbor's age data. Hair (0), skin (1) and eye colour (2) are 2-7 for the game's own colours and
// 1 for any other. 8 and anything above 11 are 0. The others come from the neighbor's looks (the
// world's outfit property).
int cachedNeighborOutfitProperty(lua_State* L) {
    static constexpr std::array<std::array<std::string_view, 6>, 3> kColours{{
        {"00000001-0000-0000-0000-000000000000", "00000002-0000-0000-0000-000000000000",
         "00000003-0000-0000-0000-000000000000", "00000004-0000-0000-0000-000000000000",
         "00000005-0000-0000-0000-000000000000", "00000006-0000-0000-0000-000000000000"},
        {"00000001-0000-0000-0000-000000000000", "00000002-0000-0000-0000-000000000000",
         "00000003-0000-0000-0000-000000000000", "00000004-0000-0000-0000-000000000000",
         "6baf064a-85ad-4e37-8d81-a987e9f8da46", "b6ee1dbc-5bb3-4146-8315-02bd64eda707"},
        {"51c4a750-c9f4-4cfe-801c-898efc360cb7", "32dee745-b6ce-419f-9e86-ae93802d2682",
         "e43f3360-3a08-4755-8b83-a0d37a6c424b", "2d6839c5-0b7c-48a1-9c55-4bd9cc873b0f",
         "0758508c-7111-40f9-b33b-706464626ac9", "12d4f3e1-fdbe-4fe7-ace3-46dd9ff52b51"},
    }};
    static const std::array<const char*, 3> kColourKeys{"haircolor", "skincolor", "eyecolor"};
    const auto* self = selfInstance(L);
    ScriptCall& c = call(L);
    const auto property = static_cast<std::uint16_t>(argInt(L, 3));
    const SavedNeighbor* n = self != nullptr && c.world != nullptr ? c.world->neighbor(static_cast<std::int16_t>(self->id)) : nullptr;
    const ObjectType* type = n != nullptr && c.code != nullptr ? c.code->objectType(n->guid) : nullptr;
    const auto* age = type != nullptr ? c.code->ageData(type->group) : nullptr;
    auto value = [&](const char* key) {
        const auto it = age->find(key);
        return it != age->end() ? it->second : std::string();
    };
    if (self == nullptr || c.world == nullptr || age == nullptr || property > 11 || property == 8) {
        lua_pushnumber(L, 0);
        return 1;
    }
    if (property <= 2) {
        std::string colour = value(kColourKeys[property]);
        std::transform(colour.begin(), colour.end(), colour.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const auto& known = kColours[property];
        const auto at = std::find(known.begin(), known.end(), colour);
        lua_pushnumber(L, at != known.end() ? static_cast<lua_Number>(at - known.begin() + 2) : 1);
        return 1;
    }
    if (const auto& looks = c.world->outfitProperty()) {
        if (const auto v = looks(static_cast<std::int16_t>(self->id), static_cast<std::uint16_t>(argInt(L, 2)), property)) {
            lua_pushnumber(L, *v);
            return 1;
        }
    }
    c.unsupported = "CachedNeighbor:getOutfitProperty " + std::to_string(property);
    lua_pushstring(L, "unsupported script function");
    return lua_error(L);
}

} // namespace ts2::sim::lua_detail
