# Lua 5.0.3

The Sims 2 runs object scripts compiled for Lua 5.0, so the engine embeds the original Lua 5.0.3
interpreter to run them exactly as the game does (for example the order `pairs` visits a table).

- Source: https://www.lua.org/ftp/lua-5.0.3.tar.gz
  (SHA-256 `1193a61b0e08acaa6eee0eecf29709179ee49c71baebc59b682a25c3b5a45671`)
- License: MIT, see `COPYRIGHT`.
- Unchanged copies of `include/`, the core in `src/`, and the auxiliary, base, math, string and
  table libraries from `src/lib/`. Left out: the I/O, dynamic-loading and debug libraries, the
  test build (`ltests.c`) and the standalone `lua` and `luac` programs.
