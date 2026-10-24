#include "sim/lua_internals.hpp"

#include <cstring>

namespace ts2::sim::lua_detail {

// nWorld.HasSolidFloor(x, y, level): whether the quarter of the tile the point lies in has a floor
// pattern, which is what the game asks first. The quarter is which of the tile's four triangles
// (split along both diagonals) holds the point. Not done yet: the game's deck test when there is no
// pattern, its flat-ground test above the ground, the ground's terrain test and the second
// per-quarter grid it checks last.
int hasSolidFloor(lua_State* L) {
    const ScriptCall& c = call(L);
    const double x = lua_tonumber(L, 1), y = lua_tonumber(L, 2);
    const int level = argInt(L, 3);
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > 1e6 || y > 1e6 || c.world == nullptr ||
        !c.world->floorPattern()) {
        lua_pushboolean(L, false);
        return 1;
    }
    const double fx = x - std::floor(x), fy = y - std::floor(y);
    const int quarter = fx <= fy ? (fx + fy < 1 ? 0 : 1) : (fx + fy < 1 ? 3 : 2);
    const auto pattern = c.world->floorPattern()(level, static_cast<int>(x), static_cast<int>(y), quarter);
    lua_pushboolean(L, pattern.has_value() && *pattern != 0);
    return 1;
}

// nWorld.IsBeachLocation(x, y, mask): whether a point of the ground (level 0) lies in a band along a
// beach lot's beach side (the lot description's flag 0x80 and side byte), measured from that side's
// edge: mask 4 the band from 12 to 20 tiles, 2 from 8 to 12, 1 from 0 to 8. True when the point is in
// any band asked for. False off the lot and on other lots.
int isBeachLocation(lua_State* L) {
    const ScriptCall& c = call(L);
    const double x = lua_tonumber(L, 1), y = lua_tonumber(L, 2);
    const std::int32_t mask = argInt(L, 3);
    if (c.world == nullptr) {
        lua_pushboolean(L, false);
        return 1;
    }
    const auto& shape = c.world->lotShape();
    const double sizeX = std::trunc(shape.sizeX), sizeY = std::trunc(shape.sizeY);
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= shape.sizeX || y >= shape.sizeY || !shape.beachSide) {
        lua_pushboolean(L, false);
        return 1;
    }
    // A band from `from` to `from` + `width` tiles off the beach side's edge, ends included.
    const auto band = [&](double from, double width) {
        switch (*shape.beachSide) {
        case 0: return sizeX - from - width <= x && x <= sizeX - from;
        case 1: return from <= y && y <= from + width;
        case 2: return from <= x && x <= from + width;
        case 3: return sizeY - from - width <= y && y <= sizeY - from;
        default: return false;
        }
    };
    double from = 0, width = 0;
    bool in = false;
    if (mask & 4) {
        from = 12;
        width = 8;
        in = band(from, width);
    }
    if (!in && (mask & 2)) {
        from = 8;
        width = 4;
        in = band(from, width);
    }
    if (!in && (mask & 1)) {
        from = 0;
        width = 8;
    }
    // The game tests the last band asked for again (or the empty band with no mask) when none matched.
    lua_pushboolean(L, in || band(from, width));
    return 1;
}

// nWorld.GetObjectsOnTile(x, y, level[, all]): the ids of the objects on the tile, in the tile
// coordinates GetObjectTile gives, as an array. Without `all`, objects whose data 0x38 has bit 0x1
// or 0x4 are left out. Two object flags the game also skips are not kept yet.
int getObjectsOnTile(lua_State* L) {
    const int x = argInt(L, 1), y = argInt(L, 2), level = argInt(L, 3);
    const bool all = lua_gettop(L) >= 4 && lua_type(L, 4) == LUA_TBOOLEAN && lua_toboolean(L, 4) != 0;
    return pushObjectIds(L, [&](const SimObject& o) {
        return o.tile && o.tile->x == y && o.tile->y == x && o.tile->level == level + 1 &&
               (all || (o.data[kSearchSkipFlagsField] & 0x05) == 0);
    });
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

// nWorld.IsOutsideForWeather(x, y, level): whether rain and snow reach a point (x and y as
// GetObjectLocation gives them). Points off the lot are not. Points inside a region of the lot's
// second room layer never are. Others are as the lot's outside grid has them.
int isOutsideForWeather(lua_State* L) {
    const ScriptCall& c = call(L);
    static const RoomMap kNone;
    const RoomMap& regions = c.world ? c.world->weatherRegions() : kNone;  // a reference: no copy of the map
    const auto x = singleOf(lua_tonumber(L, 1)), y = singleOf(lua_tonumber(L, 2));
    const auto level = argInt(L, 3);
    if (!(x >= 0 && y >= 0 && x < static_cast<float>(regions.width()) && y < static_cast<float>(regions.height()) &&
          level >= regions.firstLevel() && level < regions.firstLevel() + regions.levelCount())) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const std::optional<bool> open = c.world ? c.world->outside().open(level, x, y) : std::nullopt;
    lua_pushboolean(L, open.value_or(false) ? 1 : 0);
    return 1;
}

// WeatherInfo: the neighborhood's weather and season. Getters return a value, setters change it
// for the rest of the visit, and isCameraIndoors is false until the camera says otherwise.
int newWeatherInfo(lua_State* L) { return pushInstance(L, "WeatherInfo", 0); }

int newNeighborhoodManager(lua_State* L) { return pushInstance(L, "NeighborhoodManager", 0); }

namespace {

// The name scripts know a neighborhood type by.
const char* typeName(std::int32_t type) {
    constexpr const char* kNames[] = {"primary", "university", "downtown", "suburb", "asia", "mountains", "tropics"};
    return type >= 1 && type <= 7 ? kNames[type - 1] : "unknown";
}

// The type a name stands for, 0 for an unknown name.
std::int32_t typeOfName(const char* name) {
    for (std::int32_t type = 1; type <= 7; ++type) {
        if (name != nullptr && std::strcmp(typeName(type), name) == 0) {
            return type;
        }
    }
    return 0;
}

const nhoodid::NeighborhoodId* currentNeighborhood(const ScriptCall& c) {
    const auto id = c.world ? c.world->neighborhoodId() : std::nullopt;
    return id ? c.world->neighborhoodById(*id) : nullptr;
}

const nhoodid::NeighborhoodId* neighborhoodArg(lua_State* L) {
    const ScriptCall& c = call(L);
    return c.world ? c.world->neighborhoodById(argInt(L, 2)) : nullptr;
}

} // namespace

// NeighborhoodManager:isValidNeighborhood(id): whether the Neighborhoods folder has that neighborhood.
int neighborhoodManagerIsValid(lua_State* L) {
    lua_pushboolean(L, neighborhoodArg(L) != nullptr);
    return 1;
}

// NeighborhoodManager:getNumNeighborhoodsOfType(typeName, [value]): how many sub-neighborhoods of the
// current neighborhood have that type (any type for an unknown name) and, when a number is given,
// that value (NeighborhoodId::value2).
int neighborhoodManagerCountOfType(lua_State* L) {
    const ScriptCall& c = call(L);
    const std::int32_t type = typeOfName(lua_tostring(L, 2));
    const std::optional<std::int64_t> value =
        lua_gettop(L) >= 3 && lua_type(L, 3) == LUA_TNUMBER ? std::optional(argInteger(L, 3)) : std::nullopt;
    const auto* current = currentNeighborhood(c);
    const auto* all = c.world ? c.world->allNeighborhoods() : nullptr;
    std::int32_t count = 0;
    if (current != nullptr && all != nullptr) {
        for (const auto& [id, n] : *all) {
            if (n.prefix == current->prefix && !n.subPrefix.empty() && (type == 0 || n.type == type) &&
                (!value || n.value2 == *value)) {
                ++count;
            }
        }
    }
    lua_pushnumber(L, count);
    return 1;
}

// NeighborhoodManager:getNeighborhoodType(id): its type's name, "unknown" for an unknown id.
int neighborhoodManagerType(lua_State* L) {
    const auto* n = neighborhoodArg(L);
    lua_pushstring(L, typeName(n ? n->type : 0));
    return 1;
}

// NeighborhoodManager:getCurrentNeighborhoodType(): the type of the neighborhood the lot is in.
int neighborhoodManagerCurrentType(lua_State* L) {
    const auto* n = currentNeighborhood(call(L));
    lua_pushstring(L, typeName(n ? n->type : 0));
    return 1;
}

// NeighborhoodManager:getSeasonOfQuadrant(id, quadrant): the season of a quadrant (0-3) of that
// neighborhood, -1 for an unknown id.
int neighborhoodManagerSeasonOfQuadrant(lua_State* L) {
    const auto* n = neighborhoodArg(L);
    if (n == nullptr) {
        lua_pushnumber(L, -1);
        return 1;
    }
    const std::int32_t quadrant = argInt(L, 3);
    if (quadrant < 0 || quadrant >= static_cast<std::int32_t>(n->seasons.size())) {
        return stopUnsupported(L, "getSeasonOfQuadrant outside quadrants 0-3");
    }
    lua_pushnumber(L, n->seasons[static_cast<std::size_t>(quadrant)]);
    return 1;
}

int isCameraIndoors(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

} // namespace ts2::sim::lua_detail
