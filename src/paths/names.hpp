#pragma once

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ts2::paths {

// A path as UTF-8, for messages and settings. Windows allows names with no UTF-8 form, which give
// a placeholder instead of an exception.
inline std::string utf8Text(const std::filesystem::path& path) {
    try {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    } catch (const std::exception&) {
        return "(a name that can't be shown)";
    }
}

// A path from UTF-8 text. Nullopt for text that isn't valid UTF-8.
inline std::optional<std::filesystem::path> pathFromUtf8(std::string_view text) {
    try {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// A name as Windows compares it: any letter case, without trailing dots and spaces. A name with no
// UTF-8 form gives an empty string, which matches no real name.
inline std::string foldedName(const std::filesystem::path& name) {
    std::string out;
    try {
        const std::u8string text = name.u8string();
        out.assign(text.begin(), text.end());
    } catch (const std::exception&) {
        return {};
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back();
    }
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// True when `inner` is `outer` or lies inside it, as spelled, comparing names as Windows does.
inline bool spelledWithin(const std::filesystem::path& inner, const std::filesystem::path& outer) {
    auto a = inner.begin();
    for (const auto& part : outer) {
        if (part.empty()) {
            continue;  // a trailing separator
        }
        if (a == inner.end() || foldedName(*a) != foldedName(part)) {
            return false;
        }
        ++a;
    }
    return true;
}

} // namespace ts2::paths
