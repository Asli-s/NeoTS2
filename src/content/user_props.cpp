#include "content/user_props.hpp"

#include "content/install.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <fstream>
#include <iterator>
#include <system_error>

namespace ts2::content {

namespace {

// The game's file holds about 50 settings in 8 KB. These limits keep a broken or tampered file from
// taking much memory.
constexpr std::uintmax_t kMaxBytes = 1 << 20;
constexpr std::size_t kMaxSettings = 4096;
constexpr std::size_t kMaxText = 1024;
// The most buttons a settings group has in the game's panels is 5. More than this is a broken caption.
constexpr std::size_t kMaxOptionValues = 16;

// The element names and type ids the game writes for true or false settings and for numbers.
constexpr std::string_view kBoolElement = "AnyBoolean", kBoolType = "0xcba908e1";
constexpr std::string_view kNumberElement = "AnyUint32", kNumberType = "0xeb61e4f7";
constexpr std::string_view kClosingTag = "</cGZPropertySetString>";

bool plainWord(std::string_view text) {
    return !text.empty() && text.size() <= kMaxText &&
           std::all_of(text.begin(), text.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

bool sameWord(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::optional<long long> number(std::string_view text) {
    long long value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// Where a setting's value lies in the file's text: from past its opening tag to its closing tag.
std::optional<std::pair<std::size_t, std::size_t>> findSetting(std::string_view xml, std::string_view key) {
    const std::string needle = " key=\"" + std::string(key) + "\"";
    for (std::size_t at = xml.find(needle); at != std::string_view::npos; at = xml.find(needle, at + 1)) {
        const std::size_t open = xml.rfind('<', at);
        const std::size_t end = xml.find('>', at);
        if (open == std::string_view::npos || end == std::string_view::npos || xml.substr(open, 4) != "<Any" || xml[end - 1] == '/') {
            continue;
        }
        const std::size_t close = xml.find("</", end);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        return std::pair{end + 1, close};
    }
    return std::nullopt;
}

} // namespace

std::string OptionControl::savedKey() const { return kind == Kind::RadioOption ? "option" + key : key; }

std::string OptionControl::savedValue(std::size_t button) const {
    if (button < 1 || button > values.size()) {
        return {};
    }
    const std::string& value = values[button - 1];
    if (kind == Kind::Radio && isBool) {
        return value == "0" ? "false" : "true";
    }
    return value;
}

std::optional<std::size_t> OptionControl::buttonFor(std::string_view saved) const {
    for (std::size_t button = 1; button <= values.size(); ++button) {
        if (kind == Kind::Radio && isBool) {
            if (saved == savedValue(button)) {
                return button;
            }
        } else if (const auto a = number(saved), b = number(values[button - 1]); a && b && *a == *b) {
            return button;
        }
    }
    return std::nullopt;
}

std::optional<OptionControl> parseOptionCaption(std::string_view caption) {
    std::vector<std::string_view> parts;
    while (parts.size() <= kMaxOptionValues + 3) {
        const std::size_t colon = caption.find(':');
        parts.push_back(caption.substr(0, colon));
        if (colon == std::string_view::npos) {
            break;
        }
        caption.remove_prefix(colon + 1);
    }
    if (parts.size() < 4 || parts.size() > kMaxOptionValues + 3) {
        return std::nullopt;
    }
    OptionControl control;
    // The game compares the kind and type without minding case. The panels write "radioOption".
    if (sameWord(parts[0], "radio")) {
        control.kind = OptionControl::Kind::Radio;
    } else if (sameWord(parts[0], "radiooption")) {
        control.kind = OptionControl::Kind::RadioOption;
    } else if (sameWord(parts[0], "slider")) {
        control.kind = OptionControl::Kind::Slider;
    } else {
        return std::nullopt;
    }
    if (sameWord(parts[2], "bool")) {
        control.isBool = true;
    } else if (!sameWord(parts[2], "int")) {
        return std::nullopt;
    }
    if (!plainWord(parts[1])) {
        return std::nullopt;
    }
    control.key = parts[1];
    for (std::size_t i = 3; i < parts.size(); ++i) {
        if (!plainWord(parts[i]) || !number(parts[i])) {
            return std::nullopt;
        }
        control.values.emplace_back(parts[i]);
    }
    return control;
}

bool setUserProp(std::string& xml, std::string_view key, std::string_view value, bool isBool) {
    if (!plainWord(key) || !plainWord(value)) {
        return false;
    }
    if (const auto place = findSetting(xml, key)) {
        xml.replace(place->first, place->second - place->first, value);
        return true;
    }
    const std::size_t close = xml.rfind(kClosingTag);
    if (close == std::string::npos) {
        return false;
    }
    // Written as the game writes its lines: two spaces in, ending as the file's other lines end.
    const std::string element(isBool ? kBoolElement : kNumberElement);
    const std::string type(isBool ? kBoolType : kNumberType);
    const std::string lineEnd = xml.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    xml.insert(close, "  <" + element + " key=\"" + std::string(key) + "\" type=\"" + type + "\">" + std::string(value) + "</" +
                          element + ">" + lineEnd);
    return true;
}

namespace {

bool savePropsFile(const std::filesystem::path& userFolder, const std::string& name, const std::map<std::string, std::pair<std::string, bool>>& settings) {
    const std::filesystem::path folder = userFolder / "Config";
    const std::filesystem::path file = folder / name;
    std::error_code error;
    std::string text;
    if (std::filesystem::exists(file, error)) {
        const std::uintmax_t size = std::filesystem::file_size(file, error);
        if (error || size > kMaxBytes) {
            diag::warnOnce("user options: userProps.xml can't be read, so the options are not saved");
            return false;
        }
        std::ifstream in(file, std::ios::binary);
        text.resize(static_cast<std::size_t>(size));
        if (!in.read(text.data(), static_cast<std::streamsize>(text.size()))) {
            return false;
        }
    } else {
        // A user folder the game hasn't started with yet gets the file's frame as the game writes it.
        std::filesystem::create_directories(folder, error);
        text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<cGZPropertySetString>\r\n</cGZPropertySetString>\r\n";
    }
    for (const auto& [key, setting] : settings) {
        if (!setUserProp(text, key, setting.first, setting.second)) {
            diag::warnOnce("user options: userProps.xml has no settings list, so the options are not saved");
            return false;
        }
    }
    const std::filesystem::path written = folder / (name + ".neots2-new");
    {
        std::ofstream out(written, std::ios::binary | std::ios::trunc);
        if (!out.write(text.data(), static_cast<std::streamsize>(text.size())) || !out.flush()) {
            return false;
        }
    }
    std::filesystem::rename(written, file, error);
    if (error) {
        std::filesystem::remove(written, error);
        diag::warnOnce("user options: userProps.xml can't be replaced, so the options are not saved");
        return false;
    }
    return true;
}

} // namespace

bool saveUserProps(const std::filesystem::path& userFolder, const std::map<std::string, std::pair<std::string, bool>>& settings) {
    return savePropsFile(userFolder, "userProps.xml", settings);
}

bool saveNeoProps(const std::filesystem::path& userFolder, const std::map<std::string, std::pair<std::string, bool>>& settings) {
    return savePropsFile(userFolder, "neots2Props.xml", settings);
}

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

UserProps readPropsFile(const std::filesystem::path& file) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    if (error) {
        return {};
    }
    if (size > kMaxBytes) {
        diag::warnOnce("user options: " + file.filename().string() + " is larger than 1 MB and is left out");
        return {};
    }
    std::ifstream in(file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        return {};
    }
    return parseUserProps(text);
}

UserProps readNeoProps(const std::filesystem::path& userFolder) {
    return readPropsFile(userFolder / "Config" / "neots2Props.xml");
}

UserProps readUserProps(const std::filesystem::path& userFolder) { return readPropsFile(userFolder / "Config" / "userProps.xml"); }

namespace {

// A Graphics Rules line's words, split at spaces and tabs. Text after # is a comment.
std::vector<std::string_view> words(std::string_view line) {
    if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
        line = line.substr(0, hash);
    }
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < line.size() && out.size() < 8) {
        const std::size_t start = line.find_first_not_of(" \t\r", at);
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t end = line.find_first_of(" \t\r", start);
        out.push_back(line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        at = end == std::string_view::npos ? line.size() : end;
    }
    return out;
}

} // namespace

std::map<std::string, std::string> graphicsOptionProps(std::string_view rules, std::string_view option, int setting) {
    std::map<std::string, std::string> variables;
    std::map<std::string, std::string> props;
    bool inOption = false, inSetting = false;
    std::size_t at = 0;
    while (at < rules.size()) {
        const std::size_t end = rules.find('\n', at);
        const auto w = words(rules.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        at = end == std::string_view::npos ? rules.size() : end + 1;
        if (w.size() >= 3 && w[0] == "seti") {
            variables.insert_or_assign(std::string(w[1]), std::string(w[2]));
        } else if (w.size() >= 2 && w[0] == "option") {
            inOption = w[1] == option;
            inSetting = false;
        } else if (!w.empty() && w[0] == "end") {
            if (inOption) {
                break;
            }
        } else if (inOption && w.size() >= 2 && w[0] == "setting") {
            std::string_view name = w[1];
            std::string value(name);
            if (name.starts_with('$')) {
                const auto it = variables.find(std::string(name.substr(1)));
                value = it == variables.end() ? std::string() : it->second;
            }
            inSetting = number(value) == setting;
        } else if (inOption && inSetting && w.size() >= 3 && w[0].ends_with("Prop")) {
            props.insert_or_assign(std::string(w[1]), std::string(w[2]));
        }
    }
    return props;
}

std::string readGraphicsRules(const std::filesystem::path& install) {
    const auto file = newestDataFile(findProducts(install), "Res/Config/Graphics Rules.sgr");
    std::error_code error;
    if (!file || std::filesystem::file_size(*file, error) > kMaxBytes || error) {
        return {};
    }
    std::ifstream in(*file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::optional<std::string> savedOptionProp(const UserProps& saved, std::string_view rules, std::string_view option, std::string_view prop) {
    const auto it = saved.values.find("option" + std::string(option));
    if (it == saved.values.end()) {
        return std::nullopt;
    }
    const auto setting = number(it->second);
    if (!setting || *setting < INT_MIN || *setting > INT_MAX) {
        return std::nullopt;
    }
    const auto props = graphicsOptionProps(rules, option, static_cast<int>(*setting));
    const auto value = props.find(std::string(prop));
    return value == props.end() ? std::nullopt : std::optional(value->second);
}

} // namespace ts2::content
