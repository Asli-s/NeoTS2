#include "sim/lua_internals.hpp"

namespace ts2::sim::lua_detail {

namespace {

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
    luaL_getmetatable(L, kTokenClass);
    lua_setmetatable(L, -2);
    return 1;
}

// The token handle at argument 1, or null when it is something else.
TokenHandle* tokenHandle(lua_State* L) {
    return isClassUserdata(L, 1, kTokenClass) ? static_cast<TokenHandle*>(lua_touserdata(L, 1)) : nullptr;
}

InventoryToken& selfToken(lua_State* L) {
    TokenHandle* handle = tokenHandle(L);
    if (handle == nullptr || !handle->token) {
        scriptCheckError(L, "InventoryToken method called without its token");
    }
    return *handle->token;
}

// Properties are counted from 1. Outside the list is error 0x58.
std::int16_t& tokenProperty(lua_State* L, InventoryToken& token) {
    const int index = argInt(L, 2) - 1;
    if (index < 0 || static_cast<std::size_t>(index) >= token.properties.size()) {
        gameError(L, kErrorPropertyOutOfRange);
    }
    return token.properties[static_cast<std::size_t>(index)];
}

// Appends values to a token's properties: the values of a table (in next() order) when argument
// `first` is a table, otherwise the arguments from `first` on.
void appendProperties(lua_State* L, InventoryToken& token, int first) {
    auto add = [&](std::int16_t value) {
        if (token.properties.size() >= kMaxScriptList) {
            scriptCheckError(L, "Too many token properties.");
        }
        token.properties.push_back(value);
    };
    const int top = lua_gettop(L);
    if (top == first && lua_type(L, first) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, first) != 0) {
            add(static_cast<std::int16_t>(argInt(L, -1)));
            lua_pop(L, 1);
        }
        return;
    }
    for (int i = first; i <= top; ++i) {
        add(argShort(L, i));
    }
}

Inventory& selfInventory(lua_State* L) {
    const auto* self = selfInstance(L);
    ScriptCall& c = call(L);
    Inventory* inventory = self && c.world ? c.world->inventory(static_cast<InventoryKind>(self->kind), self->id) : nullptr;
    if (inventory == nullptr) {
        gameError(L, kErrorInventoryMissing);
    }
    return *inventory;
}

// addToken(guid, values...) and addTokenWithCategory(guid, category, values...): a new singular
// token. The values may also come as one table.
int addTokenFrom(lua_State* L, bool withCategory) {
    Inventory& inventory = selfInventory(L);
    auto token = std::make_shared<InventoryToken>();
    token->guid = argUnsigned(L, 2);
    if (withCategory) {
        token->category = static_cast<std::uint16_t>(argUnsigned(L, 3));
    }
    appendProperties(L, *token, withCategory ? 4 : 3);
    if (inventory.singular.size() >= kMaxScriptList) {
        return scriptCheckError(L, "Too many tokens.");
    }
    inventory.singular.push_back(std::move(token));
    lua_pushboolean(L, true);
    return 1;
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
    f.guid = argInt(L, 2);
    if (lua_gettop(L) >= 3) {
        f.flags = argInt(L, 3);
    }
    return f;
}

TokenFilter flagsFilter(lua_State* L) {
    TokenFilter f;
    f.flags = argInt(L, 2);
    return f;
}

TokenFilter withCategory(TokenFilter f, std::uint16_t category = 0) {
    f.category = category;
    return f;
}

} // namespace

// Tokens come only from an inventory.
int newInventoryToken(lua_State* L) {
    return gameError(L, kErrorScriptCheck, "Cannot directly create an InventoryToken--use Inventory class to access this wrapped class.");
}

// __gc. Scripts can reach it through getmetatable and call it early, so it only empties the handle
// and later calls find no token.
int releaseToken(lua_State* L) {
    if (TokenHandle* handle = tokenHandle(L)) {
        handle->token.reset();
    }
    return 0;
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
    selfToken(L).flags |= static_cast<std::uint16_t>(argInt(L, 2));
    return 0;
}

int tokenTestFlag(lua_State* L) {
    lua_pushboolean(L, (selfToken(L).flags & static_cast<std::uint16_t>(argInt(L, 2))) != 0);
    return 1;
}

int tokenClearFlag(lua_State* L) {
    selfToken(L).flags &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(argInt(L, 2)));
    return 0;
}

int tokenCategory(lua_State* L) {
    lua_pushnumber(L, selfToken(L).category);
    return 1;
}

int tokenSetCategory(lua_State* L) {
    selfToken(L).category = static_cast<std::uint16_t>(argInt(L, 2));
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

int tokenSetProperties(lua_State* L) {
    InventoryToken& token = selfToken(L);
    token.properties.clear();
    appendProperties(L, token, 2);
    return 0;
}

// Inventory.new(kind, id): kTypeGlobal, kTypeLot (lot id), kTypeFamily (family id),
// kTypeNeighbor (neighbor id) or kTypeGameWide. An unknown kind is error 0x59. An inventory the
// neighborhood doesn't have is 0x57.
int newInventory(lua_State* L) {
    ScriptCall& c = call(L);
    const int kind = argInt(L, 1);
    if (kind < 0 || kind > static_cast<int>(InventoryKind::GameWide)) {
        return gameError(L, kErrorInventoryType);
    }
    const auto id = argInt(L, 2);
    if (c.world == nullptr || c.world->inventory(static_cast<InventoryKind>(kind), id) == nullptr) {
        return gameError(L, kErrorInventoryMissing);
    }
    pushInstance(L, "Inventory", id);
    static_cast<Instance*>(lua_touserdata(L, -1))->kind = kind;
    return 1;
}

int inventoryAddToken(lua_State* L) { return addTokenFrom(L, false); }

int inventoryAddTokenWithCategory(lua_State* L) { return addTokenFrom(L, true); }

int inventoryRemoveMarked(lua_State* L) {
    auto& tokens = selfInventory(L).singular;
    std::erase_if(tokens, [](const TokenRef& t) { return (t->flags & kTokenMarkedForDeletion) != 0; });
    return 0;
}

bool matches(const InventoryToken& t, const TokenFilter& f) {
    return (f.guid == -1 || static_cast<std::int32_t>(t.guid) == static_cast<std::int32_t>(f.guid)) &&
           (f.flags == -1 || static_cast<std::int16_t>(t.flags) == f.flags) &&
           (!f.category || t.category == *f.category);
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
// GUID with category 0 whose flags match ((flags & mask) == (value & mask). With only the GUID any
// flags match, with a value and no mask all bits must match), searching from the end when asked.
// Nil when there is none.
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
    const int index = argInt(L, 2) - 1;
    if (index < 0 || static_cast<std::size_t>(index) >= tokens.size()) {
        lua_pushnil(L);
        return 1;
    }
    return pushToken(L, tokens[static_cast<std::size_t>(index)]);
}

} // namespace ts2::sim::lua_detail
