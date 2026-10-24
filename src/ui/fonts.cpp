#include "ui/fonts.hpp"

#include "content/install.hpp"
#include "diag/log.hpp"

#include "ui/font_metrics.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_FONT_FORMATS_H

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

namespace ts2::ui {

namespace {

namespace fs = std::filesystem;

// The widest line or letter spacing taken, in pixels, so sizes added to it can't overflow.
constexpr int kMaxSpacing = 2048;

// The widest line measured, in 26.6 units: far past the largest text picture, and small enough
// that adding a few more advances can't overflow.
constexpr std::int64_t kMaxPen = std::int64_t{1} << 40;

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// A whole number at the start of `text`, read as atoi reads it (leading spaces and a sign allowed)
// but held within -limit..limit, so a broken file's huge value can't overflow. 0 when there is none.
int styleNumber(std::string_view text, int limit) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    long long value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error == std::errc::result_out_of_range) {
        return text.front() == '-' ? -limit : limit;
    }
    return static_cast<int>(std::clamp<long long>(value, -limit, limit));
}

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::vector<std::uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The comma-separated fields of a style line, quotes removed.
std::vector<std::string> fields(const std::string& value) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (const char c : value) {
        if (c == '"') {
            quoted = !quoted;
        } else if (c == ',' && !quoted) {
            out.push_back(trim(current));
            current.clear();
        } else if (c == ';' && !quoted) {
            break;
        } else {
            current += c;
        }
    }
    out.push_back(trim(current));
    return out;
}

// Code points of a UTF-8 string.
std::vector<char32_t> codePoints(const std::string& s) {
    std::vector<char32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && i + static_cast<std::size_t>(k) < s.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) & 0x3F);
        }
        out.push_back(cp);
        i += static_cast<std::size_t>(n);
    }
    return out;
}

} // namespace

std::vector<std::uint8_t> decodeMxf(const std::vector<std::uint8_t>& file) {
    // "MXFN", four bytes, then the font file with every byte XOR 0x9D (its first byte, 0, unused).
    if (file.size() < 10 || file[0] != 'M' || file[1] != 'X' || file[2] != 'F' || file[3] != 'N') {
        return {};
    }
    std::vector<std::uint8_t> out(file.begin() + 9, file.end());
    for (auto& b : out) {
        b ^= 0x9D;
    }
    return out;
}

// The highest or lowest point of a character's outline in font units (its control points too, as
// the game measures it), or nothing when the font lacks the character.
std::optional<int> outlineExtreme(FT_Face face, char32_t c, bool highest) {
    const FT_UInt glyph = FT_Get_Char_Index(face, c);
    if (glyph == 0 || FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0 ||
        face->glyph->format != FT_GLYPH_FORMAT_OUTLINE || face->glyph->outline.n_points <= 0) {
        return std::nullopt;
    }
    const FT_Outline& o = face->glyph->outline;
    FT_Pos best = o.points[0].y;
    for (int i = 1; i < o.n_points; ++i) {
        best = highest ? std::max(best, o.points[i].y) : std::min(best, o.points[i].y);
    }
    return static_cast<int>(std::clamp<FT_Pos>(best, -32767, 32767));
}

// FreeType's library and faces are closed by their owners' destructors, so nothing is left open
// even when loading stops halfway with an error.
struct Fonts::Impl {
    FT_Library library = nullptr;
    struct Face {
        std::vector<std::uint8_t> data;  // FreeType reads from it while the face is open
        FT_Face face = nullptr;
        std::string family, fullName;  // the full name: family and style ("HelveticaNeueLT Std Medium")
        bool bold = false;
        // Type 1 faces: the ascender and descender the game's font engine measures them by (the
        // top of 'f' and the bottom of 'g', else 750 and -250, as the game's fonts carry no values
        // of their own). Other faces are measured by FreeType.
        bool typeOne = false;
        int ascender = 750, descender = -250;
        ~Face() {
            if (face) {
                FT_Done_Face(face);
            }
        }
    };
    std::vector<std::unique_ptr<Face>> faces;
    std::map<std::string, FontStyle> styles;  // by lowercase name
    std::map<std::uint32_t, std::string> byGuid;
    FontStyle fallback;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        faces.clear();  // faces close before the library
        if (library) {
            FT_Done_FreeType(library);
        }
    }

    // The face a style draws with: its first named face found, the one matching its boldness.
    Face* find(const FontStyle& style) {
        for (const auto& name : style.faces) {
            const std::string want = lower(name);
            Face* best = nullptr;
            for (auto& f : faces) {
                if ((lower(f->family) == want || lower(f->fullName) == want) && (!best || (f->bold == style.bold && best->bold != style.bold))) {
                    best = f.get();
                }
            }
            if (best) {
                return best;
            }
        }
        return nullptr;
    }

    // The face a style draws with, set to its size.
    FT_Face pick(const FontStyle& style, int& size) {
        Face* best = find(style);
        if (!best) {
            return nullptr;
        }
        size = style.size;
        // A narrowed style is drawn at a narrower width, hinted at that width.
        FT_Set_Char_Size(best->face, static_cast<FT_F26Dot6>(std::lround(static_cast<float>(style.size) * style.xScale * 64)),
                         static_cast<FT_F26Dot6>(style.size) * 64, 72, 72);
        return best->face;
    }

    void readStyles(std::istream& in) {
        std::string line, section;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == ';') {
                continue;
            }
            if (line[0] == '[') {
                section = lower(line);
                continue;
            }
            const auto eq = line.find('=');
            if (section != "[font styles]" || eq == std::string::npos) {
                continue;
            }
            const auto f = fields(line.substr(eq + 1));
            if (f.size() < 2) {
                continue;
            }
            FontStyle s;
            std::stringstream faceList(f[0]);
            for (std::string face; std::getline(faceList, face, ',');) {
                s.faces.push_back(trim(face));
            }
            // Sizes are clamped to 1..2048 pixels, as the game does.
            s.size = std::clamp(styleNumber(f[1], 2048), 1, 2048);
            if (f.size() > 2) {
                std::stringstream options(lower(f[2]));
                for (std::string o; std::getline(options, o, '|');) {
                    o = trim(o);
                    s.bold = s.bold || o == "bold";
                    s.italic = s.italic || o == "italic";
                    s.marks |= o == "under" || o == "underline" ? 1u : o == "strike" || o == "strikethrough" ? 2u : o == "shadow" ? 4u : 0u;
                    s.unshared = s.unshared || o == "private";
                    if (o.rfind("linespacing=", 0) == 0) {
                        s.lineSpacing = styleNumber(std::string_view(o).substr(12), kMaxSpacing);
                    }
                    if (o.rfind("charspacing=", 0) == 0) {
                        s.charSpacing = styleNumber(std::string_view(o).substr(12), kMaxSpacing);
                    }
                    if (o.rfind("xscale=", 0) == 0) {
                        s.xScale = static_cast<float>(std::atof(o.c_str() + 7));
                    }
                    if (o.rfind("yscale=", 0) == 0) {
                        s.yScale = static_cast<float>(std::atof(o.c_str() + 7));
                    }
                }
                // Safety limits for broken files (the game's own styles use spacings of -2..3 and
                // scales of 0.8..1.25): a huge spacing overflows a text's height, and a scale that
                // is zero, negative or huge makes glyphs of no size or of gigabytes.
                if (s.lineSpacing < -2048 || s.lineSpacing > 2048) {
                    diag::warnOnce("fonts: a line spacing past 2048 pixels is taken as 0");
                    s.lineSpacing = 0;
                }
                if (s.charSpacing < -2048 || s.charSpacing > 2048) {
                    diag::warnOnce("fonts: a letter spacing past 2048 pixels is taken as 0");
                    s.charSpacing = 0;
                }
                if (!(s.xScale > 0.0f && s.xScale <= 16.0f)) {
                    diag::warnOnce("fonts: a width scale outside 0..16 is taken as 1");
                    s.xScale = 1;
                }
                if (!(s.yScale > 0.0f && s.yScale <= 16.0f)) {
                    diag::warnOnce("fonts: a height scale outside 0..16 is taken as 1");
                    s.yScale = 1;
                }
            }
            if (f.size() > 3) {
                s.guid = static_cast<std::uint32_t>(std::strtoul(f[3].c_str(), nullptr, 16));
            }
            const std::string name = lower(trim(line.substr(0, eq)));
            styles[name] = s;
            byGuid[s.guid] = name;
            read.push_back(s);
        }
    }

    // The game makes one font for all styles with the same face, size, boldness, slant and marks,
    // unless a style is private. Its spacings and scales are set on that one font, style by style
    // in file order, so the last style that sets them decides them for all. Spacings are set
    // together when either isn't 0 and scales together when either isn't 1.
    void shareFonts() {
        using Key = std::tuple<const Face*, std::string, int, bool, bool, unsigned>;
        auto key = [this](const FontStyle& s) {
            const Face* face = find(s);
            std::string names;
            for (const auto& n : s.faces) {
                names += lower(n) + ",";
            }
            return Key{face, face ? std::string() : names, s.size, s.bold, s.italic, s.marks};
        };
        struct Set {
            std::optional<std::array<int, 2>> spacings;
            std::optional<std::array<float, 2>> scales;
        };
        std::map<Key, Set> set;
        for (const auto& s : read) {
            if (s.unshared) {
                continue;
            }
            Set& font = set[key(s)];
            if (s.charSpacing != 0 || s.lineSpacing != 0) {
                font.spacings = std::array<int, 2>{s.charSpacing, s.lineSpacing};
            }
            if (s.xScale != 1 || s.yScale != 1) {
                font.scales = std::array<float, 2>{s.xScale, s.yScale};
            }
        }
        for (auto& [name, s] : styles) {
            const auto font = s.unshared ? set.end() : set.find(key(s));
            if (font == set.end()) {
                continue;
            }
            if (font->second.spacings) {
                s.charSpacing = (*font->second.spacings)[0];
                s.lineSpacing = (*font->second.spacings)[1];
            }
            if (font->second.scales) {
                s.xScale = (*font->second.scales)[0];
                s.yScale = (*font->second.scales)[1];
            }
        }
        read.clear();
    }

    std::vector<FontStyle> read;  // every style line in the order read, until the fonts are shared

    // Opens a font file's contents (Type 1 or TrueType). Unreadable data is left out.
    void addFace(std::vector<std::uint8_t> data) {
        auto face = std::make_unique<Face>();
        face->data = std::move(data);
        if (face->data.empty() ||
            FT_New_Memory_Face(library, face->data.data(), static_cast<FT_Long>(face->data.size()), 0, &face->face) != 0) {
            face->face = nullptr;
            return;
        }
        face->family = face->face->family_name ? face->face->family_name : "";
        face->fullName = face->family + " " + (face->face->style_name ? face->face->style_name : "");
        face->bold = (face->face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
        if (const char* format = FT_Get_Font_Format(face->face); format && std::string_view(format) == "Type 1") {
            face->typeOne = true;
            face->ascender = outlineExtreme(face->face, U'f', true).value_or(750);
            face->descender = outlineExtreme(face->face, U'g', false).value_or(-250);
        }
        faces.push_back(std::move(face));
    }

    // After every style and face is read: the shared fonts' settings and the Default style.
    void finish() {
        shareFonts();
        const auto d = styles.find("default");
        if (d != styles.end()) {
            fallback = d->second;
        }
    }
};

Fonts::Fonts(const fs::path& install) : impl_(std::make_unique<Impl>()) {
    FT_Init_FreeType(&impl_->library);
    std::set<fs::path> seen;
    for (const auto& product : content::findProducts(install)) {
        for (const auto& objects : product.objectPackages) {
            fs::path res = objects.parent_path();
            while (res.has_relative_path() && lower(utf8(res.filename())) != "res") {
                res = res.parent_path();
            }
            const fs::path folder = res / "UI" / "Fonts";
            if (!fs::is_directory(folder) || !seen.insert(folder).second) {
                continue;
            }
            if (std::ifstream ini(folder / "FontStyle.ini"); ini) {
                impl_->readStyles(ini);
            }
            for (const auto& item : fs::directory_iterator(folder)) {
                if (lower(utf8(item.path().extension())) == ".mxf") {
                    impl_->addFace(decodeMxf(readFile(item.path())));
                }
            }
        }
    }
    impl_->finish();
}

Fonts::Fonts(const std::string& styles, std::vector<std::vector<std::uint8_t>> files) : impl_(std::make_unique<Impl>()) {
    FT_Init_FreeType(&impl_->library);
    std::istringstream ini(styles);
    impl_->readStyles(ini);
    for (auto& file : files) {
        impl_->addFace(std::move(file));
    }
    impl_->finish();
}

Fonts::~Fonts() = default;

std::size_t Fonts::faceCount() const {
    return impl_->faces.size();
}

const FontStyle& Fonts::style(const std::string& nameOrGuid) const {
    std::string name = lower(nameOrGuid);
    if (name.rfind("0x", 0) == 0) {
        const auto g = impl_->byGuid.find(static_cast<std::uint32_t>(std::strtoul(name.c_str(), nullptr, 16)));
        if (g == impl_->byGuid.end()) {
            return impl_->fallback;
        }
        name = g->second;
    }
    const auto s = impl_->styles.find(name);
    return s == impl_->styles.end() ? impl_->fallback : s->second;
}

int Fonts::width(const FontStyle& style, const std::string& utf8) {
    int size = 0;
    FT_Face face = impl_->pick(style, size);
    if (!face) {
        return 0;
    }
    std::int64_t w = 0;
    for (const char32_t c : codePoints(utf8)) {
        if (w > kMaxPen) {
            break;
        }
        if (FT_Load_Char(face, c, FT_LOAD_DEFAULT) == 0) {
            w += face->glyph->advance.x;
        }
    }
    return static_cast<int>(std::min<std::int64_t>((w + 63) / 64, std::numeric_limits<int>::max() / 2));
}

namespace {

// How far the baseline is below the top of a line: the face's ascender at the style's narrowed size,
// rounded up.
int ascentOf(FT_Face face, const FontStyle& style) {
    if (face->units_per_EM == 0) {  // a bitmap-only face has no outline units
        return style.size;
    }
    return static_cast<int>(std::ceil(static_cast<double>(face->ascender) * style.size * style.xScale / face->units_per_EM));
}

} // namespace

int Fonts::ascent(const FontStyle& style) {
    int size = 0;
    FT_Face face = impl_->pick(style, size);
    return face ? ascentOf(face, style) : style.size;
}

std::optional<LineMetrics> Fonts::gameLine(const FontStyle& style) {
    const Impl::Face* face = impl_->find(style);
    if (!face || !face->typeOne) {
        return std::nullopt;
    }
    return typeOneLine(face->ascender, face->descender, 1000, style.size);
}

int Fonts::lineHeight(const FontStyle& style) {
    return std::max(1, style.size + style.lineSpacing);
}

TextImage Fonts::draw(const FontStyle& style, const std::string& utf8) {
    TextImage out;
    int size = 0;
    FT_Face face = impl_->pick(style, size);
    if (!face || utf8.empty()) {
        return out;
    }
    const int ascent = ascentOf(face, style);
    const int descent = static_cast<int>(-face->size->metrics.descender / 64);
    const int w = width(style, utf8);
    // A broken face can measure a line of no height. A line is cut at the largest picture the
    // GPU takes, so a string of tens of thousands of letters can't ask for gigabytes.
    if (w <= 0 || ascent + descent + 1 <= 0) {
        return out;
    }
    out.baseline = ascent;
    out.pixels.width = static_cast<std::uint32_t>(std::min(w + 2, kMaxTextImage));
    out.pixels.height = static_cast<std::uint32_t>(std::min(ascent + descent + 1, kMaxTextImage));
    out.pixels.rgba.assign(static_cast<std::size_t>(out.pixels.width) * out.pixels.height * 4, 0);
    for (std::size_t i = 0; i < out.pixels.rgba.size(); i += 4) {
        out.pixels.rgba[i] = out.pixels.rgba[i + 1] = out.pixels.rgba[i + 2] = 255;
    }
    std::int64_t pen = 0;
    for (const char32_t c : codePoints(utf8)) {
        if (pen / 64 > std::int64_t{out.pixels.width} + size) {
            break;  // the rest of the line falls outside the picture, even a glyph reaching back
        }
        if (FT_Load_Char(face, c, FT_LOAD_RENDER) != 0) {
            continue;
        }
        const FT_Bitmap& b = face->glyph->bitmap;
        if (b.pixel_mode != FT_PIXEL_MODE_GRAY) {  // only 8-bit coverage is read below
            pen += face->glyph->advance.x;
            continue;
        }
        // Rows run upward in memory when the pitch is negative.
        const std::size_t stride = static_cast<std::size_t>(b.pitch < 0 ? -b.pitch : b.pitch);
        auto row = [&](unsigned y) { return b.pitch < 0 ? (b.rows - 1 - y) * stride : y * stride; };
        const int x0 = static_cast<int>(pen / 64) + face->glyph->bitmap_left;
        const int y0 = ascent - face->glyph->bitmap_top;
        for (unsigned y = 0; y < b.rows; ++y) {
            for (unsigned x = 0; x < b.width; ++x) {
                const int px = x0 + static_cast<int>(x), py = y0 + static_cast<int>(y);
                if (px < 0 || py < 0 || px >= static_cast<int>(out.pixels.width) || py >= static_cast<int>(out.pixels.height)) {
                    continue;
                }
                auto& a = out.pixels.rgba[(static_cast<std::size_t>(py) * out.pixels.width + static_cast<std::size_t>(px)) * 4 + 3];
                a = std::max<std::uint8_t>(a, b.buffer[row(y) + x]);
            }
        }
        pen += face->glyph->advance.x;
    }
    return out;
}

} // namespace ts2::ui
