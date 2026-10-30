#include "sim/lua_runtime.hpp"
#include "sim/lua_internals.hpp"

namespace ts2::sim {

namespace lua_detail {

namespace {

// Registry keys of the ScriptCall the running script works on and of the runtime itself.
char kCallKey;

int guarded(lua_State* L) {
    const auto function = reinterpret_cast<lua_CFunction>(lua_touserdata(L, lua_upvalueindex(1)));
    char message[sizeof(ScriptStop::message)];
    try {
        return function(L);
    } catch (const LuaThrow&) {
        throw;
    } catch (const ScriptStop& stop) {
        std::memcpy(message, stop.message, sizeof message);
    } catch (const std::exception& ex) {
        std::snprintf(message, sizeof message, "%s", ex.what());
    } catch (...) {
        std::snprintf(message, sizeof message, "%s", "unknown error in a script function");
    }
    lua_pushstring(L, message);
    return lua_error(L);
}

void pushGuarded(lua_State* L, lua_CFunction function) {
    lua_pushlightuserdata(L, reinterpret_cast<void*>(function));
    lua_pushcclosure(L, guarded, 1);
}

// The call being run: set by load() and run() for as long as they run Lua code, null otherwise.
ScriptCall* currentCall(lua_State* L) {
    lua_pushlightuserdata(L, &kCallKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* c = static_cast<ScriptCall*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return c;
}

void setCurrentCall(lua_State* L, ScriptCall* c) {
    lua_pushlightuserdata(L, &kCallKey);
    lua_pushlightuserdata(L, c);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

// Sets the current call for as long as it lives, then clears it, however the scope ends.
struct CallScope {
    lua_State* L;
    CallScope(lua_State* state, ScriptCall* c) : L(state) { setCurrentCall(L, c); }
    ~CallScope() { setCurrentCall(L, nullptr); }
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;
};

// The message of the error on top of the stack. A script may raise one that isn't text.
std::string errorText(lua_State* L) {
    const char* text = lua_tostring(L, -1);
    return text ? text : "(an error that isn't text)";
}

// A script is stopped once it has run this many Lua instructions in one call: an endless loop
// (say, in a broken mod) would otherwise freeze the game. The count is checked every kCountEvery.
constexpr std::uint64_t kMaxInstructions = 10'000'000;

constexpr int kCountEvery = 1000;

void countInstructions(lua_State* L, lua_Debug*) {
    ScriptCall* c = currentCall(L);
    if (c == nullptr) {
        return;
    }
    c->instructions += kCountEvery;
    if (c->instructions > kMaxInstructions) {
        lua_pushstring(L, "script ran too long (an endless loop?)");
        lua_error(L);
    }
}

struct Function {
    const char* name;
    lua_CFunction function;
};

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
    {"GetNeighborData", getNeighborData},
    {"GetNeighborRelationship", getNeighborRelationship},
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
    "SetNeighborRelationship", "GetMasterSelector",
    "OverrideTreeTable", "GetCustomStationNames", "StartSound",
};

// The world-facing parts of the API.

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
     {{"LotHasBusiness", lotHasBusiness}},
     {"CreateBusiness", "RemoveBusiness"},
     {}},
    {"CachedFamily", true, newCachedFamily,
     {{"getId", cachedFamilyId}, {"getLotId", cachedFamilyLotId}, {"getCurrentLotId", cachedFamilyCurrentLotId},
      {"getVacationLotId", cachedFamilyVacationLotId}, {"getMembers", cachedFamilyMembers},
      {"getNumMembers", cachedFamilyMemberCount}},
     {"setVacationLotId", "getName", "getFunds", "getLastSavedFunds", "isNeighborInFamily", "addMemberClothing", "IsAFamilyInThisLot"},
     {}},
    {"ObjectDefinition", true, nullptr,
     {},
     {"getField", "get32BitField"},
     {}},
    {"LightingManager", true, nullptr,
     {},
     {"setPlacementRoomLightPercent"},
     {}},
    {"WeatherInfo", true, newWeatherInfo,
     {{"getSeason", getWeather<weather::Weather::kSeason>},
      {"setSeason", setWeather<weather::Weather::kSeason>},
      {"getSeasonQuadrant", getWeather<weather::Weather::kSeasonQuadrant>},
      {"setSeasonQuadrant", setWeather<weather::Weather::kSeasonQuadrant>},
      {"getSeasonDaysStandard", getWeather<weather::Weather::kSeasonDaysStandard>},
      {"setSeasonDaysStandard", setWeather<weather::Weather::kSeasonDaysStandard>},
      {"getSeasonDaysAdditional", getWeather<weather::Weather::kSeasonDaysAdditional>},
      {"setSeasonDaysAdditional", setWeather<weather::Weather::kSeasonDaysAdditional>},
      {"getPrecipitationType", getWeather<weather::Weather::kPrecipitationType>},
      {"setPrecipitationType", setWeather<weather::Weather::kPrecipitationType>},
      {"getPrecipitationIntensity", getWeather<weather::Weather::kPrecipitationIntensity>},
      {"setPrecipitationIntensity", setWeather<weather::Weather::kPrecipitationIntensity>},
      {"getAccumulationLevel", getWeather<weather::Weather::kAccumulationLevel>},
      {"setAccumulationLevel", setWeather<weather::Weather::kAccumulationLevel>},
      {"getAccumulationType", getWeather<weather::Weather::kAccumulationType>},
      {"setAccumulationType", setWeather<weather::Weather::kAccumulationType>},
      {"getTemperature", getWeather<weather::Weather::kTemperature>},
      {"setTemperature", setWeather<weather::Weather::kTemperature>},
      {"getSkyClarity", getWeather<weather::Weather::kSkyClarity>},
      {"setSkyClarity", setWeather<weather::Weather::kSkyClarity>},
      {"getPondFrozenState", getWeather<weather::Weather::kPondFrozenState>},
      {"setPondFrozenState", setWeather<weather::Weather::kPondFrozenState>},
      {"isCameraIndoors", isCameraIndoors}},
     {},
     {}},
    {"nUI", false, nullptr,
     {{"InvalidateSimThumbnail", invalidateSimThumbnail}},
     {"ShowTextNotification", "DebugTNS", "OpenProgressDialog", "CloseProgressDialog", "ShowProgressDialogControl", "UpdateSimChooserUI", "AddActionString", "UpdateSkewerAndPortrait"},
     {}},
    {"nMessaging", false, nullptr,
     {{"PostMessage", postMessage}, {"SubscribeMessage", subscribeMessage},
      {"UnsubscribeMessage", subscribeMessage}},
     {},
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
     {{"isRoommate", neighborhoodIsRoommate}, {"isValidLot", neighborhoodIsValidLot}, {"isApartmentDoor", neighborhoodIsApartmentDoor},
      {"isApartmentResident", neighborhoodIsApartmentResident},
      {"getLotOwner", neighborhoodGetLotOwner}, {"getWitchLotType", neighborhoodGetWitchLotType}},
     {"getLotType", "setLotOwner", "getLotValue", "getLotName", "getLotIds", "getLotIdsInAllNeighborhoods", "getNeighborhoodIdFromLotId", "isHobbyFlagSet", "getApartmentLinkedFamilies", "getApartmentLinkedLots", "getApartmentLinkedControlRooms", "setAptControlRoom", "getAptBaseLot", "setAptMaxRent", "setAptMinRent", "setMaxApartments", "getRoommateId", "setRoommateId", "updateLotClass", "isNeighborsApartment", "getNIDsApartment"},
     {}},
    {"BusinessInfo", true, nullptr,
     {},
     {"getRank", "setRank", "getHighestRank", "setHighestRank", "getRemoteIncomeDecay", "setRemoteIncomeDecay", "getCurrentPnLData", "getAllPnLData", "getTotalProfit", "getTotals", "getAllCustomerData", "getCustomerIds", "getCustomerDataById", "isCustomer", "addCustomer", "removeCustomer", "getAllEmployeeData", "getEmployeeIds", "getEmployeeDataById", "isEmployee", "addEmployee", "removeEmployee", "getWholesaleMultiplier", "setWholesaleMultiplier", "getBusinessFlags", "setBusinessFlags"},
     {}},
    {"CachedNeighbor", true, newCachedNeighbor,
     {{"getPersonData", cachedNeighborGetData}, {"setPersonData", cachedNeighborSetData}, {"getGUID", cachedNeighborGuid},
      {"getOutfitProperty", cachedNeighborOutfitProperty}},
     { "getName", "setName", "getSurname", "setSurname", "getFullName", "getBio", "setBio"},
     {}},
    {"nWorld", false, nullptr,
     {{"GetObjectTile", getObjectTile}, {"GetObjectLocation", getObjectLocation}, {"IsOutsideForWeather", isOutsideForWeather},
      {"HasSolidFloor", hasSolidFloor}, {"GetObjectsOnTile", getObjectsOnTile}, {"IsBeachLocation", isBeachLocation},
      {"HasStage", hasStage}, {"HasStageAny", hasStage}, {"GetNumWallSections", getNumWallSections}, {"GetRoomOnSideOfWall", getRoomOnSideOfWall}},
     {"GetMinX", "GetMaxX", "GetMinY", "GetMaxY", "GetLotSizeX", "GetLotSizeY", "IsOnLot", "HasSolidGround", "HasRoof", "IsLocked", "HasDriveway", "IsLoadBearing", "GetMinTileX", "GetMinTileY", "GetMaxTileX", "GetMaxTileY", "GetNumTilesX", "GetNumTilesY", "GetMinLevel", "GetMaxLevel", "GetNumLevels", "GetObjectTiles", "FindLocationForObject", "GetTerrainElevationForObject", "GetTerrainElevationForXY", "GetWaterElevationForObject", "GetWaterElevationForXY", "MoveObjectToLocation", "IsPoolOnTile", "IsWaterOnTile", "IsDeckOnTile", "IsWaterOnEntireTile", "GetSnowAccumulationLevelOnTile", "IsWallBetweenPoints", "SetLotImposterDirty", "MoveObjectToWallLocation", "CanMoveObjectToLocation", "GetTileOnSideOfWall", "GetOrientationAwayFromWallSide", "GetRoomAtLocation", "GetLoadBearingRoomAtLocation", "IsLoadWallBetweenPoints"},
     {}},
    {"NeighborhoodManager", true, newNeighborhoodManager,
     {{"isValidNeighborhood", neighborhoodManagerIsValid}, {"getNeighborhoodType", neighborhoodManagerType},
      {"getCurrentNeighborhoodType", neighborhoodManagerCurrentType}, {"getSeasonOfQuadrant", neighborhoodManagerSeasonOfQuadrant},
      {"getNumNeighborhoodsOfType", neighborhoodManagerCountOfType}},
     {"getNeighborhoodIds", "getLotIdsByNeighborhood", "getLotIdsInAllNeighborhoods", "getNeighborhoodIdFromLotId"},
     {}},
    {"CachedPerson", true, newCachedPerson,
     {{"getPersonData", cachedPersonGetData}, {"setPersonData", cachedPersonSetData},
      {"getNumQueuedInteractions", cachedPersonQueueCount}, {"getGUID", cachedPersonGuid},
      {"isQueuedObjAdvertisingForX", cachedPersonQueuedAdvertising},
      {"getFriendsRequiredForPromtion", cachedPersonFriendsForPromotion}},
     {"getMotive", "setMotive", "getQueuedInteractionId", "getQueuedInteractionObjectId", "cancelQueuedInteraction", "getNumSubQueuedInteractions", "getQueuedSubInteractionId", "getQueuedSubInteractionObjectId", "unlockVacationCollectible", "isAccessibleRoom"},
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

// The game's scripts don't use these, so they are removed too. A coroutine runs without the
// instruction count, and newproxy makes a userdata whose metatable a script controls.
constexpr const char* kUnsafeGlobals[] = {"coroutine", "newproxy"};

// Loads a game chunk (rewritten for this build) as a function on the stack. False with the
// message on the stack when it can't.
bool loadChunk(lua_State* L, const luascript::Script& script, std::string& error) {
    try {
        const auto chunk = luascript::nativeChunk(script.chunk);
        if (luaL_loadbuffer(L, reinterpret_cast<const char*>(chunk.data()), chunk.size(), script.name.c_str()) != 0) {
            error = errorText(L);
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

ScriptCall& call(lua_State* L) {
    return *currentCall(L);
}

// A number as a whole number: truncated, NaN as 0, clamped to what 64 bits hold (a script can pass
// anything, and converting an out-of-range double is undefined).
std::int64_t wholeNumber(double v) {
    if (!(v == v)) {
        return 0;
    }
    constexpr double kLimit = 9.0e18;
    return static_cast<std::int64_t>(std::clamp(v, -kLimit, kLimit));
}

std::int64_t argInteger(lua_State* L, int i) { return wholeNumber(lua_tonumber(L, i)); }

// A number in single precision, beyond its range clamped (converting such a double is undefined).
float singleOf(double v) {
    constexpr double kMax = std::numeric_limits<float>::max();
    return static_cast<float>(v > kMax ? kMax : v < -kMax ? -kMax : v);
}

// Lua numbers as the game's script functions read them.
std::int16_t argShort(lua_State* L, int i) {
    return static_cast<std::int16_t>(argInteger(L, i));
}

std::int32_t argInt(lua_State* L, int i) {
    return static_cast<std::int32_t>(argInteger(L, i));
}

std::uint32_t argUnsigned(lua_State* L, int i) {
    return static_cast<std::uint32_t>(argInteger(L, i));
}

void pushUnsigned(lua_State* L, std::uint32_t value) {
    lua_pushnumber(L, static_cast<lua_Number>(value));
}

// A function the game provides that the engine doesn't have yet: stops the script and says which.
// Called by Lua directly (not guarded). It keeps nothing alive across lua_error.
int unsupported(lua_State* L) {
    try {
        call(L).unsupported = lua_tostring(L, lua_upvalueindex(1));
    } catch (...) {  // no C++ exception may cross Lua's frames (see guarded)
    }
    lua_pushstring(L, "unsupported script function");
    return lua_error(L);
}

} // namespace lua_detail

using namespace lua_detail;

std::vector<ScriptFunctionInfo> scriptFunctions() {
    std::vector<ScriptFunctionInfo> out;
    for (const auto& f : kFunctions) {
        out.push_back({f.name, true});
    }
    for (const char* name : kNotYetImplemented) {
        out.push_back({name, false});
    }
    for (const auto& cls : kClasses) {
        const std::string prefix = std::string(cls.name) + (cls.isClass ? ":" : ".");
        for (const auto& f : cls.methods) {
            out.push_back({prefix + f.name, true});
        }
        for (const char* name : cls.notYetImplemented) {
            out.push_back({prefix + name, false});
        }
        if (cls.isClass) {
            out.push_back({std::string(cls.name) + ".new", cls.constructor != nullptr});
        }
    }
    return out;
}

LuaRuntime::LuaRuntime() : L_(lua_open()) {
    luaopen_base(L_);
    luaopen_table(L_);
    luaopen_string(L_);
    luaopen_math(L_);
    lua_settop(L_, 0);
    lua_sethook(L_, countInstructions, LUA_MASKCOUNT, kCountEvery);
    for (const char* name : kRemovedGlobals) {
        lua_pushstring(L_, name);
        lua_pushnil(L_);
        lua_rawset(L_, LUA_GLOBALSINDEX);
    }
    for (const char* name : kUnsafeGlobals) {
        lua_pushstring(L_, name);
        lua_pushnil(L_);
        lua_rawset(L_, LUA_GLOBALSINDEX);
    }
    for (const auto& f : kFunctions) {
        lua_pushstring(L_, f.name);
        pushGuarded(L_, f.function);
        lua_rawset(L_, LUA_GLOBALSINDEX);
    }
    for (const char* name : kNotYetImplemented) {
        lua_pushstring(L_, name);
        lua_pushstring(L_, name);
        lua_pushcclosure(L_, unsupported, 1);
        lua_rawset(L_, LUA_GLOBALSINDEX);
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
            // getmetatable gives scripts the class name instead, so they can't give instances a
            // finalizer of their own (it would run outside any call and without the instruction count).
            lua_pushstring(L_, "__metatable");
            lua_pushstring(L_, cls.name);
            lua_settable(L_, -3);
            if (cls.finalizer) {
                lua_pushstring(L_, "__gc");
                lua_pushcfunction(L_, cls.finalizer);
                lua_settable(L_, -3);
            } else {
                lua_pushstring(L_, kInstanceMarker);
                lua_pushboolean(L_, 1);
                lua_settable(L_, -3);
            }
            lua_pop(L_, 1);
        }
        lua_pushstring(L_, cls.name);
        lua_insert(L_, -2);
        lua_rawset(L_, LUA_GLOBALSINDEX);
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
    const CallScope scope(L_, &setup);
    std::string error;
    for (const auto& m : modules) {
        if (!loadChunk(L_, m, error)) {
            problems.push_back("module " + m.name + ": " + error);
            continue;
        }
        setup.unsupported.clear();
        if (lua_pcall(L_, 0, 0, 0) != 0) {
            const std::string why = setup.unsupported.empty() ? errorText(L_) : "needs " + setup.unsupported;
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
        lua_rawset(L_, LUA_GLOBALSINDEX);
    }
    lua_settop(L_, 0);
}

bool LuaRuntime::hasScript(const std::string& name) {
    lua_pushstring(L_, name.c_str());
    lua_rawget(L_, LUA_GLOBALSINDEX);
    const bool found = lua_isfunction(L_, -1);
    lua_pop(L_, 1);
    return found;
}

bool LuaRuntime::run(const std::string& name, ScriptCall& c) {
    c.instructions = 0;
    const CallScope scope(L_, &c);
    lua_pushstring(L_, name.c_str());
    lua_rawget(L_, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        c.luaError = "no script named " + name;
        return false;
    }
    return callTop(c);
}

bool LuaRuntime::runSource(const std::string& name, const std::string& source, ScriptCall& c) {
    // Text this long is not a script. Text starting like a compiled chunk would be loaded as
    // unchecked bytecode.
    constexpr std::size_t kMaxSource = 64 * 1024;
    constexpr char kCompiledChunkStart = 27;
    c.instructions = 0;
    const CallScope scope(L_, &c);
    lua_pushstring(L_, name.c_str());
    lua_rawget(L_, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        if (source.size() > kMaxSource || (!source.empty() && source[0] == kCompiledChunkStart)) {
            c.luaError = "script text not usable: " + name;
            return false;
        }
        if (luaL_loadbuffer(L_, source.data(), source.size(), name.c_str()) != 0) {
            c.luaError = errorText(L_);
            lua_pop(L_, 1);
            return false;
        }
        lua_pushstring(L_, name.c_str());
        lua_pushvalue(L_, -2);
        lua_rawset(L_, LUA_GLOBALSINDEX);
    }
    return callTop(c);
}

// Calls the function on top of the stack for the current call.
bool LuaRuntime::callTop(ScriptCall& c) {
    if (const int status = lua_pcall(L_, 0, 0, 0); status != 0) {
        if (c.unsupported.empty() && c.errorCode == 0) {
            c.luaError = errorText(L_);
        }
        lua_pop(L_, 1);
        if (status == LUA_ERRMEM) {
            // What the script left at the memory cap is garbage now. It is collected here, as Lua's own
            // collection waits for a threshold the cap keeps it from reaching.
            lua_setgcthreshold(L_, 0);
        }
        return false;
    }
    return true;
}

} // namespace ts2::sim
