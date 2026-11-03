#!/bin/sh
# Make the Apple Silicon app and zip that a macOS release ships.
set -eu

source=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-"$source/build-macos"}
if [ ! -f "$build/neots2" ] || [ ! -f "$build/neots2-launcher" ]; then
    echo "Build neots2 and neots2-launcher first: cmake --build $build --target neots2 neots2-launcher" >&2
    exit 1
fi
if ! command -v brew >/dev/null || ! command -v install_name_tool >/dev/null || ! command -v codesign >/dev/null; then
    echo "This script needs Homebrew and the macOS command-line tools." >&2
    exit 1
fi

version=$(sed -n 's/^project(NeoTS2 VERSION \([^ ]*\).*/\1/p' "$source/CMakeLists.txt")
if [ -z "$version" ]; then
    echo "Couldn't read NeoTS2's version from CMakeLists.txt." >&2
    exit 1
fi

app="$build/NeoTS2.app"
macos="$app/Contents/MacOS"
resources="$app/Contents/Resources"
frameworks="$app/Contents/Frameworks"
rm -rf "$app"
mkdir -p "$macos" "$resources/shaders" "$frameworks"
cp "$build/neots2-launcher" "$macos"
# SDL_GetBasePath() names Contents/Resources inside an app bundle, so keep the game and its
# shaders there for the launcher to find them beside each other.
cp "$build/neots2" "$resources"
cp "$build"/shaders/*.msl "$resources/shaders"
cp "$source/app/icon/neots2.icns" "$resources/neots2.icns"
sed "s/@NEOTS2_VERSION@/$version/g" "$source/cmake/macos/Info.plist.in" > "$app/Contents/Info.plist"

homebrew=$(brew --prefix)
copy_dependencies() {
    otool -L "$1" | tail -n +2 | awk '{print $1}' | while IFS= read -r library; do
        case "$library" in
            "$homebrew"/*.dylib)
                bundled="$frameworks/$(basename "$library")"
                if [ ! -f "$bundled" ]; then
                    cp "$library" "$bundled"
                    copy_dependencies "$library"
                fi
                ;;
        esac
    done
}
copy_dependencies "$resources/neots2"
copy_dependencies "$macos/neots2-launcher"

for binary in "$resources/neots2" "$macos/neots2-launcher" "$frameworks"/*.dylib; do
    otool -L "$binary" | tail -n +2 | awk '{print $1}' | while IFS= read -r library; do
        case "$library" in
            "$homebrew"/*.dylib)
                install_name_tool -change "$library" "@executable_path/../Frameworks/$(basename "$library")" "$binary"
                ;;
        esac
    done
done
for library in "$frameworks"/*.dylib; do
    install_name_tool -id "@executable_path/../Frameworks/$(basename "$library")" "$library"
done

# With crash reports (NEOTS2_CRASHPAD): the crash catcher's program goes beside the game, and the
# dSYM symbols for BugSplat are kept in symbols/ beside the app (upload them with BugSplat's
# symbol-upload) before the shipped programs are stripped. Their ids still match.
crashpad=no
if [ -f "$build/crashpad_handler" ]; then
    crashpad=yes
    cp "$build/crashpad_handler" "$resources"
    rm -rf "$build/symbols"
    mkdir -p "$build/symbols"
    for program in neots2 crashpad_handler; do
        dsymutil "$resources/$program" -o "$build/symbols/$program.dSYM"
        strip -S "$resources/$program"
    done
    codesign --force --sign - "$resources/crashpad_handler"
fi

# NeoTS2's license and every library's, as their licenses ask when they are shipped.
cp "$source/LICENSE" "$resources/LICENSE.txt"
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
    if [ "$crashpad" = yes ]; then
        licenses="$source/third_party/licenses/crashpad"
        notice "Crashpad (Apache License 2.0), in crashpad_handler and neots2" "$licenses/Crashpad-LICENSE.txt"
        notice "mini_chromium (BSD 3-Clause License), part of Crashpad" "$licenses/mini_chromium-LICENSE.txt"
        notice "getopt (public domain), part of Crashpad" "$licenses/getopt-LICENSE.txt"
        notice "MPack (MIT License), part of Crashpad" "$licenses/mpack-LICENSE.txt"
    fi
} > "$resources/THIRD-PARTY-NOTICES.txt"

codesign --force --sign - "$resources/neots2" "$macos/neots2-launcher" "$frameworks"/*.dylib
codesign --force --sign - "$app"
codesign --verify --strict "$app"

archive="$build/NeoTS2-$version-macos-arm64.zip"
rm -f "$archive"
ditto -c -k --sequesterRsrc --keepParent "$app" "$archive"
shasum -a 256 "$archive"
echo "Made $archive"
