#include "scene/outfit_catalog.hpp"

#include "content/downloads.hpp"
#include "content/resource_index.hpp"
#include "diag/log.hpp"
#include "formats/propset.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <tuple>

namespace ts2::scene {

namespace {

constexpr std::uint32_t kPartProperties = 0xEBCF3E27;
constexpr std::uint32_t kSkinTone = 0x4C158081, kHairTone = 0x8C1580B5;
constexpr std::uint32_t kMeshOverlay = 0x0C1FE246, kTextureOverlay = 0x2C1FD8A1, kFaceTemplate = 0x8C93E35C;
constexpr int kPartBits = 14;  // the catalog lists parts 0x1 to 0x2000

bool keyOrder(const CatalogPart& a, const CatalogPart& b) {
    return std::tie(a.key.type, a.key.group, a.key.instanceHigh, a.key.instance) <
           std::tie(b.key.type, b.key.group, b.key.instanceHigh, b.key.instance);
}

bool noTone(const std::string& tone) {
    return std::all_of(tone.begin(), tone.end(), [](char c) { return c == '0'; });
}

bool toneFits(const std::string& partTone, const std::optional<std::string>& asked) {
    return !asked || noTone(partTone) || partTone == *asked;
}

bool matches(const CatalogPart& p, const PartQuery& q, const std::optional<std::string>& skin, const std::optional<std::string>& hair) {
    return (q.age & p.age) && (q.gender & p.gender) && toneFits(p.skinTone, skin) && toneFits(p.hairTone, hair) &&
           (q.species == 0 || (q.species & p.species)) && (q.category == 0 || p.category == 0 || (p.category & q.category));
}

} // namespace

std::uint32_t partCategories(const std::map<std::string, std::string>& p) {
    constexpr std::uint32_t kEveryday = 0x01, kOuterwear = 0x1000, kHairPart = 1, kFacePart = 2;
    std::uint32_t categories = propset::number(p, "category") & 0xFFFF;
    // A part with no version is version 1.
    const std::uint32_t version = p.contains("version") ? propset::number(p, "version") & 0xFFFF : 1;
    const std::uint32_t parts = propset::number(p, p.contains("parts") ? "parts" : "outfit");
    const std::uint32_t gender = propset::number(p, "gender") & 0xFF;
    if (!(categories & kEveryday)) {
        return categories;
    }
    const bool oneGender = gender != 0 && (gender & (gender - 1)) == 0;
    if ((version < 5 && parts == kHairPart) || (version < 6 && parts == kFacePart && oneGender)) {
        categories |= kOuterwear;
    }
    return categories;
}

// The game keeps age in seven bits and gender and species in a byte each. A part of species 0 is human.
CatalogPart catalogPart(const scenegraph::Link& key, const std::map<std::string, std::string>& p) {
    CatalogPart part;
    part.key = key;
    part.age = propset::number(p, "age") & 0x7F;
    part.gender = propset::number(p, "gender") & 0xFF;
    part.species = propset::number(p, "species") & 0xFF;
    if (part.species == 0) {
        part.species = 1;
    }
    part.parts = propset::number(p, p.contains("parts") ? "parts" : "outfit");
    part.flags = propset::number(p, "flags");
    part.category = partCategories(p);
    part.fitness = propset::number(p, "fitness");
    part.subtype = propset::number(p, "subtype");
    part.layer = propset::number(p, "layer");
    const auto text = [&](const char* name) {
        const auto it = p.find(name);
        return catalogGuid(it != p.end() ? it->second : std::string());
    };
    part.skinTone = text("skintone");
    part.hairTone = text("hairtone");
    part.family = text("family");
    return part;
}

std::string catalogGuid(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out.empty() ? std::string(32, '0') : out;
}

std::vector<std::filesystem::path> skinCatalogPackages(const std::vector<content::Product>& products) {
    std::vector<std::filesystem::path> packages;
    const auto ordered = content::inGameOrder(products);
    for (const char* file : {"Skins.package", "Jewelry.package"}) {
        for (const auto& product : ordered) {
            if (product.objectPackages.empty()) {
                continue;
            }
            // Res/Objects/objects.package -> Res/Catalog/Skins
            const auto path = product.objectPackages.front().parent_path().parent_path() / "Catalog" / "Skins" / file;
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec)) {
                packages.push_back(path);
            }
        }
    }
    return packages;
}

OutfitCatalog readOutfitCatalog(const std::vector<std::filesystem::path>& packages) {
    content::ResourceIndex index;
    for (const auto& path : packages) {
        try {
            index.addPackage(path);
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("skin catalog: a package that can't be read is left out: ") + ex.what());
        }
    }
    content::addDownloads(index);
    OutfitCatalog catalog;
    const auto listOf = [&](std::uint32_t type) -> std::vector<CatalogPart>* {
        switch (type) {
        case kPartProperties: return &catalog.parts;
        case kSkinTone: return &catalog.skinTones;
        case kHairTone: return &catalog.hairTones;
        case kMeshOverlay: case kTextureOverlay: return &catalog.faceOverlays;
        case kFaceTemplate: return &catalog.faceTemplates;
        default: return nullptr;
        }
    };
    for (const auto& r : index.resources()) {
        auto* list = listOf(r.entry.key.type);
        if (list == nullptr) {
            continue;
        }
        try {
            const scenegraph::Link key{r.entry.key.type, r.entry.key.group, r.entry.key.instance, r.entry.key.instanceHigh};
            list->push_back(catalogPart(key, propset::parse(index.read(r))));
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("skin catalog: a part that can't be read is left out: ") + ex.what());
        }
    }
    // Sorted by type first, the mesh overlays come before the texture overlays, as the game lists them.
    for (auto* list : {&catalog.parts, &catalog.skinTones, &catalog.hairTones, &catalog.faceOverlays, &catalog.faceTemplates}) {
        std::sort(list->begin(), list->end(), keyOrder);
    }
    return catalog;
}

const CatalogPart* findPart(const OutfitCatalog& catalog, const scenegraph::Link& key) {
    CatalogPart probe;
    probe.key = key;
    const auto it = std::lower_bound(catalog.parts.begin(), catalog.parts.end(), probe, keyOrder);
    return it != catalog.parts.end() && !keyOrder(probe, *it) ? &*it : nullptr;
}

namespace {

// The filter with its GUIDs as the catalog keeps them.
PartFilter normalized(PartFilter f) {
    for (auto* guid : {&f.family, &f.skinTone, &f.hairTone}) {
        if (*guid) {
            *guid = catalogGuid(**guid);
        }
    }
    return f;
}

bool fitsNormalized(const CatalogPart& p, const PartFilter& f) {
    const auto shares = [](const std::optional<std::uint32_t>& asked, std::uint32_t value) { return !asked || (*asked & value); };
    const auto tone = [](const std::optional<std::string>& asked, const std::string& own) {
        return !asked || noTone(own) || noTone(*asked) || own == *asked;
    };
    return shares(f.species, p.species) && shares(f.age, p.age) && shares(f.gender, p.gender) &&
           (!f.category || *f.category == 0 || p.category == 0 || (p.category & *f.category)) &&
           (!f.requiredFlags || (p.flags & *f.requiredFlags) == *f.requiredFlags) &&
           (!f.excludedFlags || (p.flags & *f.excludedFlags) == 0) && (!f.family || p.family == *f.family) &&
           tone(f.skinTone, p.skinTone) && tone(f.hairTone, p.hairTone);
}

} // namespace

bool fitsFilter(const CatalogPart& p, const PartFilter& f) {
    return fitsNormalized(p, normalized(f));
}

std::vector<const CatalogPart*> filterParts(const OutfitCatalog& catalog, const PartFilter& filter, std::size_t most) {
    const PartFilter f = normalized(filter);
    const auto fits = [&](const CatalogPart& p) { return fitsNormalized(p, f); };
    std::vector<const CatalogPart*> out;
    for (int i = 0; i < kPartBits && out.size() < most; ++i) {
        const std::uint32_t bit = 1u << i;
        if (!(f.parts & bit)) {
            continue;
        }
        for (const auto& p : catalog.parts) {
            if (out.size() >= most) {
                break;
            }
            if ((p.parts & bit) && fits(p)) {
                out.push_back(&p);
            }
        }
    }
    return out;
}

bool hasSkinTone(const OutfitCatalog& catalog, const std::string& tone, std::uint32_t species, std::uint32_t age, std::uint32_t gender) {
    const std::string family = catalogGuid(tone);
    return std::any_of(catalog.skinTones.begin(), catalog.skinTones.end(), [&](const CatalogPart& t) {
        return (species & t.species) && (age & t.age) && (gender & t.gender) && t.family == family;
    });
}

std::vector<const CatalogPart*> findParts(const OutfitCatalog& catalog, const PartQuery& query) {
    const auto skin = query.skinTone ? std::optional(catalogGuid(*query.skinTone)) : std::nullopt;
    const auto hair = query.hairTone ? std::optional(catalogGuid(*query.hairTone)) : std::nullopt;
    std::vector<const CatalogPart*> out;
    for (int i = 0; i < kPartBits; ++i) {
        const std::uint32_t bit = 1u << i;
        if (!(query.part & bit)) {
            continue;
        }
        for (const auto& p : catalog.parts) {
            if ((p.parts & bit) && matches(p, query, skin, hair)) {
                out.push_back(&p);
            }
        }
    }
    return out;
}

} // namespace ts2::scene
