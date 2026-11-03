#include "diag/user.hpp"

#include "diag/report.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cstdlib>
#endif

#include <fstream>
#include <system_error>

namespace ts2::diag {

namespace {

constexpr std::string_view kNameWritten = "%USERNAME%";

// A character that can be part of a folder name, so a name followed by one is only the start of a
// longer name. Bytes and units from 0x80 up count, since they are parts of non-English letters.
bool partOfName(unsigned c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c >= 0x80;
}

unsigned fold(unsigned c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

bool slash(unsigned c) {
    return c == '\\' || c == '/';
}

#ifdef _WIN32
std::string environment(const wchar_t* name) {
    wchar_t value[1024] = {};
    const DWORD n = GetEnvironmentVariableW(name, value, 1024);
    if (n == 0 || n >= 1024) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(n), out.data(), size, nullptr, nullptr);
    return out;
}
#else
std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}
#endif

void addName(User& user, std::string_view name) {
    if (name.size() < 2 || name.find_first_of("\\/") != std::string_view::npos) {
        return;
    }
    for (const auto& known : user.names) {
        bool same = known.size() == name.size();
        for (std::size_t i = 0; same && i < name.size(); ++i) {
            same = fold(static_cast<unsigned char>(known[i])) == fold(static_cast<unsigned char>(name[i]));
        }
        if (same) {
            return;
        }
    }
    user.names.emplace_back(name);
}

// UTF-16 units of UTF-8 text. Empty for text that isn't valid UTF-8.
std::u16string utf16(std::string_view text) {
    std::u16string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size()) {
            return {};
        }
        char32_t point = length == 1 ? c : c & (0x7F >> length);
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next >> 6) != 0x2) {
                return {};
            }
            point = (point << 6) | (next & 0x3F);
        }
        if (point >= 0x10000) {
            point -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (point >> 10));
            out += static_cast<char16_t>(0xDC00 + (point & 0x3FF));
        } else {
            out += static_cast<char16_t>(point);
        }
        i += length;
    }
    return out;
}

// Whether `name` is at `at` in 8-bit text as a whole folder name: right after a slash, and not
// followed by more of a name.
bool nameAt(std::string_view text, std::size_t at, std::string_view name) {
    if (at == 0 || !slash(static_cast<unsigned char>(text[at - 1])) || name.size() > text.size() - at) {
        return false;
    }
    for (std::size_t k = 0; k < name.size(); ++k) {
        if (fold(static_cast<unsigned char>(text[at + k])) != fold(static_cast<unsigned char>(name[k]))) {
            return false;
        }
    }
    return at + name.size() == text.size() || !partOfName(static_cast<unsigned char>(text[at + name.size()]));
}

// The same in UTF-16 (little-endian) units starting at byte `at`.
bool nameAt16(std::span<const std::uint8_t> bytes, std::size_t at, const std::u16string& name) {
    auto unit = [&](std::size_t i) { return static_cast<unsigned>(bytes[i] | bytes[i + 1] << 8); };
    if (at < 2 || name.size() > (bytes.size() - at) / 2 || !slash(unit(at - 2))) {
        return false;
    }
    for (std::size_t k = 0; k < name.size(); ++k) {
        if (fold(unit(at + 2 * k)) != fold(name[k])) {
            return false;
        }
    }
    const std::size_t end = at + 2 * name.size();
    return end + 2 > bytes.size() || !partOfName(unit(end));
}

} // namespace

User thisUser() {
    User user;
#ifdef _WIN32
    user.folder = environment(L"USERPROFILE");
    addName(user, environment(L"USERNAME"));
#else
    user.folder = environment("HOME");
    addName(user, environment("USER"));
#endif
    std::string_view folder = user.folder;
    while (!folder.empty() && slash(static_cast<unsigned char>(folder.back()))) {
        folder.remove_suffix(1);
    }
    if (const std::size_t at = folder.find_last_of("\\/"); at != std::string_view::npos) {
        addName(user, folder.substr(at + 1));
    }
    return user;
}

std::string withoutUser(std::string_view text, const User& user) {
    std::string out = withoutUserFolder(text, user.folder);
    for (const auto& name : user.names) {
        std::string next;
        for (std::size_t i = 0; i < out.size();) {
            if (nameAt(out, i, name)) {
                next += kNameWritten;
                i += name.size();
            } else {
                next += out[i++];
            }
        }
        out = std::move(next);
    }
    return out;
}

bool maskUser(std::span<std::uint8_t> bytes, const User& user) {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    bool changed = false;
    for (const auto& name : user.names) {
        const std::u16string wide = utf16(name);
        for (std::size_t i = 1; i < bytes.size(); ++i) {
            if (nameAt(text, i, name)) {
                std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(i), name.size(), static_cast<std::uint8_t>('_'));
                changed = true;
            } else if (!wide.empty() && nameAt16(bytes, i, wide)) {
                for (std::size_t k = 0; k < wide.size(); ++k) {
                    bytes[i + 2 * k] = '_';
                    bytes[i + 2 * k + 1] = 0;
                }
                changed = true;
            }
        }
    }
    return changed;
}

bool maskUserInFile(const std::filesystem::path& file, const User& user, std::uintmax_t most) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size > most) {
        return false;
    }
    // Opened for reading and writing without truncating, so the file keeps its size whatever happens.
    std::fstream io(file, std::ios::in | std::ios::out | std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!io || !io.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return false;
    }
    if (!maskUser(bytes, user)) {
        return true;
    }
    io.seekp(0);
    io.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(io.flush());
}

} // namespace ts2::diag
