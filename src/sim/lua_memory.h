/* Included into every Lua source file (see CMakeLists.txt) so that Lua's memory goes through
   ts2_lua_realloc, which caps how much one Lua state may hold. */
#ifndef TS2_LUA_MEMORY_H
#define TS2_LUA_MEMORY_H

#include <stddef.h>

struct lua_State;
void *ts2_lua_realloc(struct lua_State *L, void *block, size_t oldsize, size_t size);

/* Lua calls this from luaM_realloc, where `L` is the state allocating. */
#define l_realloc(b, os, s) ts2_lua_realloc(L, (b), (os), (s))

#endif
