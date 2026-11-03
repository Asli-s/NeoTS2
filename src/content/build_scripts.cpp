#include "content/build_scripts.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/propset.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
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
            style->floorToFloor = lower(value) == "floortofloor";
        } else if (key == "stageWall") {
            style->floorToFloor = false;
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

void BuildScripts::addRoofPatterns(std::string_view text) {
    RoofPatternTextures* pattern = nullptr;
    forEachLine(text, [&](const std::string& key, const std::string& value) {
        if (key == "roofPattern") {
            pattern = &(roofPatterns[static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10))] = RoofPatternTextures{});
        } else if (pattern == nullptr) {
            return;
        } else if (key == "rooftop") {
            pattern->top = value;
        } else if (key == "roofunder") {
            pattern->under = value;
        } else if (key == "roofedges") {
            pattern->edges = value;
        } else if (key == "rooftrim") {
            pattern->trim = value;
        }
    });
}

void BuildScripts::addFenceArches(std::string_view text) {
    forEachLine(text, [&](const std::string& key, const std::string& value) {
        if (key == "fenceArch") {
            fenceArches.insert(static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)));
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
        scripts.addRoofPatterns(read(folder / "roofPatterns.txt"));
        scripts.addFenceArches(read(folder / "fencearches.txt"));
    }
    return scripts;
}

std::set<std::uint32_t> loadGreenhouseFences(const std::vector<Product>& products) {
    constexpr std::uint32_t kFenceProperties = 0x2CB230B8;
    // A property's number, decimal or 0x hex, or a binary flag's "true".
    const auto number = [](const std::string& text) -> unsigned long long {
        if (text == "true") {
            return 1;
        }
        return std::strtoull(text.c_str(), nullptr, text.starts_with("0x") ? 16 : 10);
    };
    std::set<std::filesystem::path> folders;
    for (const auto& objects : objectLoadOrder(products)) {
        folders.insert(objects.parent_path().parent_path() / "Catalog" / "Patterns");
    }
    // The catalog doesn't change while the game runs, so every lot after the first gets the same set.
    static std::mutex cacheLock;
    static std::map<std::set<std::filesystem::path>, std::set<std::uint32_t>> cache;
    const std::lock_guard lock(cacheLock);
    if (const auto it = cache.find(folders); it != cache.end()) {
        return it->second;
    }
    std::set<std::uint32_t> greenhouse;
    for (const auto& folder : folders) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
            if (it->path().extension() != ".package") {
                continue;
            }
            try {
                dbpf::Package package{it->path()};
                for (const auto& e : package.entries()) {
                    if (e.key.type != kFenceProperties) {
                        continue;
                    }
                    try {
                        const auto p = propset::parse(package.read(e));
                        const auto guid = p.find("guid"), flag = p.find("isgreenhouse");
                        if (guid != p.end() && flag != p.end() && number(flag->second) != 0) {
                            greenhouse.insert(static_cast<std::uint32_t>(number(guid->second)));
                        }
                    } catch (const std::exception& ex) {
                        diag::warnOnce(std::string("greenhouse walls: a fence that can't be read is left out: ") + ex.what());
                    }
                }
            } catch (const std::exception& ex) {
                diag::warnOnce("greenhouse walls: " + it->path().string() + " can't be read and is passed over: " + ex.what());
            }
        }
    }
    cache.emplace(folders, greenhouse);
    return greenhouse;
}

} // namespace ts2::content
