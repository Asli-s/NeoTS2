#include "formats/camera_config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace ts2::camera {

namespace {

// A line's words, with each parenthesised list as one item.
struct Item {
    std::string word;
    std::vector<float> list;
    bool isList = false;
};

std::vector<Item> items(const std::string& line) {
    std::vector<Item> out;
    std::size_t i = 0;
    while (i < line.size()) {
        if (std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        } else if (line[i] == '(') {
            const std::size_t end = line.find(')', i);
            std::istringstream in(line.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
            Item item;
            item.isList = true;
            for (float v; in >> v;) {
                item.list.push_back(v);
            }
            out.push_back(std::move(item));
            i = end == std::string::npos ? line.size() : end + 1;
        } else {
            std::size_t end = i;
            while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])) && line[end] != '(') {
                ++end;
            }
            Item item;
            item.word = line.substr(i, end - i);
            if (item.word.size() >= 2 && item.word.front() == '"' && item.word.back() == '"') {
                item.word = item.word.substr(1, item.word.size() - 2);
            }
            out.push_back(std::move(item));
            i = end;
        }
    }
    return out;
}

// Sets one field of every zoom level from a list, adding levels as needed.
template <class F>
void setLevels(Config& c, const std::vector<float>& values, F field) {
    if (c.levels.size() < values.size()) {
        c.levels.resize(values.size());
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        field(c.levels[i]) = values[i];
    }
}

} // namespace

std::vector<Config> parse(std::string_view text) {
    std::vector<Config> configs;
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) {
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        const auto it = items(line);
        if (it.empty() || it[0].isList) {
            continue;
        }
        const std::string& key = it[0].word;
        if (key == "camera" && it.size() >= 3) {
            Config& c = configs.emplace_back();
            c.name = it[1].word;
            c.controller = it[2].word;
            continue;
        }
        if (configs.empty()) {
            continue;
        }
        Config& c = configs.back();
        // The lists after the key, in order.
        std::vector<const std::vector<float>*> lists;
        for (const auto& item : it) {
            if (item.isList) {
                lists.push_back(&item.list);
            }
        }
        if (key == "zoom" && it.size() >= 2 && !lists.empty()) {
            c.defaultZoom = std::atoi(it[1].word.c_str());
            setLevels(c, *lists[0], [](ZoomLevel& l) -> float& { return l.distance; });
            if (lists.size() > 1) setLevels(c, *lists[1], [](ZoomLevel& l) -> float& { return l.fov; });
            if (lists.size() > 2) setLevels(c, *lists[2], [](ZoomLevel& l) -> float& { return l.nearClip; });
            if (lists.size() > 3) setLevels(c, *lists[3], [](ZoomLevel& l) -> float& { return l.farClip; });
            if (lists.size() > 4) setLevels(c, *lists[4], [](ZoomLevel& l) -> float& { return l.pitchMin; });
            if (lists.size() > 5) setLevels(c, *lists[5], [](ZoomLevel& l) -> float& { return l.pitchMax; });
        } else if (key == "fov" && !lists.empty()) {
            setLevels(c, *lists[0], [](ZoomLevel& l) -> float& { return l.fov; });
        } else if (key == "clip" && lists.size() >= 2) {
            setLevels(c, *lists[0], [](ZoomLevel& l) -> float& { return l.nearClip; });
            setLevels(c, *lists[1], [](ZoomLevel& l) -> float& { return l.farClip; });
        } else if (key == "pitch" && lists.size() >= 2) {
            setLevels(c, *lists[0], [](ZoomLevel& l) -> float& { return l.pitchMin; });
            setLevels(c, *lists[1], [](ZoomLevel& l) -> float& { return l.pitchMax; });
        } else if (key == "setparam" && it.size() >= 3 && !it[1].isList && !it[2].isList) {
            std::string name = it[1].word;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const float value = std::strtof(it[2].word.c_str(), nullptr);
            // The files' values are small numbers. A wilder one, or more names than any camera has, is left out.
            constexpr std::size_t kMostParams = 256;
            if (std::isfinite(value) && std::abs(value) <= 1e6f && (c.params.size() < kMostParams || c.params.contains(name))) {
                c.params[name] = value;
            }
        } else if (key == "clip" && it.size() >= 3 && !it[1].isList && !it[2].isList) {
            // One near and far clip for every zoom level.
            const float nearClip = std::strtof(it[1].word.c_str(), nullptr), farClip = std::strtof(it[2].word.c_str(), nullptr);
            if (std::isfinite(nearClip) && std::isfinite(farClip)) {
                c.clip = {nearClip, farClip};
            }
        } else if (key == "constrain" && it.size() >= 5) {
            // A fraction of the neighborhood from each edge. Values outside 0 to 0.5 would leave no room.
            std::array<float, 4> margins{};
            bool fits = true;
            for (std::size_t k = 0; k < 4; ++k) {
                margins[k] = it[k + 1].isList ? -1 : std::strtof(it[k + 1].word.c_str(), nullptr);
                fits = fits && margins[k] >= 0 && margins[k] <= 0.5f;
            }
            if (fits) {
                c.constrain = margins;
            }
        } else if (key == "orientation" && it.size() >= 2 && !lists.empty()) {
            c.defaultOrientation = std::atoi(it[1].word.c_str());
            c.orientations = *lists[0];
        }
    }
    return configs;
}

namespace {

// The value `field` gives at a distance, blended between the two zoom levels around it by
// distance and held beyond the first and last.
template <typename Field>
float atDistance(const Config& config, float distance, Field field) {
    const auto& l = config.levels;
    if (l.empty()) {
        return 0;
    }
    std::size_t a = 0;
    while (a < l.size() && l[a].distance > distance) {
        ++a;
    }
    a = a > 0 ? a - 1 : 0;
    const std::size_t b = std::min(a + 1, l.size() - 1);
    float t = l[a].distance == l[b].distance ? 0 : (l[a].distance - distance) / (l[a].distance - l[b].distance);
    t = std::clamp(t, 0.0f, 1.0f);
    return field(l[a]) + (field(l[b]) - field(l[a])) * t;
}

} // namespace

float pitchAt(const Config& config, float distance, float blend) {
    const float least = atDistance(config, distance, [](const ZoomLevel& l) { return l.pitchMin; });
    const float most = atDistance(config, distance, [](const ZoomLevel& l) { return l.pitchMax; });
    return least + (most - least) * blend;
}

float nearClipAt(const Config& config, float distance) {
    return atDistance(config, distance, [](const ZoomLevel& l) { return l.nearClip; });
}

} // namespace ts2::camera
