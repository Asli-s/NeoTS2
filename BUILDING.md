# Building NeoTS2

The libraries, tools and tests build without SDL 3 or a shader compiler. Only the app needs them.

## Windows

Requirements:

- CMake 3.20 or newer
- Visual Studio 2022 with the C++ workload and the Windows SDK (for DXC)
- [SDL 3](https://github.com/libsdl-org/SDL/releases) development package for Visual Studio
- FreeType 2.13, built with CMake

```
cmake -S . -B build -A x64 -DSDL3_DIR=<SDL3>/cmake -Dfreetype_DIR=<FreeType>/lib/cmake/freetype
cmake --build build --config Release
```

## macOS (Apple Silicon)

NeoTS2 does not run the Windows game. It loads the data from a Windows Ultimate Collection
installation copied to the Mac, with the pack folders kept together, and renders with Metal.

```
brew install cmake sdl3 shaderc spirv-cross freetype
cmake -S . -B build-macos -DNEOTS2_STRICT=OFF
cmake --build build-macos --target neots2 neots2-launcher --parallel
```

Run `build-macos/neots2-launcher` and select the copied game folder. To package the app and the
release zip, run `tools/package_macos.sh build-macos`.

Shaders are compiled from HLSL to SPIR-V with Shaderc and converted to Metal with SPIRV-Cross,
because DXC has no macOS release.

## Linux

As on macOS, NeoTS2 loads the data from a copied Windows installation. It renders with Vulkan.

```
sudo apt install cmake ninja-build pkg-config libsdl3-dev libfreetype-dev glslc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target neots2 neots2-launcher
```

Run `build/neots2-launcher`. To package the release archive, run `tools/package_linux.sh build`
(requires `patchelf`).

## OpenGL 2 renderer

The OpenGL 2 renderer is for older systems. Enable it with `-DNEOTS2_GL2=ON`, which requires
`spirv-cross` (or `-DNEOTS2_SPIRV_CROSS=<path>`). Select it in the launcher's Graphics setting, or
start NeoTS2 with `--renderer gl2`. If the driver doesn't support OpenGL 2.1, NeoTS2 falls back to
the default renderer. Water reflections need framebuffer objects and are skipped without them.

## Running from the command line

```
neots2 "<game folder>" --menu --user "<save folder>"
neots2 "<game folder>" "<save folder>/Neighborhoods/N001"
```

The first command opens the main menu, the second opens a neighborhood. `--user` can be omitted if
the save folder is in its default location in Documents\EA Games. The game folder is the one that
contains "Double Deluxe", "Fun with Pets" and the other pack folders.
