#include "sim/lua_internals.hpp"

namespace ts2::sim::lua_detail {

namespace {

LuaRuntime& runtime(lua_State* L) {
    lua_pushlightuserdata(L, &kRuntimeKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* r = static_cast<LuaRuntime*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *r;
}

int installedPacksOrStop(lua_State* L, std::uint32_t& mask) {
    mask = runtime(L).installedPacks;
    if (mask == 0) {
        return stopUnsupported(L, "nGameState.IsEPInstalled without the install");
    }
    return 0;
}

} // namespace

// nMessaging.PostMessage(id, values...): the game builds a message from the id and up to four
// booleans, numbers or one string (a second string is error 0x9B) and posts it to its message server.
// Only the game's own (UI) listeners hear the messages scripts post, so here it has no effect.
int postMessage(lua_State* L) {
    argUnsigned(L, 1);
    constexpr int kMaxValues = 4;
    bool hasString = false;
    for (int i = 2; i <= std::min(lua_gettop(L), 1 + kMaxValues); ++i) {
        if (lua_type(L, i) != LUA_TSTRING) {
            continue;
        }
        if (hasString) {
            return gameError(L, kErrorScriptCheck, "Trying to set more than one string value in message!");
        }
        hasString = true;
    }
    return 0;
}

// nMessaging.SubscribeMessage(object id, message id, tree name): the object's tree of that name is
// to run when the message is posted. Subscribing again replaces the name. UnsubscribeMessage does
// the same in the game, so a script that unsubscribes subscribes again. Nothing in the engine
// delivers messages to subscribers yet.
int subscribeMessage(lua_State* L) {
    const std::int32_t id = argInt(L, 1);
    const auto message = static_cast<std::uint32_t>(argInt(L, 2));
    if (lua_type(L, 3) != LUA_TSTRING) {
        return stopUnsupported(L, "nMessaging.SubscribeMessage without a tree name");
    }
    const std::string_view tree(lua_tostring(L, 3), lua_strlen(L, 3));
    ScriptCall& c = call(L);
    const bool known = id >= 1 && id <= INT16_MAX && c.world != nullptr &&
                       c.world->find(static_cast<std::int16_t>(id)) != nullptr;
    if (!known) {
        return scriptCheckError(L, "Could not find object: %d", static_cast<int>(id));
    }
    if (!c.world->messageSubscriptions().subscribe(message, static_cast<std::int16_t>(id), tree)) {
        return scriptCheckError(L, "Too many message subscriptions.");
    }
    return 0;
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

// nGameState.SetProperty(name, value): a boolean or string as such. A number keeps the type of the
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
            value = singleOf(n);
        } else if (it != properties.end() && std::holds_alternative<std::uint32_t>(it->second)) {
            value = static_cast<std::uint32_t>(wholeNumber(n));
        } else {
            value = static_cast<std::int32_t>(wholeNumber(n));
        }
    }
    }
    properties[name] = std::move(value);
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

// nBusiness.LotHasBusiness(lot id): whether the lot runs a business (only with Open for Business).
// Owned businesses aren't read yet, and the neighborhoods as shipped have none, so no lot has one.
int lotHasBusiness(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

} // namespace ts2::sim::lua_detail
