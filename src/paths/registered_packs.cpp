#include "paths/names.hpp"
#include "paths/pack_records.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>

namespace ts2::paths {

namespace fs = std::filesystem;

namespace {

constexpr const char* kAppPaths = "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
// The game reads at most 32 packs.
constexpr std::size_t kMaxPacks = 32;
// system.reg is a few megabytes, so a far larger file isn't read.
constexpr std::uintmax_t kMaxRegistryFile = 64u << 20;
// The game reads at most 260 bytes of a path.
constexpr std::size_t kMaxValue = 260;

std::string lowerAscii(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

void appendUtf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | code >> 6);
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | code >> 12);
        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | code >> 18);
        out += static_cast<char>(0x80 | (code >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (code >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// A quoted registry string from just after its opening quote, with its escapes undone (\x is a
// UTF-16 unit). Moves `at` past the closing quote. Nullopt when it never closes.
std::optional<std::string> quoted(std::string_view line, std::size_t& at) {
    std::string out;
    std::uint32_t highSurrogate = 0;
    while (at < line.size()) {
        const char c = line[at++];
        if (c == '"') {
            return out;
        }
        if (c != '\\' || at >= line.size()) {
            out += c;
            continue;
        }
        const char e = line[at++];
        if (e == 'x') {
            std::uint32_t unit = 0;
            int digits = 0;
            while (digits < 4 && at < line.size() && std::isxdigit(static_cast<unsigned char>(line[at]))) {
                const char h = static_cast<char>(std::tolower(static_cast<unsigned char>(line[at++])));
                unit = unit << 4 | static_cast<std::uint32_t>(h <= '9' ? h - '0' : h - 'a' + 10);
                ++digits;
            }
            if (unit >= 0xD800 && unit < 0xDC00) {
                highSurrogate = unit;
                continue;
            }
            if (unit >= 0xDC00 && unit < 0xE000) {
                if (highSurrogate != 0) {
                    appendUtf8(out, 0x10000 + ((highSurrogate - 0xD800) << 10) + (unit - 0xDC00));
                }
                highSurrogate = 0;
                continue;
            }
            appendUtf8(out, unit);
        } else {
            out += e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e == '0' ? '\0' : e;
        }
        highSurrogate = 0;
    }
    return std::nullopt;
}

class WineRegistry {
public:
    explicit WineRegistry(std::string_view text) : text_(text) {
        std::size_t at = 0;
        while (at < text_.size()) {
            const std::size_t end = std::min(text_.find('\n', at), text_.size());
            const std::string_view line = text_.substr(at, end - at);
            if (line.starts_with('[')) {
                if (const std::size_t close = line.find(']'); close != std::string_view::npos) {
                    // Key names double their backslashes.
                    std::string name;
                    for (std::size_t i = 1; i < close; ++i) {
                        name += line[i];
                        if (line[i] == '\\' && i + 1 < close && line[i + 1] == '\\') {
                            ++i;
                        }
                    }
                    keys_.try_emplace(lowerAscii(std::move(name)), end + 1);
                }
            }
            at = end + 1;
        }
    }

    // The text after the value's '='. Wow6432Node comes first, as for a 32-bit program.
    std::optional<std::string> raw(const std::string& key, const char* name) const {
        const std::string folded = lowerAscii(key);
        constexpr std::string_view kSoftware = "software\\";
        if (folded.starts_with(kSoftware)) {
            if (auto value = rawIn("software\\wow6432node\\" + folded.substr(kSoftware.size()), name)) {
                return value;
            }
        }
        return rawIn(folded, name);
    }

private:
    std::optional<std::string> rawIn(const std::string& key, const char* name) const {
        const auto found = keys_.find(key);
        if (found == keys_.end()) {
            return std::nullopt;
        }
        const std::string wanted = lowerAscii(name);
        std::size_t at = found->second;
        while (at < text_.size()) {
            const std::size_t end = std::min(text_.find('\n', at), text_.size());
            std::string_view line = text_.substr(at, end - at);
            at = end + 1;
            if (line.starts_with('[')) {
                break;
            }
            if (!line.starts_with('"')) {
                continue;
            }
            std::size_t pos = 1;
            const auto valueName = quoted(line, pos);
            if (!valueName || pos >= line.size() || line[pos] != '=' || lowerAscii(*valueName) != wanted) {
                continue;
            }
            std::string value(line.substr(pos + 1));
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
                value.pop_back();
            }
            return value;
        }
        return std::nullopt;
    }

    std::string_view text_;
    std::unordered_map<std::string, std::size_t> keys_;  // folded key name -> offset of its values
};

// Drive C: is the prefix's drive_c, other drives its dosdevices links.
fs::path prefixPath(const std::string& text, const fs::path& prefix) {
    if (text.size() < 3 || !std::isalpha(static_cast<unsigned char>(text[0])) || text[1] != ':' || text[2] != '\\') {
        return {};
    }
    const char drive = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
    fs::path out = drive == 'c' ? prefix / "drive_c" : prefix / "dosdevices" / std::string{drive, ':'};
    std::size_t start = 3;
    while (start < text.size()) {
        const std::size_t slash = std::min(text.find('\\', start), text.size());
        if (slash > start) {
            const auto part = pathFromUtf8(std::string_view(text).substr(start, slash - start));
            if (!part) {
                return {};
            }
            out /= *part;
        }
        start = slash + 1;
    }
    return out;
}

} // namespace

std::vector<RegisteredPack> readPackRecords(const RegistryText& text, const RegistryNumber& number, const RegistryPath& toPath) {
    std::vector<RegisteredPack> packs;
    const std::string baseKey = std::string(kAppPaths) + "Sims2.exe";
    if (const auto base = text(baseKey, "Path")) {
        if (fs::path folder = toPath(*base); !folder.empty()) {
            packs.push_back({std::move(folder), 0});
        }
    }
    const auto gameKey = text(baseKey, "Game Registry");
    const auto list = gameKey ? text(*gameKey, "EPsInstalled") : std::nullopt;
    if (!list) {
        return packs;
    }
    std::size_t start = 0;
    for (std::size_t n = 0; n < kMaxPacks && start <= list->size(); ++n) {
        const std::size_t comma = std::min(list->find(',', start), list->size());
        const std::string exe = list->substr(start, comma - start);
        start = comma + 1;
        if (exe.empty() || exe.find_first_of("\\/") != std::string::npos) {
            continue;
        }
        const std::string key = kAppPaths + exe;
        const auto installed = number(key, "Installed");
        const auto path = text(key, "Path");
        if (installed && *installed != 0 && path) {
            if (fs::path folder = toPath(*path); !folder.empty()) {
                packs.push_back({std::move(folder), static_cast<int>(n) + 1});
            }
        }
    }
    return packs;
}

std::vector<RegisteredPack> wineRegisteredPacks(std::string_view systemReg, const fs::path& prefix) {
    const WineRegistry registry(systemReg);
    const auto text = [&](const std::string& key, const char* name) -> std::optional<std::string> {
        const auto raw = registry.raw(key, name);
        std::size_t at = 1;
        if (!raw || !raw->starts_with('"')) {
            return std::nullopt;
        }
        auto value = quoted(*raw, at);
        if (value && value->size() > kMaxValue) {
            value->resize(kMaxValue);
        }
        return value;
    };
    const auto number = [&](const std::string& key, const char* name) -> std::optional<std::uint32_t> {
        const auto raw = registry.raw(key, name);
        constexpr std::string_view kDword = "dword:";
        if (!raw || !raw->starts_with(kDword) || raw->size() != kDword.size() + 8) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::size_t i = kDword.size(); i < raw->size(); ++i) {
            const char h = static_cast<char>(std::tolower(static_cast<unsigned char>((*raw)[i])));
            if (!std::isxdigit(static_cast<unsigned char>(h))) {
                return std::nullopt;
            }
            value = value << 4 | static_cast<std::uint32_t>(h <= '9' ? h - '0' : h - 'a' + 10);
        }
        return value;
    };
    return readPackRecords(text, number, [&](const std::string& path) { return prefixPath(path, prefix); });
}

bool sameFolder(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    const auto resolved = [](const fs::path& path) {
        std::error_code error;
        fs::path out = fs::weakly_canonical(path, error);
        return error || out.empty() ? path.lexically_normal() : out;
    };
    const fs::path left = resolved(a);
    const fs::path right = resolved(b);
    return spelledWithin(left, right) && spelledWithin(right, left);
}

#ifndef _WIN32
std::vector<RegisteredPack> registeredPacks(const fs::path& installRoot) {
    std::error_code error;
    for (fs::path folder = fs::weakly_canonical(installRoot, error); folder.has_relative_path(); folder = folder.parent_path()) {
        if (foldedName(folder.filename()) != "drive_c") {
            continue;
        }
        const fs::path prefix = folder.parent_path();
        const fs::path file = prefix / "system.reg";
        const std::uintmax_t size = fs::file_size(file, error);
        if (error || size > kMaxRegistryFile) {
            return {};
        }
        std::string text(static_cast<std::size_t>(size), '\0');
        std::ifstream in(file, std::ios::binary);
        if (!in.read(text.data(), static_cast<std::streamsize>(text.size()))) {
            return {};
        }
        return wineRegisteredPacks(text, prefix);
    }
    return {};
}
#endif

} // namespace ts2::paths
