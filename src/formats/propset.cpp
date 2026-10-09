#include "formats/propset.hpp"

#include <string_view>
#include <utility>

namespace ts2::propset {

namespace {

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

std::string decodeEntities(std::string_view s) {
    static constexpr std::pair<std::string_view, char> kEntities[] = {
        {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'},
    };
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        bool replaced = false;
        if (s[i] == '&') {
            for (const auto& [entity, c] : kEntities) {
                if (s.substr(i, entity.size()) == entity) {
                    out += c;
                    i += entity.size();
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            out += s[i++];
        }
    }
    return out;
}

} // namespace

std::map<std::string, std::string> parse(std::span<const std::uint8_t> data) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    std::map<std::string, std::string> properties;
    for (std::size_t pos = text.find("<Any"); pos != std::string_view::npos; pos = text.find("<Any", pos)) {
        const auto tagEnd = text.find('>', pos);
        if (tagEnd == std::string_view::npos) {
            break;
        }
        const std::string_view tag = text.substr(pos, tagEnd - pos);
        pos = tagEnd + 1;
        const auto keyStart = tag.find("key=\"");
        if (keyStart == std::string_view::npos) {
            continue;
        }
        const auto keyEnd = tag.find('"', keyStart + 5);
        if (keyEnd == std::string_view::npos) {
            continue;
        }
        std::string key(tag.substr(keyStart + 5, keyEnd - keyStart - 5));
        if (tag.ends_with('/')) {
            properties[std::move(key)] = {};
            continue;
        }
        const auto valueEnd = text.find("</", pos);
        if (valueEnd == std::string_view::npos) {
            break;
        }
        properties[std::move(key)] = decodeEntities(trim(text.substr(pos, valueEnd - pos)));
        pos = valueEnd;
    }
    return properties;
}

} // namespace ts2::propset
