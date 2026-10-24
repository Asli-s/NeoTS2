#include "content/build_scripts.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace ts2::content {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Each line's first word and the rest, without comments, quotes or surrounding space.
template <typename F>
void forEachLine(std::string_view text, F&& f) {
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.resize(hash);
        }
        std::istringstream words(line);
        std::string key;
        if (!(words >> key)) {
            continue;
        }
        std::string rest;
        std::getline(words, rest);
        rest.erase(std::remove(rest.begin(), rest.end(), '"'), rest.end());
        const auto first = rest.find_first_not_of(" \t\r");
        const auto last = rest.find_last_not_of(" \t\r");
        f(key, first == std::string::npos ? std::string() : rest.substr(first, last - first + 1));
    }
}

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

void BuildScripts::addPatterns(std::string_view text, bool walls) {
    auto& patterns = walls ? wallPatterns : floorPatterns;
    const std::string start = walls ? "wallPattern" : "floorPattern";
    std::string current;
    forEachLine(text, [&](const std::string& key, const std::string& value) {
        if (key == start) {
            current = lower(value);
        } else if (key == "material" && !current.empty()) {
            patterns[current] = value;
        }
    });
}

void BuildScripts::addWallStyles(std::string_view text) {
    WallStyle* style = nullptr;
    forEachLine(text, [&](const std::string& key, const std::string& value) {
        if (key == "wall") {
            style = &(wallStyles[static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10))] = WallStyle{});
        } else if (style == nullptr) {
            return;
        } else if (key == "wallThickness") {
            style->thick = value != "none";
        } else if (key == "verticalSpan") {
            style->belowFloor = lower(value) == "floortofloorbasedceiling";
            style->toRoof = lower(value) == "floortoroof";
        } else if (key == "mayCutAway") {
            style->cutAway = lower(value) != "false";
        } else if (key == "blocksLight") {
            style->blocksLight = lower(value) != "false";
        } else if (key == "notRendered") {
            style->rendered = false;
        } else if (key == "defaultPattern") {
            style->defaultPattern = lower(value);
        }
    });
}

BuildScripts loadBuildScripts(const std::vector<Product>& products) {
    BuildScripts scripts;
    std::set<std::filesystem::path> seen;
    for (const auto& objects : objectLoadOrder(products)) {
        const auto folder = objects.parent_path().parent_path() / "Catalog" / "Scripts";
        if (!seen.insert(folder).second) {
            continue;
        }
        scripts.addPatterns(read(folder / "floorPatterns.txt"), false);
        scripts.addPatterns(read(folder / "wallPatterns.txt"), true);
        scripts.addWallStyles(read(folder / "walls.txt"));
    }
    return scripts;
}

} // namespace ts2::content
