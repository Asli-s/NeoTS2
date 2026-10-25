# Lua 5.0.3

The Sims 2 runs object scripts compiled for Lua 5.0, so the engine embeds the original Lua 5.0.3
interpreter to run them exactly as the game does (for example the order `pairs` visits a table).

- Source: https://www.lua.org/ftp/lua-5.0.3.tar.gz
  (SHA-256 `1193a61b0e08acaa6eee0eecf29709179ee49c71baebc59b682a25c3b5a45671`)
- License: MIT, see `COPYRIGHT`.
- Unchanged copies of `include/`, the core in `src/`, and the auxiliary, base, math, string and
  table libraries from `src/lib/`. Left out: the I/O, dynamic-loading and debug libraries, the
  test build (`ltests.c`) and the standalone `lua` and `luac` programs.
- Four changes, marked `NeoTS2`. In `src/lib/lstrlib.c`, pattern matching stops with "pattern too
  complex" past 200 nested match calls or 10 million steps in one search, so a pattern can't
  overflow the C stack or run without end where the instruction count can't stop it. In `src/ldo.c`: Lua is built as C++, and `luaD_throw` and
  `luaD_rawrunprotected` throw and catch a C++ exception (`src/sim/lua_error.h`) instead of using
  `longjmp`, as Lua 5.1 does when built as C++, so errors unwind the engine's C++ functions.
  Scripts see the same errors. In `src/llimits.h`, `Instruction` is an `unsigned int`, not an
  `unsigned long`, so it stays 4 bytes on systems where `long` is 8 and the game's compiled
  scripts still load there. Also in `src/llimits.h`, `IntPoint` casts a pointer to `size_t` before
  the hash type, which C++ compilers such as GCC require. The hash is the same.
