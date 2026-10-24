#include "sim/lua_internals.hpp"

namespace ts2::sim::lua_detail {

namespace {

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

} // namespace

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

int getLocal(lua_State* L) { return getFrameValue(L, call(L).locals, kErrorLocalOutOfRange); }

int setLocal(lua_State* L) { return setFrameValue(L, call(L).locals, kErrorLocalOutOfRange); }

int getTreeParameter(lua_State* L) { return getFrameValue(L, call(L).treeParams, kErrorParamOutOfRange); }

int setTreeParameter(lua_State* L) { return setFrameValue(L, call(L).treeParams, kErrorParamOutOfRange); }

int doNothing(lua_State*) { return 0; }

// Bits are numbered from 1. Values are unsigned 32-bit.
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
    const float shifted = singleOf(lua_tonumber(L, 1)) + 12582912.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &shifted, sizeof(bits));
    lua_pushnumber(L, static_cast<std::int32_t>(bits - 0x4B400000u));
    return 1;
}

} // namespace ts2::sim::lua_detail
