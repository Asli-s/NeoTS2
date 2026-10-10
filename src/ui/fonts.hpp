#pragma once

#include "formats/tga.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts2::ui {

// A named text style from the game's FontStyle.ini: font faces in order of preference, the size
// (a pixel height) and options such as bold.
struct FontStyle {
    std::vector<std::string> faces;
    int size = 11;
    bool bold = false, italic = false;
    int lineSpacing = 0;  // pixels added between lines
    std::uint32_t guid = 0;
};

// A line of text drawn in one style: white glyphs with their coverage as alpha, and where the
// baseline is (from the image's top).
struct TextImage {
    tga::Pixels pixels;
    int baseline = 0;
};

// The game's fonts (TSData/Res/UI/Fonts: Maxis .mxf font files and FontStyle.ini, every product
// read in release order, later styles replacing earlier ones) drawn with FreeType.
class Fonts {
public:
    explicit Fonts(const std::filesystem::path& install);
    ~Fonts();
    Fonts(const Fonts&) = delete;
    Fonts& operator=(const Fonts&) = delete;

    // A style by name or by its GUID written in hex ("0x6d226524"); the Default style otherwise.
    const FontStyle& style(const std::string& nameOrGuid) const;

    // How wide a string is in a style, in pixels.
    int width(const FontStyle& style, const std::string& utf8);
    // The height of a line in a style: its size and line spacing (as a neighborhood description's lines
    // in the game); and how far its baseline is below its top.
    int lineHeight(const FontStyle& style);
    int ascent(const FontStyle& style);
    // A string drawn in a style (an empty image for empty text or a missing face).
    TextImage draw(const FontStyle& style, const std::string& utf8);

    std::size_t faceCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A Maxis font file's contents: the font file it wraps (Type 1 or TrueType), or empty.
std::vector<std::uint8_t> decodeMxf(const std::vector<std::uint8_t>& file);

} // namespace ts2::ui
