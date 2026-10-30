#!/bin/sh
# Make the tar.gz a Linux release ships: the launcher and the game, the shaders, the SDL 3 library
# they were built with, the licenses and a short start-here note.
#   tools/package_linux.sh [build folder]
# Build neots2 and neots2-launcher first. The programs look for SDL 3 in the lib folder beside them
# before the system's, so players need no SDL 3 package. FreeType comes from the system.
set -eu

source=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-"$source/build"}
if [ ! -f "$build/neots2" ] || [ ! -f "$build/neots2-launcher" ] || [ ! -d "$build/shaders" ]; then
    echo "Build neots2 and neots2-launcher first: cmake --build $build --target neots2 neots2-launcher" >&2
    exit 1
fi
if ! command -v patchelf >/dev/null; then
    echo "This script needs patchelf (sudo apt install patchelf)." >&2
    exit 1
fi

version=$(sed -n 's/^project(NeoTS2 VERSION \([^ ]*\).*/\1/p' "$source/CMakeLists.txt")
if [ -z "$version" ]; then
    echo "Couldn't read NeoTS2's version from CMakeLists.txt." >&2
    exit 1
fi

name="NeoTS2-$version-linux-x86_64"
stage="$build/package/$name"
rm -rf "$build/package"
mkdir -p "$stage/lib" "$stage/shaders"
cp "$build/neots2" "$build/neots2-launcher" "$stage"
cp "$build"/shaders/*.spv "$stage/shaders"

sdl=$(ldd "$build/neots2" | awk '$1 ~ /^libSDL3\.so/ { print $3 }')
if [ -z "$sdl" ] || [ ! -f "$sdl" ]; then
    echo "neots2 doesn't load an SDL 3 library that could be found." >&2
    exit 1
fi
cp -L "$sdl" "$stage/lib/$(basename "$sdl")"
for program in "$stage/neots2" "$stage/neots2-launcher"; do
    patchelf --set-rpath '$ORIGIN/lib' "$program"
done

cp "$source/LICENSE" "$stage/LICENSE.txt"

# Every library's license, as their licenses ask when they are shipped.
heading() {
    rule="=============================================================================="
    printf '\n%s\n%s\n%s\n\n' "$rule" "$1" "$rule"
}
notice() {
    heading "$1"
    cat "$2"
}
{
    echo "NeoTS2 uses these libraries, and ships them inside its programs or beside them."
    notice "SDL 3 (zlib License)" "$source/third_party/licenses/SDL3-LICENSE.txt"
    notice "FreeType (The FreeType Project License). Portions of this software are copyright (c) 2024 The FreeType Project (www.freetype.org). All rights reserved." \
        "$source/third_party/licenses/FreeType-FTL.txt"
    notice "Dear ImGui (MIT License)" "$source/third_party/imgui/LICENSE.txt"
    notice "Noto Sans (SIL Open Font License 1.1)" "$source/third_party/fonts/OFL.txt"
    heading "stb_image (MIT License or public domain)"
    # The last copy of the license text, at the end of the header.
    awk '/This software is available under 2 licenses/ { text = "" } { text = text $0 "\n" } END { printf "%s", text }' \
        "$source/third_party/stb/stb_image.h"
    notice "Lua 5.0.3 (MIT License)" "$source/third_party/lua-5.0.3/COPYRIGHT"
} > "$stage/THIRD-PARTY-NOTICES.txt"

cat > "$stage/Start here.txt" <<EOF
NeoTS2 $version for Linux (x86_64)

BACK UP YOUR SAVES FIRST. NeoTS2 is early, unfinished software. Copy your save folder somewhere
safe first, or make a new save folder for NeoTS2 with the launcher's "New save folder..." button.

You need your own copy of The Sims 2: the Ultimate Collection or the Legacy Collection, with its
game folder on this computer.

The Linux build is new and not well tested yet. If something goes wrong, please report it on
https://github.com/Asli-s/NeoTS2/issues with the log.

To play, run ./neots2-launcher. Choose your game folder and a save folder, and press Play.
NeoTS2 draws with Vulkan, so it needs a Vulkan driver (Mesa's or your GPU maker's).

The launcher's settings and NeoTS2's logs are kept in ~/.local/share/NeoTS2. To remove NeoTS2,
delete this folder and that one.
EOF

archive="$build/$name.tar.gz"
rm -f "$archive"
tar -C "$build/package" -czf "$archive" "$name"
sha256sum "$archive"
echo "Made $archive"
