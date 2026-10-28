#include "content/user_props.hpp"

#include "diag/log.hpp"

#include <fstream>
#include <system_error>

namespace ts2::content {

namespace {

// The game's file holds about 50 settings in 8 KB. These limits keep a broken or tampered file from
// taking much memory.
constexpr std::uintmax_t kMaxBytes = 1 << 20;
constexpr std::size_t kMaxSettings = 4096;
constexpr std::size_t kMaxText = 1024;

} // namespace

std::optional<bool> UserProps::flag(std::string_view key) const {
    const auto it = values.find(std::string(key));
    if (it == values.end()) {
        return std::nullopt;
    }
    if (it->second == "true") {
        return true;
    }
    if (it->second == "false") {
        return false;
    }
    return std::nullopt;
}

UserProps parseUserProps(std::string_view xml) {
    // Each setting is one element, <AnyBoolean key="name" type="0x...">value</AnyBoolean>. Lists
    // hold <Element> children instead of a value and are skipped.
    constexpr std::string_view kKey = " key=\"";
    UserProps props;
    std::size_t at = 0;
    while (props.values.size() < kMaxSettings) {
        const std::size_t open = xml.find("<Any", at);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t end = xml.find('>', open);
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1;
        const std::string_view tag = xml.substr(open, end - open);
        const std::size_t keyAt = tag.find(kKey);
        if (keyAt == std::string_view::npos || tag.back() == '/') {
            continue;
        }
        const std::size_t keyStart = keyAt + kKey.size();
        const std::size_t keyEnd = tag.find('"', keyStart);
        const std::size_t close = xml.find('<', at);
        if (keyEnd == std::string_view::npos || close == std::string_view::npos || xml.substr(close, 2) != "</") {
            continue;
        }
        const std::string_view key = tag.substr(keyStart, keyEnd - keyStart);
        const std::string_view value = xml.substr(at, close - at);
        if (!key.empty() && key.size() <= kMaxText && value.size() <= kMaxText) {
            props.values.insert_or_assign(std::string(key), std::string(value));
        }
        at = close;
    }
    return props;
}

UserProps readUserProps(const std::filesystem::path& userFolder) {
    const std::filesystem::path file = userFolder / "Config" / "userProps.xml";
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    if (error) {
        return {};
    }
    if (size > kMaxBytes) {
        diag::warnOnce("user options: userProps.xml is larger than 1 MB and is left out");
        return {};
    }
    std::ifstream in(file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        return {};
    }
    return parseUserProps(text);
}

} // namespace ts2::content
