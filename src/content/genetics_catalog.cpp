#include "content/genetics_catalog.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/propset.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

namespace ts2::content {

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kSkinTone = 0x4C158081, kHairTone = 0x8C1580B5, kTextureOverlay = 0x2C1FD8A1;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string value(const std::map<std::string, std::string>& set, const char* key) {
    const auto it = set.find(key);
    return it != set.end() ? it->second : std::string{};
}

} // namespace

GeneticsCatalog loadGeneticsCatalog(const std::vector<Product>& products) {
    GeneticsCatalog catalog;
    std::set<std::string> seenSkins, seenHairs, seenEyes;
    std::set<fs::path> folders;
    for (const auto& objects : objectLoadOrder(products)) {
        const fs::path folder = objects.parent_path().parent_path() / "Catalog" / "Skins";
        if (!folders.insert(folder).second) {
            continue;
        }
        std::vector<fs::path> packages;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
            if (it->path().extension() == ".package") {
                packages.push_back(it->path());
            }
        }
        std::sort(packages.begin(), packages.end());
        for (const auto& path : packages) {
            try {
                dbpf::Package package{path};
                for (const auto& e : package.entries()) {
                    if (e.key.type != kSkinTone && e.key.type != kHairTone && e.key.type != kTextureOverlay) {
                        continue;
                    }
                    const auto set = propset::parse(package.read(e));
                    if (!set.contains("genetic")) {
                        continue;
                    }
                    if (e.key.type == kTextureOverlay && value(set, "subtype") != "3") {
                        continue;
                    }
                    GeneticColour colour{lower(value(set, "family")), std::strtof(value(set, "genetic").c_str(), nullptr)};
                    // A broken file's value that isn't a number would match no range of values.
                    if (!std::isfinite(colour.genetic)) {
                        continue;
                    }
                    auto& list = e.key.type == kSkinTone ? catalog.skins : e.key.type == kHairTone ? catalog.hairs : catalog.eyes;
                    auto& seen = e.key.type == kSkinTone ? seenSkins : e.key.type == kHairTone ? seenHairs : seenEyes;
                    if (seen.insert(colour.guid).second) {
                        list.push_back(std::move(colour));
                    }
                }
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("genetics: a skin catalog that can't be read is left out: ") + ex.what());
            }
        }
    }
    return catalog;
}

const GeneticColour* findColour(const std::vector<GeneticColour>& colours, const std::string& guid) {
    const std::string wanted = lower(guid);
    const auto it = std::find_if(colours.begin(), colours.end(), [&](const GeneticColour& c) { return c.guid == wanted; });
    return it != colours.end() ? &*it : nullptr;
}

} // namespace ts2::content
