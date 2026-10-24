/* Lua is built as C++ (see CMakeLists.txt), and its errors are thrown as this type instead of
   longjmp (see luaD_throw in ldo.c), so they unwind C++ frames and run their destructors. */
#ifndef TS2_LUA_ERROR_H
#define TS2_LUA_ERROR_H

struct LuaThrow {};

#endif
