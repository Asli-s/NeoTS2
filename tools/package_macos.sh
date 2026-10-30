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

codesign --force --sign - "$resources/neots2" "$macos/neots2-launcher" "$frameworks"/*.dylib
codesign --force --sign - "$app"
codesign --verify --strict "$app"

archive="$build/NeoTS2-$version-macos-arm64.zip"
rm -f "$archive"
ditto -c -k --sequesterRsrc --keepParent "$app" "$archive"
shasum -a 256 "$archive"
echo "Made $archive"
