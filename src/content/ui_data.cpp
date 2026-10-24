#include "content/ui_data.hpp"

#include "content/install.hpp"
#include "formats/hash.hpp"
#include "formats/image.hpp"
#include "formats/resource_types.hpp"
#include "formats/str.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace ts2::content {

namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// A product's TSData/Res folder, above its objects package.
std::optional<fs::path> resFolder(const fs::path& objects) {
    for (fs::path p = objects.parent_path(); p.has_relative_path(); p = p.parent_path()) {
        if (lower(utf8(p.filename())) == "res") {
            return p;
        }
    }
    return std::nullopt;
}

// Most strings are UTF-8, but some keep a Windows-1252 byte (0x99 for the trademark sign).
std::string asUtf8(const std::string& s) {
    bool valid = true;
    for (std::size_t i = 0; i < s.size() && valid;) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || i + n > s.size()) {
            valid = false;
            break;
        }
        for (std::size_t k = 1; k < n; ++k) {
            valid = valid && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        }
        i += n;
    }
    if (valid) {
        return s;
    }
    static constexpr char32_t k1252[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160,
                                           0x2039, 0x152, 0x8D, 0x17D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                           0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        const char32_t cp = c >= 0x80 && c < 0xA0 ? k1252[c - 0x80] : c;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

} // namespace

UiData::UiData(const fs::path& install) {
    std::set<fs::path> seen;
    for (const auto& product : findProducts(install)) {
        for (const auto& objects : product.objectPackages) {
            const auto res = resFolder(objects);
            if (!res || !seen.insert(*res).second) {
                continue;
            }
            const fs::path ui = *res / "UI" / "ui.package";
            if (fs::exists(ui)) {
                index_.addPackage(ui);
            }
            const fs::path text = *res / "Text";
            if (!fs::is_directory(text)) {
                continue;
            }
            std::vector<fs::path> packages;
            for (const auto& item : fs::directory_iterator(text)) {
                if (item.is_regular_file() && lower(utf8(item.path().extension())) == ".package") {
                    packages.push_back(item.path());
                }
            }
            std::sort(packages.begin(), packages.end());
            for (const auto& p : packages) {
                index_.addPackage(p, hash::groupId(utf8(p.stem())));
            }
        }
    }
}

std::optional<std::vector<ui::Window>> UiData::script(std::uint32_t group, std::uint32_t instance) {
    const auto* r = index_.find({types::kUiScript, group, instance, 0});
    if (!r) {
        return std::nullopt;
    }
    const auto data = index_.read(*r);
    return ui::parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

std::optional<tga::Pixels> UiData::image(std::uint32_t group, std::uint32_t instance) {
    const auto* r = index_.find({types::kImage, group, instance, 0});
    if (!r) {
        return std::nullopt;
    }
    return image::decode(index_.read(*r));
}

std::string UiData::text(std::uint32_t group, std::uint32_t value, std::uint8_t language) {
    const auto* r = index_.find({types::kStringTable, group, value >> 16, 0});
    const std::uint32_t n = value & 0xFFFF;
    if (!r || n == 0) {
        return {};
    }
    // A script's captions mostly come from a few tables: each is read once.
    const std::uint64_t key = static_cast<std::uint64_t>(group) << 32 | (value >> 16);
    auto known = tables_.find(key);
    if (known == tables_.end()) {
        known = tables_.emplace(key, str::parse(index_.read(*r))).first;
    }
    const auto& table = known->second;
    auto entries = table.forLanguage(language);
    if (entries.empty()) {
        entries = table.forLanguage(1);
    }
    return n <= entries.size() ? asUtf8(entries[n - 1]->value) : std::string();
}

} // namespace ts2::content
