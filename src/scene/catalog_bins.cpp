#include "scene/catalog_bins.hpp"

#include "content/resource_index.hpp"
#include "diag/log.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "scene/game_sort.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <set>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace ts2::scene {

namespace {

constexpr std::uint32_t kCollection = 0x6C4F359D, kBinaryIndex = 0x0C560F39, kPartProperties = 0xEBCF3E27;
constexpr std::uint32_t kBuiltInGroup = 0x0FFEFEFE, kBuiltInInstance = 0x0FFE0000;

auto tied(const scenegraph::Link& k) {
    return std::tie(k.type, k.group, k.instance, k.instanceHigh);
}

bool sameKey(const scenegraph::Link& a, const scenegraph::Link& b) {
    return tied(a) == tied(b);
}

struct KeyLess {
    bool operator()(const scenegraph::Link& a, const scenegraph::Link& b) const { return tied(a) < tied(b); }
};

// A field that may be negative, as sort indexes often are. Unsigned and hex text wraps around, as
// the game keeps the field in 32 bits.
std::int32_t signedNumber(const std::map<std::string, std::string>& p, const std::string& key) {
    const auto it = p.find(key);
    if (it == p.end()) {
        return 0;
    }
    const std::string_view s = it->second;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        return static_cast<std::int32_t>(propset::number(p, key));
    }
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    return error == std::errc{} ? static_cast<std::int32_t>(value) : 0;
}

bool noTone(const std::string& tone) {
    return std::all_of(tone.begin(), tone.end(), [](char c) { return c == '0'; });
}

} // namespace

std::uint32_t collectionKind(std::string_view type) {
    // The game's own list. "lotcollection" sets two bits, as written.
    static constexpr std::pair<std::string_view, std::uint32_t> kKinds[] = {
        {"gamesim", 0x1}, {"gamehead", 0x2}, {"toolsim", 0x4}, {"genetics", 0x8}, {"cosmetics", 0x10},
        {"archetype", 0x20}, {"modifier", 0x40}, {"clothing", kClothingKind}, {"collection", 0x100},
        {"gamenpcsim", 0x200}, {"communitylotcollection", 0x400}, {"lotcollection", 0x500},
        {"toolmodifier", 0x1000}, {"gamepetbreed", 0x2000}, {"gamepetcoat", 0x4000},
        {"gamebodyoptions", 0x8000}, {"breedcollection", 0x10000}, {"specialoutfit", kSpecialOutfitKind},
        {"hiddenpetbreed", 0x40000},
    };
    for (const auto& [name, kind] : kKinds) {
        if (name == type) {
            return kind;
        }
    }
    return 0;
}

std::vector<std::filesystem::path> catalogBinPackages(const std::vector<content::Product>& products) {
    std::vector<std::filesystem::path> packages;
    for (const auto& product : content::inGameOrder(products)) {
        if (product.objectPackages.empty()) {
            continue;
        }
        // Res/Objects/objects.package -> Res/Catalog/Bins
        const auto folder = product.objectPackages.front().parent_path().parent_path() / "Catalog" / "Bins";
        std::error_code ec;
        std::vector<std::filesystem::path> found;
        for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == ".package" && it->is_regular_file(ec)) {
                found.push_back(it->path());
            }
        }
        std::sort(found.begin(), found.end());
        packages.insert(packages.end(), found.begin(), found.end());
    }
    return packages;
}

CatalogBins readCatalogBins(const std::vector<std::filesystem::path>& packages) {
    CatalogBins bins;
    for (const auto& path : packages) {
        content::ResourceIndex index;
        try {
            index.addPackage(path);
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("catalog bins: a package that can't be read is left out: ") + ex.what());
            continue;
        }
        auto& entries = bins.packages.emplace_back();
        for (const auto& r : index.resources()) {
            const auto& key = r.entry.key;
            if (key.type != kCollection && key.type != kBinaryIndex) {
                continue;
            }
            try {
                const auto p = propset::parse(index.read(r));
                if (key.type == kCollection) {
                    const auto type = p.find("type");
                    bins.collections.push_back({{key.type, key.group, key.instance, key.instanceHigh},
                                                collectionKind(type != p.end() ? type->second : std::string()),
                                                propset::number(p, "flags"),
                                                signedNumber(p, "sortindex")});
                    continue;
                }
                const auto* refs = index.find({reflist::kType, key.group, key.instance, key.instanceHigh});
                if (refs == nullptr || !p.contains("objectidx") || !p.contains("binidx")) {
                    continue;
                }
                const auto links = reflist::parse(index.read(*refs));
                const std::uint32_t object = propset::number(p, "objectidx"), bin = propset::number(p, "binidx");
                if (object >= links.size() || bin >= links.size()) {
                    continue;
                }
                entries.push_back({links[object], links[bin], signedNumber(p, "sortindex")});
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("catalog bins: a resource that can't be read is left out: ") + ex.what());
            }
        }
    }
    return bins;
}

std::vector<CatalogCollection> rankedCollections(const CatalogBins& bins) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, CatalogCollection> gathered;
    for (std::uint32_t kind = 1; kind <= kClothingKind; kind <<= 1) {
        gathered.emplace(std::pair(kBuiltInGroup, kBuiltInInstance | kind),
                         CatalogCollection{{kCollection, kBuiltInGroup, kBuiltInInstance | kind, 0}, kind, 0, 0});
    }
    for (const auto& c : bins.collections) {
        gathered.emplace(std::pair(c.key.group, c.key.instance), c);
    }
    std::vector<CatalogCollection> ranked;
    ranked.reserve(gathered.size());
    for (const auto& [key, c] : gathered) {
        ranked.push_back(c);
    }
    gameSort(ranked, [](const CatalogCollection& a, const CatalogCollection& b) { return a.sortIndex < b.sortIndex; });
    return ranked;
}

std::vector<const CatalogPart*> binnedParts(const CatalogBins& bins, const OutfitCatalog& catalog, std::uint32_t kind) {
    const auto ranked = rankedCollections(bins);
    std::map<scenegraph::Link, std::size_t, KeyLess> rank;
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        rank.emplace(ranked[i].key, i);
    }
    // A collection with flag 0x1 or 0x2 is passed over, unless it is the first of exactly this kind.
    const auto primary = std::find_if(ranked.begin(), ranked.end(), [&](const CatalogCollection& c) { return c.kind == kind; });
    struct Item {
        const CatalogPart* part;
        std::size_t rank;
        std::int32_t sortIndex;
    };
    std::vector<Item> items;
    std::set<scenegraph::Link, KeyLess> seen;
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        const auto& c = ranked[i];
        if (!(c.kind & kind) || (ranked.begin() + static_cast<std::ptrdiff_t>(i) != primary && (c.flags & 3))) {
            continue;
        }
        for (const auto& entries : bins.packages) {
            for (const auto& e : entries) {
                if (!sameKey(e.bin, c.key) || e.object.type != kPartProperties || !seen.insert(e.object).second) {
                    continue;
                }
                if (const auto* part = findPart(catalog, e.object)) {
                    items.push_back({part, i, e.sortIndex});
                }
            }
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        const std::uint32_t pa = a.part->product, pb = b.part->product;
        if (pa != pb) {
            if (pa == 0) {
                return true;
            }
            if (pa == 0xFFFF || pb == 0) {
                return false;
            }
            return pb == 0xFFFF || pb < pa;
        }
        if (a.rank != b.rank) {
            return a.rank < b.rank;
        }
        return pa == 0 ? a.sortIndex > b.sortIndex : a.sortIndex < b.sortIndex;
    });
    std::vector<const CatalogPart*> parts;
    parts.reserve(items.size());
    for (const auto& item : items) {
        parts.push_back(item.part);
    }
    return parts;
}

std::vector<const CatalogPart*> builderParts(const std::vector<const CatalogPart*>& binned, const BuilderQuery& query) {
    const auto tone = query.hairTone ? std::optional(catalogGuid(*query.hairTone)) : std::nullopt;
    std::vector<const CatalogPart*> out;
    for (const auto* p : binned) {
        if (!(query.age & p->age) || !(query.gender & p->gender) || !(query.species & p->species) ||
            (p->parts != 0 && !(p->parts & query.parts)) || (p->category != 0 && !(p->category & query.category)) ||
            (!query.withHidden && (p->flags & 1)) || (tone && !noTone(p->hairTone) && p->hairTone != *tone)) {
            continue;
        }
        out.push_back(p);
    }
    return out;
}

} // namespace ts2::scene
