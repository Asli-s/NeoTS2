#include "audio/custom_stations.hpp"

#include <cstdio>
#include <cstdlib>

namespace ts2::audio {

namespace {

// How many stations and songs are read, so a huge file cannot fill the memory.
constexpr std::size_t kMaxStations = 1024, kMaxSongs = 65536;

struct Tag {
    std::string name;
    bool closing = false, selfClosing = false;
    std::vector<std::pair<std::string, std::string>> attributes;
};

std::string unescape(std::string_view text) {
    static constexpr std::pair<std::string_view, char> kEntities[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        bool named = false;
        for (const auto& [entity, character] : kEntities) {
            if (text.substr(i).starts_with(entity)) {
                out += character;
                i += entity.size();
                named = true;
                break;
            }
        }
        if (!named) {
            out += text[i++];
        }
    }
    return out;
}

std::string escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// The next tag from `at`, skipping text, comments and the xml declaration. False at the end.
bool nextTag(std::string_view xml, std::size_t& at, Tag& tag) {
    while (true) {
        at = xml.find('<', at);
        if (at == std::string_view::npos) {
            return false;
        }
        if (xml.substr(at).starts_with("<!--")) {
            const std::size_t end = xml.find("-->", at);
            at = end == std::string_view::npos ? xml.size() : end + 3;
        } else if (xml.substr(at).starts_with("<?")) {
            const std::size_t end = xml.find("?>", at);
            at = end == std::string_view::npos ? xml.size() : end + 2;
        } else {
            break;
        }
    }
    const std::size_t end = xml.find('>', at);
    if (end == std::string_view::npos) {
        return false;
    }
    std::string_view body = xml.substr(at + 1, end - at - 1);
    at = end + 1;
    tag = {};
    if (body.starts_with('/')) {
        tag.closing = true;
        body.remove_prefix(1);
    }
    if (body.ends_with('/')) {
        tag.selfClosing = true;
        body.remove_suffix(1);
    }
    std::size_t i = body.find_first_of(" \t\r\n");
    tag.name = std::string(body.substr(0, i));
    while (i != std::string_view::npos && i < body.size()) {
        i = body.find_first_not_of(" \t\r\n", i);
        const std::size_t equals = i == std::string_view::npos ? i : body.find('=', i);
        if (equals == std::string_view::npos) {
            break;
        }
        std::string_view key = body.substr(i, equals - i);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.remove_suffix(1);
        }
        const std::size_t open = body.find_first_of("\"'", equals);
        const std::size_t close = open == std::string_view::npos ? open : body.find(body[open], open + 1);
        if (close == std::string_view::npos) {
            break;
        }
        tag.attributes.emplace_back(std::string(key), unescape(body.substr(open + 1, close - open - 1)));
        i = close + 1;
    }
    return true;
}

std::optional<std::uint32_t> number(const Tag& tag, std::string_view key) {
    for (const auto& [name, value] : tag.attributes) {
        if (name == key && !value.empty()) {
            char* end = nullptr;
            const unsigned long long n = std::strtoull(value.c_str(), &end, 0);
            if (*end == '\0' && n <= 0xFFFFFFFFull) {
                return static_cast<std::uint32_t>(n);
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::vector<CustomStation>> parseCustomStations(std::string_view xml) {
    std::size_t at = 0;
    Tag tag;
    if (!nextTag(xml, at, tag) || tag.name != "CustomStations" || tag.closing) {
        return std::nullopt;
    }
    std::vector<CustomStation> out;
    std::size_t songs = 0;
    while (nextTag(xml, at, tag)) {
        if (tag.name == "Station" && !tag.closing && out.size() < kMaxStations) {
            CustomStation station;
            for (const auto& [name, value] : tag.attributes) {
                if (name == "Name") {
                    station.name = value;
                }
            }
            out.push_back(std::move(station));
        } else if (tag.name == "Song" && !tag.closing && !out.empty() && songs < kMaxSongs) {
            const auto id1 = number(tag, "ID1"), id2 = number(tag, "ID2");
            if (id1 && id2) {
                out.back().songs.push_back({*id1, *id2});
                ++songs;
            }
        }
    }
    return out;
}

std::string writeCustomStations(const std::vector<CustomStation>& stations) {
    std::string out = "<CustomStations>\n";
    for (const CustomStation& station : stations) {
        if (station.songs.empty()) {
            continue;
        }
        out += "  <Station Name=\"" + escape(station.name) + "\">\n    <Songs>\n";
        for (const CustomSong& song : station.songs) {
            char line[64];
            std::snprintf(line, sizeof line, "      <Song ID1=\"0x%08x\" ID2=\"0x%08x\"/>\n", song.id1, song.id2);
            out += line;
        }
        out += "    </Songs>\n  </Station>\n";
    }
    return out + "</CustomStations>\n";
}

} // namespace ts2::audio
