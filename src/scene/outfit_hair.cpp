#include "scene/outfit_hair.hpp"

#include "formats/propset.hpp"

namespace ts2::scene {

namespace {

constexpr std::uint32_t kHairPart = 1, kHuman = 1;
// The bald scalp and the PlantSim skin, which OK leaves alone.
constexpr std::uint32_t kSkinOutfits = 0x80 | 0x800;

std::uint32_t listKey(std::uint32_t category) {
    return (category << 16) | kHairPart;
}

bool noTone(const std::string& tone) {
    const std::string guid = catalogGuid(tone);
    return guid.find_first_not_of('0') == std::string::npos;
}

bool sameTone(const std::string& a, const std::string& b) {
    return catalogGuid(a) == catalogGuid(b);
}

struct Sim {
    std::uint32_t age = 0, gender = 0;
    std::string skin;
};

Sim simOf(const AgeData& data) {
    Sim sim;
    sim.age = propset::number(data.properties, "age");
    sim.gender = propset::number(data.properties, "gender") == 1 ? 1 : 2;
    if (const auto it = data.properties.find("skincolor"); it != data.properties.end()) {
        sim.skin = it->second;
    }
    return sim;
}

// A part made for this Sim's age, gender and skin, and for the outfit. A part with no skin tone fits
// every skin. What the game does with a part of another skin tone is not known yet, and no such hair
// is offered, so it doesn't fit here.
bool fitsSim(const CatalogPart& p, const Sim& sim, std::uint32_t category) {
    return (p.age & sim.age) && (p.gender & sim.gender) && (noTone(p.skinTone) || sameTone(p.skinTone, sim.skin)) &&
           (p.category == 0 || (p.category & category));
}

bool unflagged(const CatalogPart& p) {
    return (p.flags & 1) == 0;
}

std::vector<const CatalogPart*> hairsOfTone(const OutfitCatalog& catalog, const Sim& sim, std::uint32_t category, const std::string& tone) {
    PartQuery query;
    query.age = sim.age;
    query.gender = sim.gender;
    query.species = kHuman;
    query.part = kHairPart;
    query.category = category;
    if (!noTone(sim.skin)) {
        query.skinTone = sim.skin;
    }
    // The game's search lets every tone through when the Sim's tone is the null GUID.
    if (!noTone(tone)) {
        query.hairTone = tone;
    }
    return findParts(catalog, query);
}

} // namespace

HairSlots hairSlots(const AgeData& data) {
    HairSlots slots;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (const auto it = data.lists.find(listKey(1u << i)); it != data.lists.end() && !it->second.empty()) {
            slots[i] = it->second.back();
        }
    }
    return slots;
}

bool isStockHairTone(const std::string& tone) {
    const std::string guid = catalogGuid(tone);
    return guid.size() == 32 && guid.compare(0, 7, "0000000") == 0 && guid[7] >= '1' && guid[7] <= '5' &&
           guid.find_first_not_of('0', 8) == std::string::npos;
}

// The builder looks at the hair itself, then at hairs of the Sim's colour in key order. There a
// shown hair of the same family wins, else (when the family has only hidden hairs) the family's last
// one, else the first shown hair of exactly that colour, else the last shown hair. With no shown
// hair of that colour it looks at each colour's hairs in turn: the first of the same family wins,
// else the last shown hair of them all. The builder also keeps the hairs it has picked by family and
// looks there first, which isn't kept here (UI-63).
std::optional<scenegraph::Link> outfitHair(const OutfitCatalog& catalog, const AgeData& data, const scenegraph::Link& hair,
                                           std::uint32_t category, const std::string& tone, bool customTones) {
    const Sim sim = simOf(data);
    const CatalogPart* chosen = findPart(catalog, hair);
    if (chosen == nullptr) {
        return std::nullopt;
    }
    if (fitsSim(*chosen, sim, category) &&
        (noTone(chosen->hairTone) || sameTone(chosen->hairTone, tone) || (customTones && !isStockHairTone(chosen->hairTone)))) {
        return chosen->key;
    }
    const std::string family = catalogGuid(chosen->family);
    const auto list = hairsOfTone(catalog, sim, category, tone);
    const CatalogPart* lastOfFamily = nullptr;
    for (const auto* p : list) {
        if (catalogGuid(p->family) == family) {
            if (unflagged(*p)) {
                return p->key;
            }
            lastOfFamily = p;
        }
    }
    if (lastOfFamily != nullptr) {
        return lastOfFamily->key;
    }
    const CatalogPart* lastShown = nullptr;
    for (const auto* p : list) {
        if (unflagged(*p)) {
            if (sameTone(p->hairTone, tone)) {
                return p->key;
            }
            lastShown = p;
        }
    }
    if (lastShown != nullptr) {
        return lastShown->key;
    }
    // Every colour made for the Sim's age and gender, each by its own GUID, in key order.
    for (const auto& colour : catalog.hairTones) {
        if (!(colour.age & sim.age) || !(colour.gender & sim.gender)) {
            continue;
        }
        for (const auto* p : hairsOfTone(catalog, sim, category, colour.family)) {
            if (catalogGuid(p->family) == family) {
                return p->key;
            }
            if (unflagged(*p)) {
                lastShown = p;
            }
        }
    }
    return lastShown != nullptr ? std::optional(lastShown->key) : std::nullopt;
}

void wearHairs(AgeData& data, const OutfitCatalog& catalog, const HairSlots& slots, const std::string& tone) {
    data.properties["haircolor"] = tone;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const std::uint32_t category = 1u << i;
        // A hair the catalog doesn't hold leaves the outfit's hair as it is, as in the game.
        if (!slots[i] || (category & kSkinOutfits) || findPart(catalog, *slots[i]) == nullptr) {
            continue;
        }
        if (const auto hair = outfitHair(catalog, data, *slots[i], category, tone, true)) {
            data.lists[listKey(category)] = {*hair};
        } else {
            data.lists.erase(listKey(category));
        }
    }
}

} // namespace ts2::scene
