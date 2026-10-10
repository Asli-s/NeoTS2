#include "formats/propset.hpp"

#include "formats/binary_reader.hpp"

#include <bit>
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

// The binary form (magic 0xCBE750E0): typed values, written out as the text form shows them.
std::map<std::string, std::string> parseBinary(std::span<const std::uint8_t> data) {
    constexpr std::uint32_t kUint = 0xEB61E4F7, kString = 0x0B8BEA18, kFloat = 0xABC78708, kBool = 0xCBA908E1,
                            kInt = 0x0C264712;
    BinaryReader r(data);
    r.u32();
    r.u16();  // version
    std::map<std::string, std::string> properties;
    const std::uint32_t count = r.u32();
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t type = r.u32();
        const auto nameBytes = r.bytes(r.u32());
        std::string name(nameBytes.begin(), nameBytes.end());
        std::string value;
        if (type == kString) {
            const auto bytes = r.bytes(r.u32());
            value.assign(bytes.begin(), bytes.end());
        } else if (type == kUint) {
            value = std::to_string(r.u32());
        } else if (type == kInt) {
            value = std::to_string(static_cast<std::int32_t>(r.u32()));
        } else if (type == kFloat) {
            value = std::to_string(std::bit_cast<float>(r.u32()));
        } else if (type == kBool) {
            value = r.u8() != 0 ? "true" : "false";
        } else {
            break;  // an unknown type: its size isn't known either
        }
        properties[std::move(name)] = std::move(value);
    }
    return properties;
}

} // namespace

std::map<std::string, std::string> parse(std::span<const std::uint8_t> data) {
    if (data.size() >= 4 && data[0] == 0xE0 && data[1] == 0x50 && data[2] == 0xE7 && data[3] == 0xCB) {
        return parseBinary(data);
    }
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
