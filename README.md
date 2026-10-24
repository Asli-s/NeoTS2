<h1><img src="app/icon/neots2.svg" alt="" height="28"> NeoTS2</h1>

**A new, open, 64-bit engine for The Sims 2.**

NeoTS2 runs The Sims 2 on a modern, open codebase. It reads the game's own files from your
installation (neighborhoods, lots, objects, Sims, scripts, 3D assets and the user interface) and
rebuilds the game on top of them: the simulation that drives objects and Sims, the houses and
neighborhoods in 3D, and the game's screens.

> **You need your own copy of The Sims 2** (the Ultimate Collection or the base game with its
> packs). NeoTS2 contains none of the game's files and never will.

## Why

The Sims 2 engine is twenty years old: a 32-bit, fixed-function program tied to old hardware and
closed to deeper modding. NeoTS2 aims to keep everything that makes the game what it is while
giving it:

- **a native 64-bit (x64) engine** that uses modern graphics APIs (Direct3D 12 today) and the
  memory a modern PC has
- **an engine you can read and change**
- **modern graphics as an option**: the classic look by default, with room for improved
  lighting, shadows and effects as mods
- **modding without limits**: new behavior, new rendering, new tools, built on code rather
  than workarounds

The classic look is rebuilt from how the original game works and is not approximated: where the game
computes something (terrain paint, lighting, camera framing), NeoTS2 does the same computation.

## What works today

NeoTS2 is in early development. It isn't a full game yet, but you can already visit your
neighborhoods and lots:

- **Launcher:** finds or lets you choose your Ultimate Collection or Legacy Collection and your
  save folders
- **Main menu:** the game's neighborhood chooser, built from its own UI scripts, fonts and
  images
- **Neighborhoods:** the terrain with the game's paints, lighting and shadows, and its
  reflective water
- **Lots:** walls with doors and windows, floors, roofs, fences and objects, in the game's
  daylight and night lighting, with bump-mapped surfaces
- **Live mode:** the game's camera for lots (drag, scroll, zoom and turn), and its panels: needs,
  mood and aspiration bars
- **Sims:** your Sims are on their lots, but what they do is still very early and buggy
- **Game files:** the game's packages and resource formats: objects, string tables,
  behavior scripts, 3D models and textures, saved neighborhoods, families, lots and Sims

Custom content (your Downloads folder) isn't read yet, so objects, clothes, hair and other
downloads don't show in NeoTS2 for now.

*Screenshots from an unreleased development version of NeoTS2.*

| Main menu | Neighborhood |
| --- | --- |
| ![The main menu](assets/screenshots/main-menu.jpg) | ![Pleasantview with its lots panel](assets/screenshots/neighborhood.webp) |
| **Lot** | **Sims** |
| ![A lot with its Sims and live panel](assets/screenshots/lot.webp) | ![Two Sims talking on a deck](assets/screenshots/sims.webp) |

## Your saves

> **Back up your saves before you try NeoTS2.** It is early, unfinished software. Copy your save
> folder (in Documents\EA Games, for example The Sims™ 2 Ultimate Collection) somewhere safe first, or
> make a new save folder for NeoTS2 with the launcher's **New save folder...**.

NeoTS2 doesn't save games yet, and when it starts with a save folder:

- a folder that already has anything in it is loaded as it is, and nothing is added or changed
- only a missing or empty folder is set up, and the copy is made beside it and moved in at the
  end, so a crash or power cut part way leaves the folder as it was
- if a save folder has files but no neighborhoods, the launcher asks before adding the ones your
  install ships
- what your install holds is copied as it is, so if you removed the shipped neighborhoods or
  neighborhood templates from your install, they stay removed
- system folders and the game's own install folder are never used as save folders

## Running

**Back up your saves first** (see [Your saves](#your-saves)).

Download the newest release from the [Releases page](https://github.com/Asli-s/NeoTS2/releases),
extract the whole zip, and start **`neots2-launcher.exe`**. It finds your install and your save
folders. Choose them, or make a new save folder with **New save folder...**, and press **Play**.

Windows may say "Windows protected your PC", because NeoTS2 isn't signed yet. Click **More info**,
then **Run anyway**.

## Tools and libraries

| What | Used for |
| --- | --- |
| C++20, CMake 3.20+ | the language and the build |
| Visual Studio 2022 (MSVC), x64 | the compiler on Windows; NeoTS2 is built and tested as 64-bit |
| [SDL 3](https://www.libsdl.org/) | the window, input and GPU access (Direct3D 12) |
| DXC, from the Windows SDK | compiling the HLSL shaders |
| [FreeType](https://freetype.org/) 2.13 | drawing the game's fonts |
| [stb_image](https://github.com/nothings/stb) 2.30 (included) | reading PNG, JPEG and BMP images |
| [Lua](https://www.lua.org/) 5.0.3 (included) | the scripting language the game's expansions use |

## Building

You need CMake 3.20+, Visual Studio 2022 with the C++ workload and the Windows SDK, the
[SDL 3](https://github.com/libsdl-org/SDL/releases) development package for Visual Studio, and
FreeType 2.13 built with CMake. Then:

```
cmake -S . -B build -A x64 -DSDL3_DIR=<SDL3>/cmake -Dfreetype_DIR=<FreeType install>/lib/cmake/freetype
cmake --build build --config Release
```

This builds `neots2`. Without SDL 3 or DXC, the libraries, tools and tests still build; the app
is skipped.

### OpenGL 2 renderer

NeoTS2 draws with SDL's GPU API (Direct3D 12 on Windows, Metal on macOS, Vulkan elsewhere). It can also draw with
OpenGL 2.1, for systems with no more: start it with `--renderer gl2` (the launcher has a Graphics
choice). It is not built unless you ask for it with `-DNEOTS2_GL2=ON`, which needs `spirv-cross` (from the Vulkan SDK
or your package manager; or give `-DNEOTS2_SPIRV_CROSS=<path>`). The launcher offers it only in a build that has it.
If it can't start (a driver below OpenGL 2.1), the game says so and draws with the default renderer.
Its GLSL 1.20 shaders are made from the same HLSL by DXC (built with SPIR-V) and SPIRV-Cross.

The OpenGL 2 renderer draws the same things, some another way: the neighborhood terrain takes a pass
for each paint texture (there are no texture arrays) and Sims are posed on the CPU. The water's reflection
and the pictures drawn off-screen (portraits) need framebuffer objects: without them the reflection is left
out and the pictures are drawn in a corner of the window. Scenes are uploaded on other threads where the
driver lets contexts share objects.

### OpenGL 1 renderer (work in progress)

`--renderer gl1` draws without shaders, with the fixed pipeline of OpenGL 1.5, as a step toward
older hardware. It needs neither DXC nor SPIRV-Cross, and is left out with `-DNEOTS2_GL1=OFF`. The
game's lighting is worked out for each vertex on the CPU. It draws lots, the neighborhood (terrain,
roads, lot imposters, a flat water without a reflection, the sky) and the UI, and leaves out normal
maps, specular and environment cubes. Floors and walls draw their texture and then multiply their
light map in with a second pass. The terrain's paint weights and underwater gradient are worked out
once when the scene is uploaded. The terrain and roads are drawn with one texture a pass, their
weights and light map worked out at the vertices, which is all the oldest hardware can do. F9 switches
them to two texture units (weights and light map per pixel), to compare the two. Objects are lit
brighter than their textures where the game's lights add up past 1 (the texture environment multiplies
by 2, and the colours are kept at half); F10 clamps the light at 1 instead. Floors and walls take their
light map's light at their vertices (one sample a tile on a floor), as the oldest hardware must; F11
multiplies the light map in with a second pass for comparing, as before. The Dreamcast has no such
multiplier, the PlayStation 2 does (0x80 is full brightness). Vertex arrays are one tightly packed block
for each attribute, and indices are 16-bit where they fit.

Textures are powers of two from 8 to 1024 on a side, as the consoles take them. Images are resampled to
fit; UI sprites, which are never repeated, are padded instead (their texture coordinates shrink to match).
A lot's sky is a box of six pictures rather than a cube map. UI sprites have a cap of their own
(`NEOTS2_GL1_MAX_SPRITE`, 1024 by default), so a small `NEOTS2_GL1_MAX_TEXTURE` leaves the UI alone.
`NEOTS2_GL1_TEXTURE_DETAIL=0.25` makes each scene image only as big as the screen needs it (times that,
as texels for each pixel across a repeat of it), growing from 32 pixels as things are looked at, to see
what small textures cost. How close the camera is doesn't matter past a limit for each kind of
thing: floors, walls, roofs and the ground around a lot are given at most 128 texels across
(`NEOTS2_GL1_SURFACE_SIDE`), things 256 (`NEOTS2_GL1_OBJECT_SIDE`), and Sims, who get twice the detail,
512 (`NEOTS2_GL1_SIM_SIDE`). `NEOTS2_GL1_PALETTE=4` (or 8) shows textures with only 16 (or 256) colours, by
median cut (improved by k-means passes), to see what a console's palettes would keep. `NEOTS2_GL1_DITHER=1`
dithers them. `NEOTS2_GL1_PALETTE=auto` gives each texture a 4-bit palette if that changes it by no more than
`NEOTS2_GL1_PALETTE_ERROR` (8 by default, in levels RMS), else 8-bit if that changes it by half as much, else none. The strict mode also logs, for each scene, how big its images would need to be on a screen
`NEOTS2_GL1_TARGET_WIDTH` pixels wide (640 by default), as 16-bit texels and as 8- and 4-bit palettes. To see what the consoles' limits look
like, `NEOTS2_GL1_MAX_TEXTURE=256` caps textures at 256 a side and `NEOTS2_GL1_TEXTURE_BITS=16` keeps them
at 16 bits a texel (4-4-4-4 where any is see-through, else 5-5-5). With `NEOTS2_GL1_STRICT=1`, the log ends
with how many textures there were, and how much memory they would take.

Set `NEOTS2_GL1_STRICT=1` to have it report, once each in the log, the calls the console OpenGL
libraries (GLdc and ps2gl) can't do: blend factors, alpha tests, texture sizes, 32-bit indices, a
second texture unit and so on. The calls still run, so a PC shows what a console build would have
to do differently. The rules are in `src/render/gl_subset.cpp`, taken from those libraries' source.

A build from this source starts from the command line:

```
neots2 "<The Sims 2 install folder>" --menu --user "<your Sims 2 user folder>"
neots2 "<The Sims 2 install folder>" "<user folder>/Neighborhoods/N001"
```

The first opens the main menu, the second a neighborhood.

For example, with the Ultimate Collection installed, open a Command Prompt in the folder you 
built into and run:

```
cd build\Release
neots2.exe "C:\Program Files\EA Games\The Sims 2 Ultimate Collection" --menu
```

`--user` can be left out when your user folder is the usual one, in Documents\EA Games\The Sims™ 2
Ultimate Collection. If the game is installed elsewhere, use the folder that holds its "Double
Deluxe", "Fun with Pets" and other pack folders.

## Crash reports

If NeoTS2 crashes, it saves a report in the `logs` folder next to it (or in
`%LOCALAPPDATA%\NeoTS2\logs`). Nothing is sent unless you click **Send a report**.

A report has two files: `crash.txt` (what went wrong) and `neots2.dmp` (a snapshot of the program
when it crashed). In `crash.txt` your user folder is shown as `%USERPROFILE%`; the dump may still
contain folder names from your computer.

**Send a report** uploads only those two files to [BugSplat](https://www.bugsplat.com), which keeps
them for a month. I use them only to fix crashes. You can also open a GitHub issue and attach them
yourself (GitHub issues are public).

## How NeoTS2 is made

Much of the code is written together with AI (Claude), and I direct the work. Formats and
behavior come from studying the game itself, but only the cases we've tested are verified, and
some details are still open. If you use NeoTS2 as a reference for the game's file formats, check
anything important against the game yourself.

## Credits

Thanks to The Sims 2 community: [SimPE](https://sourceforge.net/projects/simpe/) was used as a
reference (none of its code is used). The AI has likely picked up general knowledge, like file
formats and where things are stored, from public community work such as
[OpenTS2](https://github.com/LazyDuchess/OpenTS2). None of OpenTS2's code is used.

Thanks to [kouta-kun](https://github.com/kouta-kun) for the Linux build, the Vulkan path, the
OpenGL 2 renderer and the Mac OS X PowerPC port.

The app and launcher icons are hand-drawn by me.

The launcher uses [Dear ImGui](https://github.com/ocornut/imgui) (MIT) and the
[Noto Sans](https://fonts.google.com/noto/specimen/Noto+Sans) font (SIL Open Font License 1.1).

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) first. In short:

- no game files, and nothing exported from them
- no code taken from the game or from projects whose license doesn't allow it

## Disclaimer

NeoTS2 is an independent fan project. It is not affiliated with, endorsed by or connected to
Electronic Arts or Maxis. The Sims and the plumbob are trademarks of Electronic Arts Inc.

Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org). All
rights reserved.

## License

NeoTS2 is free to use, change, copy and share, and to build on: fork it, make mods and tools for
it, use its code in your own projects. Mods, add-ons, content and tools made for NeoTS2 are yours,
and you may sell them. The one thing you may not do is sell NeoTS2 itself, or a product or service
that is essentially NeoTS2.

[MIT with the Commons Clause](LICENSE)
