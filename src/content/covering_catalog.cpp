#include "content/covering_catalog.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/propset.hpp"
#include "formats/resource_types.hpp"
#include "formats/str.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <optional>
#include <set>

namespace ts2::content {

namespace {

constexpr std::uint32_t kPatternProperties = 0xCCA8E925;
// Property sets are small. A larger one is broken and left out.
constexpr std::size_t kMostPropertyBytes = 64 * 1024;
// The game ships about 2,300 patterns. Far more is a broken install, and the rest are left out.
constexpr std::size_t kMostCoverings = 100000;

// A property's number, or `fallback` when it has none.
long long number(const std::map<std::string, std::string>& p, const char* key, long long fallback) {
    const auto it = p.find(key);
    if (it == p.end() || it->second.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long long v = std::strtoll(it->second.c_str(), &end, 10);
    return end != it->second.c_str() ? v : fallback;
}

std::string text(const std::map<std::string, std::string>& p, const char* key) {
    const auto it = p.find(key);
    return it != p.end() ? it->second : std::string{};
}

// Each product's Catalog/Patterns packages, a folder at a time in load order, each sorted by name.
std::vector<std::vector<std::filesystem::path>> patternPackages(const std::vector<Product>& products) {
    std::vector<std::vector<std::filesystem::path>> out;
    std::set<std::filesystem::path> seen;
    for (const auto& objects : objectLoadOrder(products)) {
        const auto folder = objects.parent_path().parent_path() / "Catalog" / "Patterns";
        if (!seen.insert(folder).second) {
            continue;
        }
        std::error_code ec;
        std::vector<std::filesystem::path> packages;
        for (std::filesystem::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
            if (it->path().extension() == ".package") {
                packages.push_back(it->path());
            }
        }
        std::sort(packages.begin(), packages.end());
        out.push_back(std::move(packages));
    }
    return out;
}

// A property set, none when it is larger than any the game ships, stored or unpacked.
std::optional<std::map<std::string, std::string>> properties(dbpf::Package& package, const dbpf::IndexEntry& e) {
    if (e.size > kMostPropertyBytes) {
        return std::nullopt;
    }
    const auto data = package.read(e);
    if (data.size() > kMostPropertyBytes) {
        return std::nullopt;
    }
    return propset::parse(data);
}

} // namespace

std::vector<Covering> loadCoverings(const std::vector<Product>& products) {
    std::map<std::uint32_t, Covering> byGuid;
    for (const auto& packages : patternPackages(products)) {
        for (const auto& path : packages) {
            try {
                dbpf::Package package{path};
                for (const auto& e : package.entries()) {
                    if (e.key.type != kPatternProperties || byGuid.size() >= kMostCoverings) {
                        continue;
                    }
                    const auto found = properties(package, e);
                    if (!found) {
                        continue;
                    }
                    const auto& p = *found;
                    const std::string type = text(p, "type");
                    const long long guid = number(p, "guid", -1);
                    if ((type != "wall" && type != "floor") || guid < 0 || guid > 0xFFFFFFFFll) {
                        continue;
                    }
                    const auto id = static_cast<std::uint32_t>(guid);
                    if (number(p, "showincatalog", 1) == 0 || number(p, "deprecated", 0) != 0) {
                        byGuid.erase(id);
                        continue;
                    }
                    byGuid[id] = {type == "wall" ? CoveringKind::Wall : CoveringKind::Floor, id, text(p, "subsort"), text(p, "name"),
                                  text(p, "description"), text(p, "filename"), static_cast<int>(number(p, "cost", 0))};
                }
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("build catalog: a pattern package that can't be read is left out: ") + ex.what());
            }
        }
    }
    // Each file name is lowered once, not at every comparison.
    std::vector<std::pair<std::string, Covering>> keyed;
    keyed.reserve(byGuid.size());
    for (auto& [guid, covering] : byGuid) {
        std::string key = covering.filename;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        keyed.emplace_back(std::move(key), std::move(covering));
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Covering> out;
    out.reserve(keyed.size());
    for (auto& [key, covering] : keyed) {
        out.push_back(std::move(covering));
    }
    return out;
}

std::vector<HalfWall> loadHalfWalls(const std::vector<Product>& products) {
    constexpr std::uint32_t kFenceProperties = 0x2CB230B8;
    // The game ships 43 half walls. Far more is a broken install, and the rest are left out.
    constexpr std::size_t kMostHalfWalls = 10000;
    const auto folders = patternPackages(products);
    std::vector<HalfWall> out;
    // The newest product's half walls come first.
    for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) {
        for (const auto& path : *folder) {
            try {
                dbpf::Package package{path};
                std::vector<const dbpf::IndexEntry*> sets;
                std::map<std::pair<std::uint32_t, std::uint32_t>, const dbpf::IndexEntry*> tables;  // by group and instance
                for (const auto& e : package.entries()) {
                    if (e.key.type == kFenceProperties) {
                        sets.push_back(&e);
                    } else if (e.key.type == types::kStringTable) {
                        tables.try_emplace({e.key.group, e.key.instance}, &e);
                    }
                }
                std::stable_sort(sets.begin(), sets.end(), [](const auto* a, const auto* b) { return a->offset < b->offset; });
                for (const auto* e : sets) {
                    if (out.size() >= kMostHalfWalls) {
                        break;
                    }
                    const auto found = properties(package, *e);
                    if (!found) {
                        continue;
                    }
                    const auto& p = *found;
                    const long long guid = number(p, "guid", -1);
                    if (number(p, "ishalfwall", 0) == 0 || guid < 0 || guid > 0xFFFFFFFFll) {
                        continue;
                    }
                    HalfWall wall;
                    wall.guid = static_cast<std::uint32_t>(guid);
                    const long long group = number(p, "stringsetgroupid", -1), instance = number(p, "stringsetid", -1);
                    const auto s = group >= 0 && group <= 0xFFFFFFFFll && instance >= 0 && instance <= 0xFFFFFFFFll
                                       ? tables.find({static_cast<std::uint32_t>(group), static_cast<std::uint32_t>(instance)})
                                       : tables.end();
                    if (s != tables.end()) {
                        const auto table = str::parse(package.read(*s->second));
                        const auto texts = table.forLanguage(1);
                        if (!texts.empty()) {
                            wall.name = texts[0]->value;
                        }
                        if (texts.size() > 1) {
                            wall.description = texts[1]->value;
                        }
                        if (texts.size() > 2) {
                            wall.price = static_cast<int>(std::strtol(texts[2]->value.c_str(), nullptr, 10));
                        }
                    }
                    out.push_back(std::move(wall));
                }
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("build catalog: a half wall package that can't be read is left out: ") + ex.what());
            }
        }
    }
    return out;
}

std::string coveringPictureName(std::uint32_t guid) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%08x", guid);
    return buffer;
}

} // namespace ts2::content
