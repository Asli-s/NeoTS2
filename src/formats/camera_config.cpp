#include "formats/camera_config.hpp"

#include <algorithm>
#include <cctype>
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
            configs.push_back({it[1].word, it[2].word});
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
        } else if (key == "orientation" && it.size() >= 2 && !lists.empty()) {
            c.defaultOrientation = std::atoi(it[1].word.c_str());
            c.orientations = *lists[0];
        }
    }
    return configs;
}

float pitchAt(const Config& config, float distance) {
    const auto& l = config.levels;
    if (l.empty()) {
        return 0;
    }
    if (l.size() < 2) {
        return l[0].pitchMin;
    }
    std::size_t a = 0;
    while (a < l.size() && l[a].distance > distance) {
        ++a;
    }
    a = a > 0 ? a - 1 : 0;
    const std::size_t b = std::min(a + 1, l.size() - 1);
    float t = l[a].distance == l[b].distance ? 0 : (l[a].distance - distance) / (l[a].distance - l[b].distance);
    t = std::clamp(t, 0.0f, 1.0f);
    return l[a].pitchMin + (l[b].pitchMin - l[a].pitchMin) * t;
}

} // namespace ts2::camera
