#include "lstate.h"

#include <stdlib.h>

/* The most memory one Lua state may hold. A script that asks for more (an endless string or table
   in a broken mod) fails with Lua's out-of-memory error instead of using up the computer's memory. */
#define TS2_LUA_MAX_BYTES ((lu_mem)256 * 1024 * 1024)

void *ts2_lua_realloc(lua_State *L, void *block, size_t oldsize, size_t size) {
    /* While the state is being created it has no totals yet. Shrinking is always allowed. */
    if (L != NULL && L->l_G != NULL && size > oldsize && G(L)->nblocks + (size - oldsize) > TS2_LUA_MAX_BYTES) {
        return NULL;
    }
    return realloc(block, size);
}
