#include "formats/propset.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"
#include "parse_float.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
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

std::uint32_t number(const std::map<std::string, std::string>& properties, const std::string& key) {
    const auto it = properties.find(key);
    if (it == properties.end()) {
        return 0;
    }
    std::string_view s = it->second;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    return error == std::errc{} ? value : 0;
}

namespace {

constexpr std::uint16_t kBinaryVersion = 2;

std::optional<std::string> readText(BinaryReader& r) {
    const auto bytes = r.bytes(r.u32());
    std::string text(bytes.begin(), bytes.end());
    if (text.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    return text;
}

void putText(BinaryWriter& w, const std::string& text) {
    if (text.find('\0') != std::string::npos) {
        throw std::runtime_error("property set: a text with a zero byte cannot be written");
    }
    w.fullText(text);
}

} // namespace

std::optional<std::vector<Property>> readBinary(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.u32() != kBinaryMagic || r.u16() != kBinaryVersion) {
            return std::nullopt;
        }
        // A property takes at least 9 bytes: its type, an empty name and a bool.
        std::vector<Property> properties(r.u32Count(9));
        for (Property& p : properties) {
            p.type = r.u32();
            auto name = readText(r);
            if (!name) {
                return std::nullopt;
            }
            p.name = std::move(*name);
            switch (p.type) {
            case kStringType: {
                auto text = readText(r);
                if (!text) {
                    return std::nullopt;
                }
                p.text = std::move(*text);
                break;
            }
            case kStringListType:
                for (std::size_t n = r.u32Count(4); n > 0; --n) {
                    auto text = readText(r);
                    if (!text) {
                        return std::nullopt;
                    }
                    p.texts.push_back(std::move(*text));
                }
                break;
            case kUintType:
            case kIntType:
            case kFloatType:
                p.word = r.u32();
                break;
            case kBoolType:
                p.word = r.u8();
                if (p.word > 1) {
                    return std::nullopt;
                }
                break;
            default:
                return std::nullopt;
            }
        }
        if (r.remaining() != 0) {
            return std::nullopt;
        }
        return properties;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeBinary(const std::vector<Property>& properties) {
    BinaryWriter w;
    w.u32(kBinaryMagic);
    w.u16(kBinaryVersion);
    w.count(properties.size());
    for (const Property& p : properties) {
        w.u32(p.type);
        putText(w, p.name);
        switch (p.type) {
        case kStringType:
            putText(w, p.text);
            break;
        case kStringListType:
            w.count(p.texts.size());
            for (const std::string& text : p.texts) {
                putText(w, text);
            }
            break;
        case kUintType:
        case kIntType:
        case kFloatType:
            w.u32(p.word);
            break;
        case kBoolType:
            w.u8(p.word != 0 ? 1 : 0);
            break;
        default:
            throw std::runtime_error("property set: a type the game does not write");
        }
    }
    return w.take();
}

namespace {

constexpr std::string_view kXmlStart = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<cGZPropertySetString";
constexpr std::string_view kXmlEnd = "\n</cGZPropertySetString>";

std::string_view elementName(std::uint32_t type) {
    switch (type) {
    case kStringType:
        return "AnyString";
    case kStringListType:
        return "AnyStringSet";
    case kUintType:
        return "AnyUint32";
    case kIntType:
        return "AnySint32";
    case kFloatType:
        return "AnyFloat32";
    case kBoolType:
        return "AnyBoolean";
    default:
        return {};
    }
}

void putEscaped(std::string& out, const std::string& text) {
    if (text.find('\0') != std::string::npos) {
        throw std::runtime_error("property set: a text with a zero byte cannot be written");
    }
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "&quot;";
            break;
        case '&':
            out += "&amp;";
            break;
        case '\'':
            out += "&apos;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        default:
            out += c;
        }
    }
}

// The game's float text: a minus sign when negative, "0" below 1 in size, the integer digits, then
// a point and the digits up to the last nonzero one, at most 10 of them after any leading zeros.
std::string floatText(float value) {
    const double magnitude = std::fabs(static_cast<double>(value));
    if (!std::isfinite(value) || magnitude >= 1e7 || (value == 0.0f && std::signbit(value))) {
        throw std::runtime_error("property set: a float the game's digits are not known for");
    }
    std::string out;
    if (value < 0.0f) {
        out += '-';
    }
    if (magnitude < 1.0) {
        out += '0';
    }
    if (value == 0.0f) {
        return out;
    }
    // The first 64 significant digits, as the game takes them.
    char buffer[96];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, magnitude, std::chars_format::scientific, 63);
    const std::string_view text(buffer, result.ptr);
    std::string digits(1, text[0]);
    digits.append(text.substr(2, 63));
    int exponent = 0;
    const std::string_view exponentText = text.substr(text.find('e') + 1);
    std::from_chars(exponentText.data() + (exponentText[0] == '+' ? 1 : 0), exponentText.data() + exponentText.size(), exponent);
    const int point = exponent + 1;
    std::size_t next = 0;
    while (static_cast<int>(next) < point) {
        out += digits[next++];
    }
    const std::size_t lastNonzero = digits.find_last_not_of('0') + 1;
    if (lastNonzero > next) {
        out += '.';
        for (int zero = point; zero < 0; ++zero) {
            out += '0';
        }
        for (int written = 0; written < 10 && next < lastNonzero; ++written) {
            out += digits[next++];
        }
    }
    return out;
}

std::string valueText(const Property& p) {
    switch (p.type) {
    case kStringType:
        return p.text;
    case kUintType:
        return std::to_string(p.word);
    case kIntType:
        return std::to_string(static_cast<std::int32_t>(p.word));
    case kFloatType:
        return floatText(std::bit_cast<float>(p.word));
    default:
        return p.word != 0 ? "true" : "false";
    }
}

// Takes `prefix` off the front of `s`, false when `s` does not start with it.
bool take(std::string_view& s, std::string_view prefix) {
    if (!s.starts_with(prefix)) {
        return false;
    }
    s.remove_prefix(prefix.size());
    return true;
}

// The text up to `end`, taken off the front of `s` with `end` itself.
std::optional<std::string> takeUntil(std::string_view& s, char end) {
    const auto at = s.find(end);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    std::string text = decodeEntities(s.substr(0, at));
    s.remove_prefix(at + 1);
    return text;
}

template <typename T>
bool readNumber(const std::string& text, T& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

bool readValue(Property& p, const std::string& text) {
    switch (p.type) {
    case kStringType:
        p.text = text;
        return true;
    case kUintType:
        return readNumber(text, p.word);
    case kIntType: {
        std::int32_t value = 0;
        if (!readNumber(text, value)) {
            return false;
        }
        p.word = static_cast<std::uint32_t>(value);
        return true;
    }
    case kFloatType: {
        const std::optional<float> value = parseFloat(text);
        if (!value) {
            return false;
        }
        p.word = std::bit_cast<std::uint32_t>(*value);
        return true;
    }
    default:
        p.word = text == "true" ? 1 : 0;
        return text == "true" || text == "false";
    }
}

std::optional<std::vector<Property>> readXmlProperties(std::string_view s) {
    if (!take(s, kXmlStart)) {
        return std::nullopt;
    }
    std::vector<Property> properties;
    if (take(s, "/>")) {
        return properties;
    }
    if (!take(s, ">")) {
        return std::nullopt;
    }
    while (take(s, "\n  <")) {
        Property p;
        auto name = takeUntil(s, ' ');
        for (const std::uint32_t type : {kStringType, kStringListType, kUintType, kIntType, kFloatType, kBoolType}) {
            if (name && elementName(type) == *name) {
                p.type = type;
                name.reset();
                break;
            }
        }
        if (name || !take(s, "key=\"")) {
            return std::nullopt;
        }
        auto key = takeUntil(s, '"');
        // The type text itself is checked when the properties are written back.
        if (!key || !take(s, " type=\"") || !takeUntil(s, '"')) {
            return std::nullopt;
        }
        p.name = std::move(*key);
        if (p.type == kStringListType) {
            if (!take(s, "/>")) {
                if (!take(s, ">")) {
                    return std::nullopt;
                }
                while (take(s, "\n    <Element>")) {
                    auto text = takeUntil(s, '<');
                    if (!text || !take(s, "/Element>")) {
                        return std::nullopt;
                    }
                    p.texts.push_back(std::move(*text));
                }
                if (!take(s, "\n  </AnyStringSet>")) {
                    return std::nullopt;
                }
            }
        } else {
            std::optional<std::string> text;
            if (!take(s, ">") || !(text = takeUntil(s, '<')) || !readValue(p, *text) || !take(s, "/") ||
                !take(s, elementName(p.type)) || !take(s, ">")) {
                return std::nullopt;
            }
        }
        properties.push_back(std::move(p));
    }
    if (!take(s, kXmlEnd)) {
        return std::nullopt;
    }
    return properties;
}

} // namespace

std::optional<std::vector<Property>> readXml(std::span<const std::uint8_t> data) {
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    auto properties = readXmlProperties(text);
    try {
        if (!properties || writeXml(*properties) != std::vector<std::uint8_t>(data.begin(), data.end())) {
            return std::nullopt;
        }
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
    return properties;
}

std::vector<std::uint8_t> writeXml(const std::vector<Property>& properties) {
    std::string out(kXmlStart);
    if (properties.empty()) {
        out += "/>";
        return {out.begin(), out.end()};
    }
    out += '>';
    for (const Property& p : properties) {
        const std::string_view name = elementName(p.type);
        if (name.empty()) {
            throw std::runtime_error("property set: a type the game does not write");
        }
        char type[11];
        std::snprintf(type, sizeof type, "0x%08x", static_cast<unsigned>(p.type));
        out += "\n  <";
        out += name;
        out += " key=\"";
        putEscaped(out, p.name);
        out += "\" type=\"";
        out += type;
        out += '"';
        if (p.type == kStringListType) {
            if (p.texts.empty()) {
                out += "/>";
                continue;
            }
            out += '>';
            for (const std::string& text : p.texts) {
                out += "\n    <Element>";
                putEscaped(out, text);
                out += "</Element>";
            }
            out += "\n  </AnyStringSet>";
        } else {
            out += '>';
            putEscaped(out, valueText(p));
            out += "</";
            out += name;
            out += '>';
        }
    }
    out += kXmlEnd;
    return {out.begin(), out.end()};
}

} // namespace ts2::propset
