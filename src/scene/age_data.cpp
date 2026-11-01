#include "scene/age_data.hpp"

#include "content/resource_index.hpp"
#include "formats/propset.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <tuple>

namespace ts2::scene {

namespace {

namespace sg = ts2::scenegraph;

constexpr std::uint32_t kHair = 0x01, kFace = 0x02, kTop = 0x04, kBody = 0x08, kBottom = 0x10, kGlasses = 0x20;
constexpr std::uint32_t kSkinCategory = 0x80, kPlantCategory = 0x800;
constexpr int kCategories = 13;  // 0x1 to 0x1000
constexpr std::uint32_t kTeen = 0x04, kAdult = 0x08, kElder = 0x10, kBaby = 0x20, kYoungAdult = 0x40;
constexpr std::uint32_t kHuman = 1;
constexpr const char* kGreyHair = "00000005-0000-0000-0000-000000000000";
constexpr const char* kNoSkinTone = "00000002-0000-0000-0000-000000000000";
constexpr const char* kUnknownHair = "00000002-0000-0000-0000-000000000000";
constexpr const char* kOtherHair = "00000006-0000-0000-0000-000000000000";
// The outfits whose hairs the copy back keeps in the Sim's hair colour.
constexpr std::uint32_t kHairCopyCategories = 0x1779;

std::uint32_t listKey(std::uint32_t category, std::uint32_t part) {
    return category << 16 | part;
}

// The catalog's flags for an outfit category. 1 keeps hairs of flag 2 out of it, 2 leaves it out of
// the clothes drawn at random.
std::uint32_t categoryFlags(std::uint32_t category) {
    switch (category) {
    case 0x08: case 0x10: case 0x40: case 0x80: case 0x200: return 1;
    case 0x400: return 2;
    case 0x800: return 3;
    default: return 0;
    }
}

bool isList(const std::string& name) {
    for (const char* prefix : {"listcnt", "lk", "ls", "le"}) {
        if (name.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

// The parts a Sim of these looks wears, as the catalog has them for it.
struct Looks {
    std::uint32_t species = kHuman, age = 0, gender = 0, fitness = 0;
    std::string skin, hair;
};

Looks looksOf(const AgeData& d) {
    const auto text = [&](const char* name) {
        const auto it = d.properties.find(name);
        return it != d.properties.end() ? it->second : std::string();
    };
    Looks l;
    l.species = propset::number(d.properties, "species");
    if (l.species == 0) {
        l.species = kHuman;
    }
    l.age = propset::number(d.properties, "age");
    l.gender = propset::number(d.properties, "gender");
    l.fitness = propset::number(d.properties, "fitness");
    l.skin = text("skincolor");
    l.hair = text("haircolor");
    return l;
}

bool noTone(const std::string& tone) {
    return std::all_of(tone.begin(), tone.end(), [](char c) { return c == '0'; });
}

// Whether a GUID is all zeros, the catalog's "none".
bool none(const std::string& guid) {
    const std::string g = catalogGuid(guid);
    return std::all_of(g.begin(), g.end(), [](char c) { return c == '0'; });
}

// The catalog draws both makers of age data share: the bare skin outfit and clothes drawn at
// random, into the new data `n_`, for Sims of age `drawAge_`.
class CatalogDraws {
protected:
    CatalogDraws(const OutfitCatalog& catalog, OutfitRandom& random, std::uint32_t drawAge)
        : catalog_(catalog), random_(random), drawAge_(drawAge) {}

    // The bare skin outfit's parts from the catalog: per part, the first that matches the Sim's
    // fitness, else the last of fitness 0, else the first. A young adult wears an adult's.
    void defaultOutfits(AgeData& d) {
        const Looks now = looksOf(d);
        PartQuery q;
        q.age = now.age == kYoungAdult ? kAdult : now.age;
        q.gender = now.gender;
        q.species = now.species;
        q.skinTone = now.skin;
        q.hairTone = now.hair;
        q.category = kSkinCategory;
        for (const std::uint32_t part : {kFace, kHair, kTop, kBottom, kBody}) {
            d.lists.erase(listKey(kSkinCategory, part));
            q.part = part;
            const auto found = findParts(catalog_, q);
            const CatalogPart* pick = nullptr;
            for (const auto* p : found) {
                if (p->fitness == now.fitness) {
                    pick = p;
                    break;
                }
            }
            if (pick == nullptr) {
                for (const auto* p : found) {
                    if (p->fitness == 0) {
                        pick = p;
                    }
                }
            }
            if (pick == nullptr && !found.empty()) {
                pick = found.front();
            }
            if (pick != nullptr) {
                d.lists[listKey(kSkinCategory, part)].push_back(pick->key);
            }
        }
    }

    // The catalog's parts for the drawn age in an outfit category, kept by their flags: those with
    // one of `wanted` (any for -1) and none of `unwanted`.
    std::vector<const CatalogPart*> candidates(std::uint32_t part, std::uint32_t category, std::int64_t wanted,
                                               std::uint32_t unwanted) {
        const Looks now = looksOf(n_);
        PartQuery q;
        q.age = drawAge_;
        q.gender = now.gender;
        q.species = now.species;
        q.skinTone = now.skin;
        q.hairTone = now.hair;
        q.part = part;
        q.category = category;
        std::vector<const CatalogPart*> kept;
        if (wanted == 0) {
            if (unwanted == 0) {
                kept = findParts(catalog_, q);
            }
            return kept;
        }
        for (const auto* p : findParts(catalog_, q)) {
            if ((wanted == -1 || (p->flags & static_cast<std::uint32_t>(wanted))) && !(p->flags & unwanted)) {
                kept.push_back(p);
            }
        }
        return kept;
    }

    void put(std::uint32_t category, std::uint32_t part, const std::vector<const CatalogPart*>& from,
             std::initializer_list<std::uint32_t> clears) {
        for (const std::uint32_t c : clears) {
            n_.lists.erase(listKey(category, c));
        }
        n_.lists[listKey(category, part)].push_back(from[random_.below(static_cast<std::uint32_t>(from.size()))]->key);
    }

    // One round of clothes drawn at random for every outfit but the skin and the PlantSim's: a
    // full body about half the time, else a top and a bottom, else a body after all.
    void fill(std::int64_t wanted, std::uint32_t unwanted) {
        for (int i = 0; i < kCategories; ++i) {
            const std::uint32_t c = 1u << i;
            if ((c & (kSkinCategory | kPlantCategory)) || (categoryFlags(c) & 2)) {
                continue;
            }
            if (random_.below(100) > 50) {
                if (const auto bodies = candidates(kBody, c, wanted, unwanted); !bodies.empty()) {
                    put(c, kBody, bodies, {kBody, kBottom, kTop});
                    continue;
                }
            }
            if (const auto tops = candidates(kTop, c, wanted, unwanted); !tops.empty()) {
                put(c, kTop, tops, {kBody, kTop});
            }
            if (const auto bottoms = candidates(kBottom, c, wanted, unwanted); !bottoms.empty()) {
                put(c, kBottom, bottoms, {kBody, kBottom});
                continue;
            }
            if (const auto bodies = candidates(kBody, c, wanted, unwanted); !bodies.empty()) {
                put(c, kBody, bodies, {kBody, kBottom, kTop});
            }
        }
    }

    const OutfitCatalog& catalog_;
    OutfitRandom& random_;
    std::uint32_t drawAge_;
    AgeData n_;
};

class AgeUp : CatalogDraws {
public:
    AgeUp(const AgeData& old, std::uint32_t age, const std::vector<CatalogPart>& own, const OutfitCatalog& catalog,
          OutfitRandom& random)
        : CatalogDraws(catalog, random, age), a_(old), age_(age), own_(own), was_(looksOf(old)) {}

    std::optional<AgeData> run() {
        if (was_.species != kHuman || was_.age == kBaby) {
            return std::nullopt;
        }
        copy();
        n_.properties["age"] = std::to_string(age_);
        if (age_ == kElder) {
            n_.properties["haircolor"] = kGreyHair;
        }
        n_.properties["stretch"] = age_ == kTeen ? "0.94" : "1";
        // The new age wears its own skeleton, whatever the file named.
        for (const char* name : {"skeletonkeyidx", "skeltype", "skelgroup", "skelinst"}) {
            n_.properties.erase(name);
        }
        n_.skeleton.reset();
        if (!faces()) {
            return std::nullopt;
        }
        defaultOutfits(n_);
        hairs();
        clothes();
        plantParts();
        return copyBack();
    }

private:
    void copyList(std::uint32_t key) {
        if (const auto it = a_.lists.find(key); it != a_.lists.end() && !it->second.empty()) {
            n_.lists[key] = it->second;
        }
    }

    // Everything but the hairs. The bare skin outfit comes along only when the catalog knows the
    // Sim's skin tone. Otherwise the tone becomes "none" and the default skin outfit is put on.
    void copy() {
        for (const auto& [name, value] : a_.properties) {
            if (!isList(name)) {
                n_.properties[name] = value;
            }
        }
        if (hasSkinTone(catalog_, was_.skin, was_.species, was_.age, was_.gender)) {
            for (const std::uint32_t part : {kBody, kBottom, kTop, kHair, kFace, kGlasses}) {
                copyList(listKey(kSkinCategory, part));
            }
        } else {
            n_.properties["skincolor"] = kNoSkinTone;
            defaultOutfits(n_);
        }
        for (int i = 0; i < kCategories; ++i) {
            const std::uint32_t category = 1u << i;
            if (category == kSkinCategory || category == kPlantCategory) {
                continue;
            }
            for (const std::uint32_t part : {kGlasses, kFace, kTop, kBottom, kBody}) {
                copyList(listKey(category, part));
            }
        }
        for (const std::uint32_t part : {kBody, kBottom, kTop, kHair}) {
            copyList(listKey(kPlantCategory, part));
        }
    }

    // Faces of exactly the new age and gender among the Sim's own parts, worn in every outfit.
    // False when there are none for a young adult: the game then makes young adult copies of the
    // adult faces, which isn't done here yet.
    bool faces() {
        const std::uint32_t gender = propset::number(n_.properties, "gender");
        std::vector<sg::Link> found;
        for (const auto& p : own_) {
            if (p.age == age_ && p.gender == gender) {
                found.push_back(p.key);
            }
        }
        if (found.empty()) {
            return age_ != kYoungAdult;
        }
        for (int i = 0; i < kCategories; ++i) {
            n_.lists[listKey(1u << i, kFace)] = found;
        }
        return true;
    }

    // The new data copied back into the Sim's own, as aging ends. The bare skin outfit, glasses and
    // PlantSim lists are replaced outright. Faces, hairs and clothes are replaced only by lists that
    // have something, so an outfit the new data left empty keeps the old one.
    AgeData copyBack() {
        AgeData d = a_;
        for (const char* name : {"skeletonkeyidx", "skeltype", "skelgroup", "skelinst"}) {
            d.properties.erase(name);
        }
        for (const auto& [name, value] : n_.properties) {
            d.properties[name] = value;
        }
        d.skeleton = n_.skeleton;
        const Looks now = looksOf(d);
        const auto has = [&](std::uint32_t key) {
            const auto it = n_.lists.find(key);
            return it != n_.lists.end() && !it->second.empty();
        };
        // A top or bottom put on takes the body off, and a body takes both off.
        const auto replace = [&](std::uint32_t category, std::uint32_t part) {
            const std::uint32_t key = listKey(category, part);
            d.lists.erase(key);
            if (!has(key)) {
                return;
            }
            if (part == kTop || part == kBottom) {
                d.lists.erase(listKey(category, kBody));
            } else if (part == kBody) {
                d.lists.erase(listKey(category, kTop));
                d.lists.erase(listKey(category, kBottom));
            }
            d.lists[key] = n_.lists.at(key);
        };
        if (hasSkinTone(catalog_, now.skin, now.species, now.age, now.gender)) {
            for (const std::uint32_t part : {kBody, kBottom, kTop, kHair, kFace, kGlasses}) {
                replace(kSkinCategory, part);
            }
        } else {
            d.properties["skincolor"] = kNoSkinTone;
            std::erase_if(d.lists, [](const auto& l) { return (l.first >> 16) == kSkinCategory; });
            defaultOutfits(d);
        }
        for (int i = 0; i < kCategories; ++i) {
            const std::uint32_t c = 1u << i;
            if (c == kSkinCategory || c == kPlantCategory) {
                continue;
            }
            replace(c, kGlasses);
            if (has(listKey(c, kFace))) {
                replace(c, kFace);
            }
        }
        for (const std::uint32_t part : {kBody, kBottom, kTop, kHair}) {
            replace(kPlantCategory, part);
        }
        for (int i = 0; i < kCategories; ++i) {
            const std::uint32_t c = 1u << i;
            if ((c & kHairCopyCategories) && has(listKey(c, kHair))) {
                d.lists[listKey(c, kHair)] = inHairColour(n_.lists.at(listKey(c, kHair)), c, now);
            }
        }
        for (int i = 0; i < kCategories; ++i) {
            const std::uint32_t c = 1u << i;
            if (c == kSkinCategory || c == kPlantCategory) {
                continue;
            }
            for (const std::uint32_t part : {kTop, kBottom, kBody}) {
                if (has(listKey(c, part))) {
                    replace(c, part);
                }
            }
        }
        return d;
    }

    // Hairs as the copy keeps them: a hair of no colour, the Sim's colour or "Other" stays, and any
    // other becomes its family's hair for the outfit, else stays as it is.
    std::vector<sg::Link> inHairColour(const std::vector<sg::Link>& hairs, std::uint32_t category, const Looks& now) {
        std::vector<sg::Link> out;
        const std::string colour = catalogGuid(none(now.hair) ? std::string(kUnknownHair) : now.hair);
        for (const auto& hair : hairs) {
            const CatalogPart* p = findPart(catalog_, hair);
            if (p == nullptr || noTone(p->hairTone) || p->hairTone == colour || p->hairTone == catalogGuid(kOtherHair)) {
                out.push_back(hair);
                continue;
            }
            PartFilter q;
            q.parts = kHair;
            q.species = now.species;
            q.age = now.age;
            q.gender = now.gender;
            if (!none(now.skin)) {
                q.skinTone = now.skin;
            }
            q.hairTone = colour;
            q.category = category;
            q.family = p->family;
            if (categoryFlags(category) & 1) {
                q.excludedFlags = 2;
            }
            q.requiredFlags = p->flags;
            const CatalogPart* pick = first(q);
            if (pick == nullptr) {
                q.requiredFlags.reset();
                pick = first(q);
            }
            out.push_back(pick != nullptr ? pick->key : hair);
        }
        return out;
    }

    const CatalogPart* first(const PartFilter& f) {
        const auto found = filterParts(catalog_, f, 1);
        return found.empty() ? nullptr : found.front();
    }

    // The new age's hair for an old hair in an outfit category: of the same family when it has
    // one, loosening the search step by step until one is found.
    std::optional<sg::Link> convert(const sg::Link& hair, std::uint32_t category) {
        const CatalogPart* old = findPart(catalog_, hair);
        if (old == nullptr) {
            return std::nullopt;
        }
        PartFilter q;
        q.parts = kHair;
        q.species = was_.species;
        q.age = age_;
        q.gender = was_.gender;
        if (!none(was_.skin)) {
            q.skinTone = was_.skin;
        }
        const std::string tone = age_ == kElder ? std::string(kGreyHair) : was_.hair;
        if (!none(tone)) {
            q.hairTone = tone;
        }
        q.category = category;
        q.family = old->family;
        if (categoryFlags(category) & 1) {
            q.excludedFlags = 2;
        }
        if (const auto it = converted_.find(hair); it != converted_.end()) {
            if (const auto* cached = findPart(catalog_, it->second); cached != nullptr && fitsFilter(*cached, q)) {
                return it->second;
            }
        }
        const CatalogPart* pick = none(old->family) ? familyless(q) : sameFamily(q);
        if (pick == nullptr) {
            return std::nullopt;
        }
        converted_[hair] = pick->key;
        return pick->key;
    }

    // A hair with no family: one of flag 4, then any, then the bare skin outfit's hair that suits
    // the Sim's fitness best.
    const CatalogPart* familyless(PartFilter q) {
        q.requiredFlags = 4;
        if (const auto* p = first(q)) {
            return p;
        }
        q.requiredFlags.reset();
        if (const auto* p = first(q)) {
            return p;
        }
        if (age_ == kYoungAdult) {
            q.age = kAdult;
        }
        q.category = kSkinCategory;
        const CatalogPart* last = nullptr;
        for (const auto* p : filterParts(catalog_, q, SIZE_MAX)) {
            if (p->fitness == was_.fitness) {
                return p;
            }
            if (p->fitness == 0) {
                last = p;
            }
        }
        return last;
    }

    // A hair of a family: the same family first, then without flags 1 and 8 and any family, then
    // the family in any hair colour, then any hair at all.
    const CatalogPart* sameFamily(PartFilter q) {
        if (const auto* p = first(q)) {
            return p;
        }
        if (age_ == kYoungAdult) {
            q.age = kAdult;
            if (const auto* p = first(q)) {
                return p;
            }
        }
        const auto family = q.family;
        q.excludedFlags = q.excludedFlags.value_or(0) | 9;
        q.family.reset();
        if (const auto* p = first(q)) {
            return p;
        }
        q.family = family;
        q.hairTone.reset();
        if (const auto* p = first(q)) {
            return p;
        }
        q.family.reset();
        return first(q);
    }

    // Every outfit but the bare skin gets the new age's version of each of its old hairs.
    void hairs() {
        for (int i = 0; i < kCategories; ++i) {
            const std::uint32_t category = 1u << i;
            if (category == kSkinCategory) {
                continue;
            }
            n_.lists.erase(listKey(category, kHair));
            const auto old = a_.lists.find(listKey(category, kHair));
            if (old == a_.lists.end()) {
                continue;
            }
            for (const auto& hair : old->second) {
                if (const auto key = convert(hair, category)) {
                    n_.lists[listKey(category, kHair)].push_back(*key);
                }
            }
        }
    }

    // The old clothes go, and two rounds of new ones are drawn: first among parts of flag 4, then
    // among all but those of flags 1 or 8. A part the second round finds replaces the first's.
    void clothes() {
        for (int i = 0; i < kCategories; ++i) {
            const std::uint32_t c = 1u << i;
            if (c == kSkinCategory) {
                continue;
            }
            for (const std::uint32_t part : {kTop, kBody, kBottom}) {
                n_.lists.erase(listKey(c, part));
            }
        }
        fill(4, 0);
        fill(-1, 9);
    }

    // A PlantSim's leaf hair and plant body: kept when they fit the new age, else the catalog's
    // first of the same family.
    void plantParts() {
        const Looks now = looksOf(n_);
        for (const std::uint32_t part : {kHair, kBody}) {
            const std::uint32_t key = listKey(kPlantCategory, part);
            n_.lists.erase(key);
            const auto old = a_.lists.find(key);
            if (old == a_.lists.end()) {
                continue;
            }
            for (const auto& link : old->second) {
                const CatalogPart* p = findPart(catalog_, link);
                if (p == nullptr) {
                    continue;
                }
                if (p->age & age_) {
                    n_.lists[key].push_back(link);
                    continue;
                }
                PartFilter q;
                q.parts = part;
                q.species = now.species;
                q.age = age_;
                q.gender = now.gender;
                if (!none(now.skin)) {
                    q.skinTone = now.skin;
                }
                if (!none(now.hair)) {
                    q.hairTone = now.hair;
                }
                q.category = kPlantCategory;
                q.family = p->family;
                if (const auto* swap = first(q)) {
                    n_.lists[key].push_back(swap->key);
                }
            }
        }
    }

    const AgeData& a_;
    std::uint32_t age_;
    const std::vector<CatalogPart>& own_;
    const Looks was_;
    struct LinkLess {
        bool operator()(const sg::Link& a, const sg::Link& b) const {
            return std::tie(a.type, a.group, a.instance, a.instanceHigh) < std::tie(b.type, b.group, b.instance, b.instanceHigh);
        }
    };
    std::map<sg::Link, sg::Link, LinkLess> converted_;
};

// Lock bits of a Sim's age data ("validparts"): a set bit keeps that look as it is, a clear one has
// it drawn.
constexpr std::uint32_t kLockSkin = 0x01, kLockHairColour = 0x02, kLockHair = 0x04, kLockFace = 0x08, kLockEyes = 0x10,
                        kLockOutfits = 0x20, kLockFitness = 0x40, kLockAge = 0x80, kLockGender = 0x100, kLockSpecies = 0x200,
                        kLockPlant = 0x400, kLockLocks = 0x4000;
constexpr const char* kNoGuid = "00000000-0000-0000-0000-000000000000";
// What the catalog falls back to when it has no skin or hair tone for the Sim.
constexpr const char* kFallbackSkin = "b9a94827-7544-450c-a8f4-6f643ae89a71";
constexpr const char* kFallbackHair = "00000001-0000-0000-0000-000000000000";
// Face overlay kinds the random face draws.
constexpr std::uint32_t kFacialHair = 0, kEyebrows = 1, kEyes = 3, kGlassesOverlay = 5;

// Whether a catalog entry of these flags is kept by a draw for one of `wanted` (any for -1) and none
// of `unwanted`. A draw for neither keeps everything.
bool keptBy(std::uint32_t flags, std::int64_t wanted, std::uint32_t unwanted) {
    if (wanted == 0 && unwanted == 0) {
        return true;
    }
    return (wanted == -1 || (flags & static_cast<std::uint32_t>(wanted))) && !(flags & unwanted);
}

// An age data's lock, 0xFF meaning every bit of the low word, as the game reads it.
std::uint32_t lockOf(const AgeData& d) {
    if (!d.properties.contains("validparts")) {
        return 0xFFFF;
    }
    const std::uint32_t lock = propset::number(d.properties, "validparts");
    return lock == 0xFF ? 0xFFFF : lock;
}

// What an outfit's lock names is put on. Its tops, bottoms and bodies replace the Sim's in each
// outfit it has them for (a top or bottom takes the body off, a body takes both off), and its
// PlantSim lists replace the Sim's outright.
void putOnOutfit(AgeData& data, const AgeData& outfit, std::vector<std::string>& problems) {
    const std::uint32_t lock = lockOf(outfit) | kLockSpecies | kLockLocks;
    const auto copy = [&](const char* name) {
        if (const auto it = outfit.properties.find(name); it != outfit.properties.end()) {
            data.properties[name] = it->second;
        }
    };
    if (lock & kLockAge) {
        copy("age");
        copy("stretch");
    }
    if (lock & kLockGender) {
        copy("gender");
    }
    if (lock & kLockFitness) {
        copy("fitness");
    }
    if (lock & (kLockSkin | kLockHairColour | kLockHair | kLockFace | kLockEyes)) {
        problems.push_back("an outfit's skin, hair or face isn't put on yet");
    }
    const auto listOf = [&](std::uint32_t key) -> const std::vector<sg::Link>* {
        const auto it = outfit.lists.find(key);
        return it != outfit.lists.end() && !it->second.empty() ? &it->second : nullptr;
    };
    if (lock & kLockPlant) {
        for (const std::uint32_t part : {kBody, kBottom, kTop, kHair}) {
            data.lists.erase(listKey(kPlantCategory, part));
            if (const auto* list = listOf(listKey(kPlantCategory, part))) {
                data.lists[listKey(kPlantCategory, part)] = *list;
            }
        }
    }
    if (lock & kLockOutfits) {
        for (const std::uint32_t part : {kTop, kBottom, kBody}) {
            for (int i = 0; i < kCategories; ++i) {
                const std::uint32_t c = 1u << i;
                const auto* list = (c & (kSkinCategory | kPlantCategory)) ? nullptr : listOf(listKey(c, part));
                if (list == nullptr) {
                    continue;
                }
                data.lists.erase(listKey(c, kBody));
                if (part == kBody) {
                    data.lists.erase(listKey(c, kTop));
                    data.lists.erase(listKey(c, kBottom));
                }
                data.lists[listKey(c, part)] = *list;
            }
        }
    }
}

class MadeLookDraws : CatalogDraws {
public:
    MadeLookDraws(const NewCharacter& who, const std::vector<AgeData>& outfits, const OutfitCatalog& catalog, OutfitRandom& random)
        : CatalogDraws(catalog, random, who.age == kYoungAdult ? kAdult : who.age), who_(who), outfits_(outfits) {}

    MadeLook run() {
        n_.properties["age"] = std::to_string(who_.age);
        n_.properties["gender"] = std::to_string(who_.gender);
        n_.properties["species"] = std::to_string(kHuman);
        n_.properties["fitness"] = "0";
        n_.properties["stretch"] = who_.age == kTeen ? "0.94" : "1";
        for (const char* tone : {"skincolor", "haircolor", "eyecolor"}) {
            n_.properties[tone] = kNoGuid;
        }
        draw(~kLockOutfits, 4, 0);
        // An outfit of exactly the Sim's kind that brings its own face keeps the face from being drawn.
        std::uint32_t lock = ~(kLockHairColour | kLockHair | kLockFace | kLockEyes | kLockOutfits);
        bool outfitFace = false;
        for (const auto& outfit : outfits_) {
            if (fits(outfit) && (lockOf(outfit) & kLockFace)) {
                lock |= kLockFace;
                outfitFace = true;
                break;
            }
        }
        draw(lock, -1, 9);
        if (who_.skin >= 1 && who_.skin <= 4) {
            n_.properties["skincolor"] = "0000000" + std::to_string(who_.skin) + "-0000-0000-0000-000000000000";
        } else {
            draw(~kLockSkin, -1, 9);
        }
        for (const auto& outfit : outfits_) {
            if (fits(outfit)) {
                putOn(outfit);
            }
        }
        if (outfitFace) {
            look_.problems.push_back("the face an outfit brings isn't made yet");
        }
        defaultOutfits(n_);
        look_.data = std::move(n_);
        return std::move(look_);
    }

private:
    bool fits(const AgeData& outfit) const {
        const Looks o = looksOf(outfit);
        return o.age == who_.age && o.gender == who_.gender && o.species == kHuman;
    }

    // A tone list as the catalog gives it for the Sim, kept by the draw's flags.
    std::vector<const CatalogPart*> tones(const std::vector<CatalogPart>& from, std::int64_t wanted, std::uint32_t unwanted) const {
        std::vector<const CatalogPart*> kept;
        for (const auto& t : from) {
            if ((drawAge_ & t.age) && (who_.gender & t.gender) && (kHuman & t.species) && keptBy(t.flags, wanted, unwanted)) {
                kept.push_back(&t);
            }
        }
        return kept;
    }

    const CatalogPart* pick(const std::vector<const CatalogPart*>& from) {
        return from.empty() ? nullptr : from[random_.below(static_cast<std::uint32_t>(from.size()))];
    }

    // One pass of the game's random fill: everything `lock` leaves clear is drawn, among catalog
    // entries with one of `wanted` flags and none of `unwanted`. The bare skin outfit is put on
    // afresh each pass.
    void draw(std::uint32_t lock, std::int64_t wanted, std::uint32_t unwanted) {
        if (!(lock & kLockSkin)) {
            const auto* t = pick(tones(catalog_.skinTones, wanted, unwanted));
            n_.properties["skincolor"] = t != nullptr ? t->family : std::string(kFallbackSkin);
        }
        if (!(lock & kLockHairColour)) {
            const auto* t = pick(tones(catalog_.hairTones, wanted, unwanted));
            n_.properties["haircolor"] = t != nullptr ? t->family : std::string(kFallbackHair);
        }
        defaultOutfits(n_);
        if (!(lock & kLockHair)) {
            hair(wanted, unwanted);
        }
        if (!(lock & kLockFace)) {
            face(wanted, unwanted);
        }
        if (!(lock & kLockOutfits)) {
            fill(wanted, unwanted);
        }
    }

    // One everyday hair, worn in every outfit but the skin and the PlantSim's. Where an outfit keeps
    // hairs of flag 2 out, that hair's family member without the flag is worn there instead.
    void hair(std::int64_t wanted, std::uint32_t unwanted) {
        const auto* p = pick(candidates(kHair, 1, wanted, unwanted));
        if (p == nullptr) {
            return;
        }
        for (int i = 0; i < kCategories; ++i) {
            const std::uint32_t c = 1u << i;
            if (c & (kSkinCategory | kPlantCategory)) {
                continue;
            }
            n_.lists.erase(listKey(c, kHair));
            sg::Link key = p->key;
            if ((p->flags & 2) && (categoryFlags(c) & 1)) {
                for (const auto* other : candidates(kHair, c, 0, 0)) {
                    if (other->family == p->family && !(other->flags & 2)) {
                        key = other->key;
                        break;
                    }
                }
            }
            n_.lists[listKey(c, kHair)].push_back(key);
        }
    }

    // The face overlays and the face template, drawn among those that fit the Sim's skin and hair.
    void face(std::int64_t wanted, std::uint32_t unwanted) {
        const Looks now = looksOf(n_);
        const std::string skin = catalogGuid(now.skin), hairTone = catalogGuid(now.hair);
        const auto toneFits = [](const std::string& own, const std::string& asked) { return noTone(own) || noTone(asked) || own == asked; };
        std::map<std::uint32_t, std::vector<const CatalogPart*>> kinds;
        for (const auto& o : catalog_.faceOverlays) {
            if ((drawAge_ & o.age) && (who_.gender & o.gender) && (kHuman & o.species) && toneFits(o.skinTone, skin) &&
                toneFits(o.hairTone, hairTone) && (o.category == 0 || (o.category & 1)) && keptBy(o.flags, wanted, unwanted)) {
                kinds[o.subtype].push_back(&o);
            }
        }
        if (const auto* brows = pick(kinds[kEyebrows])) {
            look_.faceOverlays.push_back(brows->key);
        }
        if (const auto* eyes = pick(kinds[kEyes])) {
            n_.properties["eyecolor"] = eyes->family;
            look_.faceOverlays.push_back(eyes->key);
        }
        if (who_.gender == 2 && drawAge_ != 1 && drawAge_ != 2 && !kinds[kFacialHair].empty() && random_.below(100) > 25) {
            look_.faceOverlays.push_back(pick(kinds[kFacialHair])->key);
        }
        if (!kinds[kGlassesOverlay].empty() && random_.below(100) > 90) {
            look_.faceOverlays.push_back(pick(kinds[kGlassesOverlay])->key);
        }
        std::vector<const CatalogPart*> templates;
        for (const auto& t : catalog_.faceTemplates) {
            if ((drawAge_ & t.age) && (who_.gender & t.gender) && (kHuman & t.species) && keptBy(t.flags, wanted, unwanted)) {
                templates.push_back(&t);
            }
        }
        if (const auto* t = pick(templates)) {
            look_.faceFamily = t->family;
        }
    }

    void putOn(const AgeData& outfit) { putOnOutfit(n_, outfit, look_.problems); }

    const NewCharacter& who_;
    const std::vector<AgeData>& outfits_;
    MadeLook look_;
};

} // namespace

AgeData readAgeData(const std::map<std::string, std::string>& file, const std::vector<sg::Link>& references) {
    AgeData data;
    for (const auto& [name, value] : file) {
        if (!isList(name)) {
            data.properties[name] = value;
        }
    }
    const bool newer = file.contains("listcntStd");
    const std::string count = newer ? "listcntStd" : "listcnt", key = newer ? "lks" : "lk", size = newer ? "lss" : "ls",
                      entry = newer ? "les" : "le";
    // A broken file could claim billions of lists or entries. Each is a property of its own, so all
    // the lists together can't hold more entries than the file has properties.
    std::uint32_t budget = static_cast<std::uint32_t>(std::min<std::size_t>(file.size(), UINT32_MAX));
    const std::uint32_t lists = std::min(propset::number(file, count), budget);
    for (std::uint32_t i = 0; i < lists; ++i) {
        const std::uint32_t k = propset::number(file, key + std::to_string(i));
        const std::uint32_t entries = std::min(propset::number(file, size + std::to_string(i)), budget);
        budget -= entries;
        auto& list = data.lists[k];
        for (std::uint32_t j = 0; j < entries; ++j) {
            const std::uint32_t index = propset::number(file, entry + std::to_string(i) + "_" + std::to_string(j));
            if (index < references.size()) {
                list.push_back(references[index]);
            }
        }
    }
    if (const std::uint32_t s = propset::number(file, "skeletonkeyidx"); file.contains("skeletonkeyidx") && s < references.size()) {
        data.skeleton = references[s];
    }
    return data;
}

std::vector<CatalogPart> characterParts(const std::filesystem::path& characterFile) {
    constexpr std::uint32_t kPartProperties = 0xEBCF3E27;
    content::ResourceIndex file;
    std::vector<CatalogPart> parts;
    file.addPackage(characterFile);
    for (const auto& r : file.resources()) {
        const auto& k = r.entry.key;
        if (k.type == kPartProperties) {
            parts.push_back(catalogPart({k.type, k.group, k.instance, k.instanceHigh}, propset::parse(file.read(r))));
        }
    }
    return parts;
}

std::uint32_t OutfitRandom::below(std::uint32_t n) {
    if (n == 0) {
        return 0;
    }
    const std::uint64_t p = static_cast<std::uint64_t>(state_) * 0x41C64E6Du + 0x3039u;
    state_ = static_cast<std::uint32_t>(p);
    const auto draw = static_cast<std::uint32_t>(p >> 16);
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(draw) * n) >> 32);
}

std::optional<AgeData> ageUp(const AgeData& old, std::uint32_t age, const std::vector<CatalogPart>& ownParts,
                             const OutfitCatalog& catalog, OutfitRandom& random) {
    return AgeUp(old, age, ownParts, catalog, random).run();
}

MadeLook makeLook(const NewCharacter& who, const std::vector<AgeData>& outfits, const OutfitCatalog& catalog, OutfitRandom& random) {
    return MadeLookDraws(who, outfits, catalog, random).run();
}

const AgeData* pickOutfit(const std::vector<AgeData>& outfits, std::uint32_t age, std::uint32_t gender, std::uint32_t simGuid) {
    std::vector<const AgeData*> fitting;
    for (const auto& outfit : outfits) {
        const std::uint32_t a = propset::number(outfit.properties, "age"), g = propset::number(outfit.properties, "gender");
        if ((a == 0 || (a & age)) && (g == 0 || (g & gender))) {
            fitting.push_back(&outfit);
        }
    }
    if (fitting.size() <= 1) {
        return fitting.empty() ? nullptr : fitting.front();
    }
    OutfitRandom draw(simGuid);
    return fitting[draw.below(static_cast<std::uint32_t>(fitting.size()))];
}

std::vector<std::string> putOn(AgeData& data, const AgeData& outfit) {
    std::vector<std::string> problems;
    putOnOutfit(data, outfit, problems);
    return problems;
}

} // namespace ts2::scene
