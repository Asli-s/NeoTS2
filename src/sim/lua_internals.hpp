// What the files of the Lua runtime share: lua_runtime.cpp runs scripts in their sandbox and
// registers the game's script functions, which the lua_*.cpp files implement by group.
#pragma once

#include "sim/lua_runtime.hpp"
#include "sim/advertisement.hpp"
#include "sim/code_source.hpp"
#include "sim/pie_menu.hpp"
#include "sim/lua_error.h"
#include "sim/world.hpp"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ts2::sim::lua_detail {

// The sandbox: guarded calls, the running call, argument checks (lua_runtime.cpp).
inline char kRuntimeKey;
constexpr std::int16_t kErrorTempOutOfRange = 2;
constexpr std::int16_t kErrorParamOutOfRange = 8;
constexpr std::int16_t kErrorLocalOutOfRange = 0x33;
constexpr std::int16_t kErrorScriptCheck = 0x9B;

// Lot types (content::Neighborhood::lotTypes).
constexpr std::uint8_t kApartmentBuildingLot = 8;
constexpr std::uint8_t kApartmentLot = 9;

// Script functions stop a script by throwing ScriptStop. guarded() catches it (and any other C++
// exception, such as running out of memory) and turns it into a Lua error with its message. Lua's
// own errors (LuaThrow, from a Lua call inside a function) pass through unchanged.
struct ScriptStop {
    char message[256];
};

[[noreturn]] inline void stopScript(const char* message) {
    ScriptStop stop{};
    std::snprintf(stop.message, sizeof stop.message, "%s", message);
    throw stop;
}

ScriptCall& call(lua_State* L);
std::int64_t wholeNumber(double v);
std::int64_t argInteger(lua_State* L, int i);
float singleOf(double v);
std::int16_t argShort(lua_State* L, int i);
std::int32_t argInt(lua_State* L, int i);
std::uint32_t argUnsigned(lua_State* L, int i);
void pushUnsigned(lua_State* L, std::uint32_t value);

[[noreturn]] inline int gameError(lua_State* L, std::int16_t code, const char* message = "Object Error running script!") {
    call(L).errorCode = code;
    stopScript(message);
}

// Stops the script at a script function (or a use of one) the engine doesn't have yet.
[[noreturn]] inline int stopUnsupported(lua_State* L, const char* what) {
    call(L).unsupported = what;
    stopScript("unsupported script function");
}
int unsupported(lua_State* L);
constexpr std::int16_t kErrorNotAPerson = 0x1C;
constexpr std::int16_t kErrorInvalidConstant = 0x2E;
constexpr std::int16_t kErrorMissingNeighbor = 0x37;
constexpr std::int16_t kErrorGlobalOutOfRange = 0x3F;
constexpr std::int16_t kErrorBadGuid = 0x7A;
constexpr std::int16_t kErrorMissingFamily = 0x9A;

// Script results, temps, params, locals, bits and GUIDs (lua_script_values.cpp).
int setScriptReturnValue(lua_State* L);
int getStackObjectId(lua_State* L);
int getPrimitiveParameter(lua_State* L);
int getTemp(lua_State* L);
int setTemp(lua_State* L);
int getLocal(lua_State* L);
int setLocal(lua_State* L);
int getTreeParameter(lua_State* L);
int setTreeParameter(lua_State* L);
int doNothing(lua_State*);
int isBitSet(lua_State* L);
int setBit(lua_State* L);
int clearBit(lua_State* L);
int bitwiseAnd(lua_State* L);
int bitwiseOr(lua_State* L);
int bitwiseXOr(lua_State* L);
int bitShiftLeft(lua_State* L);
int bitShiftRight(lua_State* L);
int createObjectGuid(lua_State* L);
int splitObjectGuid(lua_State* L);
int roundFloat(lua_State* L);

// Messages, game properties, installed packs and businesses (lua_game_state.cpp).
int postMessage(lua_State* L);
int subscribeMessage(lua_State* L);
int getProperty(lua_State* L);
int setProperty(lua_State* L);
int isEpInstalled(lua_State* L);
int getInstalledEps(lua_State* L);
int lotHasBusiness(lua_State* L);

// Class instances and the ObjectManager searches (lua_object_manager.cpp).
// An instance of a script class: what its constructor bound (an object or neighbor id).
struct Instance {
    std::int32_t id = 0;
    bool neighbor = false;  // for data tables: a neighbor's tables rather than an object's
    std::int32_t kind = 0;  // for inventories: which inventory (InventoryKind)
};

// Metatables of classes whose instances are an Instance carry this field, so that a method can
// tell its self apart from other script objects.
constexpr const char* kInstanceMarker = "__ts2instance";
constexpr const char* kTokenClass = "InventoryToken";
bool isClassUserdata(lua_State* L, int index, const char* className);
const Instance* selfInstance(lua_State* L);
int pushInstance(lua_State* L, const char* className, std::int32_t id);
const SimObject* personOnLot(ScriptCall& c, std::int16_t id);
constexpr std::size_t kSearchSkipFlagsField = 0x38;
constexpr std::size_t kSpeciesPersonData = 0xBA;

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
int objectManagerVerifyObjectId(lua_State* L);
int objectManagerVerifyNeighborId(lua_State* L);
int objectManagerIsValidObjectGuid(lua_State* L);
int objectManagerGetObjectGuidFromObjectId(lua_State* L);
int objectManagerGetObjectIdFromGuid(lua_State* L);
int objectManagerGetPersonIdFromGuid(lua_State* L);
int objectManagerGetPersonIds(lua_State* L);
int objectManagerGetObjectIds(lua_State* L);
int objectManagerGetNeighborIds(lua_State* L);

template <typename... Args>
[[noreturn]] int scriptCheckError(lua_State* L, const char* format, Args... args) {
    char message[256];
    if constexpr (sizeof...(args) == 0) {
        std::snprintf(message, sizeof message, "%s", format);  // a message without values is used as it is
    } else {
        std::snprintf(message, sizeof message, format, args...);
    }
    gameError(L, kErrorScriptCheck, message);
}

int newObjectManager(lua_State* L);

// Object and neighbor data tables (lua_data_tables.cpp).
// Lists scripts fill hold at most as many values as a 16-bit index can name (a limit of ours).
constexpr std::size_t kMaxScriptList = 0x7FFF;
int newObjectDataTable(lua_State* L);
int newNeighborDataTable(lua_State* L);
int dataTableNew(lua_State* L);
int dataTableRemove(lua_State* L);
int dataTableClear(lua_State* L);
int dataTableHas(lua_State* L);
int dataTableRemoveAll(lua_State* L);
int dataTableIds(lua_State* L);
int dataTableKeys(lua_State* L);
int dataTableValues(lua_State* L);
int dataTableCopy(lua_State* L);
int dataTableSetFromCopy(lua_State* L);
int dataTableGetValue(lua_State* L);
int dataTableSetValue(lua_State* L);
int dataTableSetValueAsInt(lua_State* L);
int dataTableRemoveValue(lua_State* L);
int dataTableHasValue(lua_State* L);
int dataTableGetName(lua_State* L);
int dataTableSetName(lua_State* L);
int getObjectTableValue(lua_State* L);
int getNeighborTableValue(lua_State* L);

// Objects, people, neighbors, constants and globals (lua_objects.cpp).
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
int getNeighborRelationship(lua_State* L);
int getNeighborData(lua_State* L);
int setSimulatorGlobal(lua_State* L);
int getNumObjectArrays(lua_State* L);
int getObjectArrayCopy(lua_State* L);
int setObjectArray(lua_State* L);
int newNeighborhood(lua_State* L);
int newCachedNeighbor(lua_State* L);
int newCachedPerson(lua_State* L);
int newCachedFamily(lua_State* L);
int cachedFamilyId(lua_State* L);
int cachedFamilyLotId(lua_State* L);
int cachedFamilyCurrentLotId(lua_State* L);
int cachedFamilyVacationLotId(lua_State* L);
int neighborhoodIsRoommate(lua_State* L);
int neighborhoodIsApartmentDoor(lua_State* L);
int neighborhoodIsApartmentResident(lua_State* L);
int neighborhoodGetLotOwner(lua_State* L);
int neighborhoodGetWitchLotType(lua_State* L);
int neighborhoodIsValidLot(lua_State* L);
int getSimulatorGlobal(lua_State* L);
int getConstant(lua_State* L);
int getNumConstants(lua_State* L);
int getConstantWithGuid(lua_State* L);
int getNumConstantsWithGuid(lua_State* L);
int getConstantsTableByGuid(lua_State* L);
SimObject& objectOrError(lua_State* L, std::int16_t id);
int getObjectData(lua_State* L);
int setObjectData(lua_State* L);
int getPersonData(lua_State* L);
int setPersonData(lua_State* L);
int cachedPersonGetData(lua_State* L);
int cachedPersonQueuedAdvertising(lua_State* L);
int cachedPersonQueueCount(lua_State* L);
int cachedPersonGuid(lua_State* L);
int cachedPersonSetData(lua_State* L);
int cachedNeighborGetData(lua_State* L);
int cachedNeighborSetData(lua_State* L);
int cachedNeighborGuid(lua_State* L);
int cachedNeighborOutfitProperty(lua_State* L);

// Floors, tiles, locations, weather and the camera (lua_world.cpp).
int hasSolidFloor(lua_State* L);
int hasStage(lua_State* L);
int getNumWallSections(lua_State* L);
int getRoomOnSideOfWall(lua_State* L);
int isBeachLocation(lua_State* L);
int getObjectsOnTile(lua_State* L);
int getObjectTile(lua_State* L);
int getObjectLocation(lua_State* L);
int isOutsideForWeather(lua_State* L);
int newWeatherInfo(lua_State* L);
int newNeighborhoodManager(lua_State* L);
int neighborhoodManagerIsValid(lua_State* L);
int neighborhoodManagerCountOfType(lua_State* L);
int neighborhoodManagerType(lua_State* L);
int neighborhoodManagerCurrentType(lua_State* L);
int neighborhoodManagerSeasonOfQuadrant(lua_State* L);

template <weather::Weather::Value V>
int getWeather(lua_State* L) {
    const ScriptCall& c = call(L);
    lua_pushnumber(L, c.world ? c.world->weather().values[V] : 0);
    return 1;
}

template <weather::Weather::Value V>
int setWeather(lua_State* L) {
    const ScriptCall& c = call(L);
    if (c.world) {
        c.world->weather().values[V] = argInt(L, 2);
    }
    return 0;
}

int isCameraIndoors(lua_State* L);

// Inventory tokens and inventories (lua_inventory.cpp).
int newInventoryToken(lua_State* L);
int releaseToken(lua_State* L);
int tokenGuid(lua_State* L);
int tokenFlags(lua_State* L);
int tokenSetFlag(lua_State* L);
int tokenTestFlag(lua_State* L);
int tokenClearFlag(lua_State* L);
int tokenCategory(lua_State* L);
int tokenSetCategory(lua_State* L);
int tokenCountProperties(lua_State* L);
int tokenClearProperties(lua_State* L);
int tokenSetProperty(lua_State* L);
int tokenGetProperty(lua_State* L);
int tokenPushProperty(lua_State* L);
int tokenPopProperty(lua_State* L);
int tokenPropertiesCopy(lua_State* L);
int tokenSetProperties(lua_State* L);
int newInventory(lua_State* L);
int inventoryAddToken(lua_State* L);
int inventoryAddTokenWithCategory(lua_State* L);
int inventoryRemoveMarked(lua_State* L);

// Which tokens a list or count takes: -1 for any GUID or flags. The singular lists also need the
// category to match (0 unless asked for).
struct TokenFilter {
    std::int64_t guid = -1;
    std::int32_t flags = -1;
    std::optional<std::uint16_t> category;
};
bool matches(const InventoryToken& t, const TokenFilter& f);
int inventoryTokens(lua_State* L);
int inventoryTokensByGuid(lua_State* L);
int inventoryTokensByFlags(lua_State* L);
int inventoryTokensByCategory(lua_State* L);
int inventoryCountedTokens(lua_State* L);
int inventoryCountedTokensByGuid(lua_State* L);
int inventoryCountedTokensByFlags(lua_State* L);
int countTokens(lua_State* L, const TokenFilter& filter);
int inventoryCountTokens(lua_State* L);
int inventoryCountTokensByGuid(lua_State* L);
int inventoryCountTokensByFlags(lua_State* L);
int inventoryFindToken(lua_State* L);
int inventoryTokenAtIndex(lua_State* L);

} // namespace ts2::sim::lua_detail
