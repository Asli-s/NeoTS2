#include "ui/fonts.hpp"

#include "content/install.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace ts2::ui {

namespace {

namespace fs = std::filesystem;

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

// FreeType's library and faces are closed by their owners' destructors, so nothing is left open
// even when loading stops halfway with an error.
struct Fonts::Impl {
    FT_Library library = nullptr;
    struct Face {
        std::vector<std::uint8_t> data;  // FreeType reads from it while the face is open
        FT_Face face = nullptr;
        std::string family, fullName;  // the full name: family and style ("HelveticaNeueLT Std Medium")
        bool bold = false;
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

    // The face a style draws with, set to its size.
    FT_Face pick(const FontStyle& style, int& size) {
        for (const auto& name : style.faces) {
            const std::string want = lower(name);
            Face* best = nullptr;
            for (auto& f : faces) {
                if ((lower(f->family) == want || lower(f->fullName) == want) && (!best || (f->bold == style.bold && best->bold != style.bold))) {
                    best = f.get();
                }
            }
            if (best) {
                size = style.size;
                // A narrowed style is drawn at a narrower width, hinted at that width.
                FT_Set_Char_Size(best->face, static_cast<FT_F26Dot6>(std::lround(static_cast<float>(style.size) * style.xScale * 64)),
                                 static_cast<FT_F26Dot6>(style.size) * 64, 72, 72);
                return best->face;
            }
        }
        return nullptr;
    }

    void readStyles(const fs::path& ini) {
        std::ifstream in(ini);
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
            s.size = std::max(1, std::atoi(f[1].c_str()));
            if (f.size() > 2) {
                std::stringstream options(lower(f[2]));
                for (std::string o; std::getline(options, o, '|');) {
                    o = trim(o);
                    s.bold = s.bold || o == "bold";
                    s.italic = s.italic || o == "italic";
                    if (o.rfind("linespacing=", 0) == 0) {
                        s.lineSpacing = std::atoi(o.c_str() + 12);
                    }
                    if (o.rfind("xscale=", 0) == 0) {
                        s.xScale = static_cast<float>(std::atof(o.c_str() + 7));
                    }
                }
            }
            if (f.size() > 3) {
                s.guid = static_cast<std::uint32_t>(std::strtoul(f[3].c_str(), nullptr, 16));
            }
            const std::string name = lower(trim(line.substr(0, eq)));
            styles[name] = s;
            byGuid[s.guid] = name;
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
            if (fs::exists(folder / "FontStyle.ini")) {
                impl_->readStyles(folder / "FontStyle.ini");
            }
            for (const auto& item : fs::directory_iterator(folder)) {
                if (lower(utf8(item.path().extension())) != ".mxf") {
                    continue;
                }
                auto face = std::make_unique<Impl::Face>();
                face->data = decodeMxf(readFile(item.path()));
                if (face->data.empty() ||
                    FT_New_Memory_Face(impl_->library, face->data.data(), static_cast<FT_Long>(face->data.size()), 0, &face->face) != 0) {
                    face->face = nullptr;
                    continue;
                }
                face->family = face->face->family_name ? face->face->family_name : "";
                face->fullName = face->family + " " + (face->face->style_name ? face->face->style_name : "");
                face->bold = (face->face->style_flags & FT_STYLE_FLAG_BOLD) != 0;
                impl_->faces.push_back(std::move(face));
            }
        }
    }
    const auto d = impl_->styles.find("default");
    if (d != impl_->styles.end()) {
        impl_->fallback = d->second;
    }
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
    long w = 0;
    for (const char32_t c : codePoints(utf8)) {
        if (FT_Load_Char(face, c, FT_LOAD_DEFAULT) == 0) {
            w += face->glyph->advance.x;
        }
    }
    return static_cast<int>((w + 63) / 64);
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
    if (w <= 0) {
        return out;
    }
    out.baseline = ascent;
    out.pixels.width = static_cast<std::uint32_t>(w + 2);
    out.pixels.height = static_cast<std::uint32_t>(ascent + descent + 1);
    out.pixels.rgba.assign(static_cast<std::size_t>(out.pixels.width) * out.pixels.height * 4, 0);
    for (std::size_t i = 0; i < out.pixels.rgba.size(); i += 4) {
        out.pixels.rgba[i] = out.pixels.rgba[i + 1] = out.pixels.rgba[i + 2] = 255;
    }
    long pen = 0;
    for (const char32_t c : codePoints(utf8)) {
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
