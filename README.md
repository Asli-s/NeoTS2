# NeoTS2

**A new, open engine for The Sims 2.**

NeoTS2 runs The Sims 2 on a modern, open codebase. It reads the game's own files from your
installation: neighborhoods, lots, objects, Sims, scripts and 3D assets. Then it rebuilds the
game on top of them: the simulation that drives objects and Sims, and the house and its
contents in 3D.

> **You need your own copy of The Sims 2** (the Ultimate Collection or the base game with its
> packs). NeoTS2 contains none of the game's files and never will.

## Why

The Sims 2 is still loved, but its engine is twenty years old: it is fixed-function, tied to
old hardware and closed to deeper modding. NeoTS2 aims to keep everything that makes the game
what it is (its objects, its saves, its behavior) while giving it:

- **an engine you can read and change**, under the MIT license;
- **modern graphics as an option**: the classic look by default, with room for improved
  lighting, shadows and effects as mods;
- **modding without limits**: new behavior, new rendering, new tools, built on code rather
  than workarounds.

## What works today

NeoTS2 is in early development. It isn't playable yet, but the foundation is taking shape:

- **Game files:** reading the game's packages and most resource formats: objects, string
  tables, behavior scripts, 3D models and textures, saved neighborhoods, families, lots and
  Sims.
- **Simulation:** running object behavior (SimAntics, and the Lua scripts the expansions use)
  against a saved lot, with a growing set of the game's primitives.

Next up is the renderer, followed by animation and Sims.

## Building

Requires CMake 3.20+ and a C++20 compiler (Visual Studio 2022 on Windows).

```
cmake -S . -B build
cmake --build build --config Release
```

## Contributing

Contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first. In short:

- no game files, and nothing exported from them;
- no code taken from the game or from projects whose license doesn't allow it.

## Disclaimer

NeoTS2 is an independent fan project. It is not affiliated with, endorsed by or connected to
Electronic Arts or Maxis. The Sims is a trademark of Electronic Arts Inc.

## License

[MIT](LICENSE)
