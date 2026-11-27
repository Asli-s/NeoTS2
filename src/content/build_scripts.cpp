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
#include <optional>
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
    // A wall style has at most a few catalog clones. A larger number is a broken script and is left out.
    constexpr std::size_t kMostClones = 16;
    WallStyle* style = nullptr;
    bool hidden = false;  // the style has notInCatalog
    // The building tips of each clone of the entries that follow, by clone.
    std::vector<int> tips;
    const auto clone = [&](std::size_t index) -> WallStyle::CatalogClone* {
        if (style == nullptr || hidden || index >= kMostClones) {
            return nullptr;
        }
        if (style->clones.size() <= index) {
            style->clones.resize(index + 1);
        }
        return &style->clones[index];
    };
    // Resource ids are written in hex and reach past what a 32-bit signed number holds.
    const auto numbers = [](const std::string& value) {
        std::vector<long long> out;
        std::istringstream in(value);
        std::string word;
        while (in >> word && out.size() < kMostClones) {
            out.push_back(std::strtoll(word.c_str(), nullptr, 0));
        }
        return out;
    };
    forEachLine(text, [&](const std::string& key, const std::string& value) {
        if (key == "catalogTextResource") {
            std::istringstream words(value);
            std::string file;
            std::uint32_t table = 0;
            if (words >> file >> table) {
                wallTextFile = file;
                wallTextTable = table;
            }
        } else if (key == "buildTipIndices") {
            tips.clear();
            for (const long long tip : numbers(value)) {
                tips.push_back(static_cast<int>(tip));
            }
        } else if (key == "wall") {
            style = &(wallStyles[static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10))] = WallStyle{});
            hidden = false;
        } else if (style == nullptr) {
            return;
        } else if (key == "thumbnailResource") {
            const auto v = numbers(value);
            if (v.size() == 3 && v[0] >= 0) {
                if (auto* c = clone(static_cast<std::size_t>(v[0]))) {
                    c->thumbnailGroup = static_cast<std::uint32_t>(v[1]);
                    c->thumbnailInstance = static_cast<std::uint32_t>(v[2]);
                }
            }
        } else if (key == "catalogTextIndex" || key == "catalogTextIndices") {
            const auto v = numbers(value);
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (auto* c = clone(i)) {
                    c->textIndex = static_cast<int>(v[i]);
                    c->tip = i < tips.size() ? tips[i] : 0;
                }
            }
        } else if (key == "notInCatalog") {
            style->clones.clear();
            hidden = true;
        } else if (key == "wallThickness") {
            style->thick = value != "none";
        } else if (key == "verticalSpan") {
            const std::string span = lower(value);
            style->belowFloor = span == "floortofloorbasedceiling";
            style->toRoof = span == "floortoroof";
            style->floorToFloor = span == "floortofloor";
            style->span = span == "fixedheight"                ? WallStyle::Span::Fixed
                          : span == "floortoroof"              ? WallStyle::Span::FloorToRoof
                          : span == "floortostairs"            ? WallStyle::Span::FloorToStairs
                          : span == "floortofloorbasedceiling" ? WallStyle::Span::FloorToFloorBasedCeiling
                                                               : WallStyle::Span::FloorToFloor;
        } else if (key == "stageWall") {
            style->floorToFloor = false;
            style->kind = WallStyle::Kind::Stage;
        } else if (key == "invisibleLightBlocking") {
            style->kind = WallStyle::Kind::InvisibleLightBlocking;
        } else if (key == "requiresFlatBottom") {
            style->flatBottom = lower(value) != "false";
        } else if (key == "requiresFlatTop") {
            style->flatTop = lower(value) != "false";
        } else if (key == "mayPlaceDiagonally") {
            style->mayPlaceDiagonally = lower(value) != "false";
        } else if (key == "blocksLocomotion") {
            style->blocksLocomotion = lower(value) != "false";
        } else if (key == "requiresSupportUnderneath") {
            style->supportUnderneath = lower(value) != "false";
        } else if (key == "submersible") {
            style->submersible = lower(value) != "false";
        } else if (key == "blocksPlacement") {
            style->blocksPlacement = lower(value) != "false";
        } else if (key == "mayCutAway") {
            style->cutAway = lower(value) != "false";
        } else if (key == "mayChangeSurface") {
            style->mayChangeSurface = lower(value) != "false";
        } else if (key == "blocksLight") {
            style->blocksLight = lower(value) != "false";
        } else if (key == "bearsLoad") {
            style->bearsLoad = lower(value) != "false";
        } else if (key == "deleteTool") {
            // One tool only. A name the game doesn't know leaves every tool allowed.
            const std::string tool = lower(value);
            style->deleteTools = tool == "partition"       ? WallStyle::kDeletePartition
                                 : tool == "roof"          ? WallStyle::kDeleteRoof
                                 : tool == "levelroom"     ? WallStyle::kDeleteLevelRoom
                                 : tool == "modularstairs" ? WallStyle::kDeleteModularStairs
                                                           : style->deleteTools;
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

void BuildScripts::addPlatforms(std::string_view text) {
    using Kind = Platform::Kind;
    std::optional<std::int32_t> product;
    forEachLine(text, [&](const std::string& key, const std::string& value) {
        if (key == "deck" || key == "foundation" || key == "pool" || key == "stage") {
            product = static_cast<std::int32_t>(std::strtol(value.c_str(), nullptr, 10));
            platforms[*product] = {key == "deck" ? Kind::Deck : key == "foundation" ? Kind::Foundation : key == "pool" ? Kind::Pool : Kind::Stage};
        } else if (key == "catalogTextResource") {
            std::istringstream words(value);
            std::string file;
            std::uint32_t table = 0;
            if (words >> file >> table) {
                platformTextFile = file;
                platformTextTable = table;
            }
        } else if (!product) {
            return;
        } else if (key == "floorPattern") {
            platformFloors[*product] = lower(value);
        } else if (key == "wallStyle") {
            platforms[*product].wallStyle = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "catalogTextIndex" || key == "catalogTextIndices") {
            platforms[*product].textIndex = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        } else if (key == "supportLayout") {
            const std::string layout = lower(value);
            platforms[*product].laysSupports = layout.find("-floodfill") != std::string::npos || layout.find("-edgefill") != std::string::npos ||
                                               layout.find("-corners") != std::string::npos;
        }
    });
}

void BuildScripts::addFireplaces(std::string_view text) {
    std::string file;
    std::uint32_t table = 0;
    int tip = 0;
    Fireplace* fireplace = nullptr;
    forEachLine(text, [&](const std::string& key, const std::string& value) {
        const std::string k = lower(key);
        if (k == "catalogtextresource") {
            std::istringstream words(value);
            std::string name;
            std::uint32_t index = 0;
            if (words >> name >> index) {
                file = name;
                table = index;
            }
        } else if (k == "buildtipindex") {
            tip = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        } else if (k == "fireplace") {
            fireplace = &fireplaces[static_cast<std::int32_t>(std::strtol(value.c_str(), nullptr, 10))];
            *fireplace = {};
            fireplace->textFile = file;
            fireplace->textTable = table;
            fireplace->tip = tip;
        } else if (fireplace == nullptr) {
            return;
        } else if (k == "mantle") {
            fireplace->mantle = value;
        } else if (k == "chimneyhearth") {
            fireplace->chimney = value;
        } else if (k == "chimneytop") {
            fireplace->top = value;
        } else if (k == "chimneytopfp") {
            fireplace->topFootprint = value;
        } else if (k == "chimneyfoundation") {
            fireplace->foundation = value;
        } else if (k == "catalogtextindex") {
            fireplace->textIndex = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
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
        scripts.addPlatforms(read(folder / "deckfoundations.txt"));
        scripts.addFireplaces(read(folder / "fireplaces.txt"));
    }
    return scripts;
}

std::set<std::uint32_t> loadFencesWith(const std::vector<Product>& products, const std::string& property) {
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
    static std::map<std::pair<std::set<std::filesystem::path>, std::string>, std::set<std::uint32_t>> cache;
    const std::lock_guard lock(cacheLock);
    if (const auto it = cache.find({folders, property}); it != cache.end()) {
        return it->second;
    }
    std::set<std::uint32_t> found;
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
                        const auto guid = p.find("guid"), flag = p.find(property);
                        if (guid != p.end() && flag != p.end() && number(flag->second) != 0) {
                            found.insert(static_cast<std::uint32_t>(number(guid->second)));
                        }
                    } catch (const std::exception& ex) {
                        diag::warnOnce(std::string("fences: a fence that can't be read is left out: ") + ex.what());
                    }
                }
            } catch (const std::exception& ex) {
                diag::warnOnce("fences: " + it->path().string() + " can't be read and is passed over: " + ex.what());
            }
        }
    }
    cache.emplace(std::pair{folders, property}, found);
    return found;
}

std::set<std::uint32_t> loadGreenhouseFences(const std::vector<Product>& products) {
    return loadFencesWith(products, "isgreenhouse");
}

} // namespace ts2::content
