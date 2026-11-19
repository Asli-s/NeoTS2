#include "sim/genetics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace ts2::sim {

namespace {

constexpr std::uint32_t kRecessiveKey = 0x10000000;
constexpr std::uint32_t kGeneCount = 15;

std::uint32_t draw(SimRandom& random, std::uint32_t n) {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(random()) * n) >> 32);
}

bool coin(SimRandom& random) {
    return draw(random, 100) < 50;
}

bool sameText(const std::string& x, const std::string& y) {
    return std::equal(x.begin(), x.end(), y.begin(), y.end(),
                      [](unsigned char p, unsigned char q) { return std::tolower(p) == std::tolower(q); });
}

Gene geneOf(const Genome& genome, std::uint32_t number) {
    const auto it = genome.find(number);
    return it != genome.end() ? it->second : Gene{};
}

// One allele of a parent's gene: its dominant or its recessive value, half the time each.
std::string allele(const Gene& gene, SimRandom& random) {
    return coin(random) ? gene.dominant : gene.recessive;
}

// Two alleles, the more dominant colour first. A tie or a colour the catalog lacks leads by a coin.
Gene byDominance(const std::string& a, const std::string& b, const std::vector<content::GeneticColour>& colours, SimRandom& random) {
    const auto* ca = content::findColour(colours, a);
    const auto* cb = content::findColour(colours, b);
    if (ca != nullptr && cb != nullptr && ca->genetic != cb->genetic) {
        return ca->genetic < cb->genetic ? Gene{a, b} : Gene{b, a};
    }
    return coin(random) ? Gene{a, b} : Gene{b, a};
}

// Gene 6 of a parent, a missing value taken from its gene 2.
Gene skinGene(const Genome& parent) {
    Gene skin = geneOf(parent, kSkinGene);
    const Gene alleles = geneOf(parent, kSkinAllelesGene);
    if (skin.dominant.empty()) {
        skin.dominant = alleles.dominant;
    }
    if (skin.recessive.empty()) {
        skin.recessive = alleles.recessive;
    }
    return skin;
}

void inheritSkin(const Genome& a, const Genome& b, const content::GeneticsCatalog& catalog, SimRandom& random, Genome& child) {
    const Gene skinA = skinGene(a), skinB = skinGene(b);
    const std::string fromA = allele(skinA, random);
    const std::string fromB = allele(skinB, random);
    const auto* ca = content::findColour(catalog.skins, fromA);
    const auto* cb = content::findColour(catalog.skins, fromB);
    if (ca == nullptr || cb == nullptr) {
        child[kSkinAllelesGene] = coin(random) ? Gene{fromA, fromB} : Gene{fromB, fromA};
        return;
    }
    if (ca->genetic == 0 || cb->genetic == 0) {
        // A parent of one unblended skin leads.
        const bool leadsA = sameText(skinA.dominant, skinA.recessive) && ca->genetic == 0;
        const bool leadsB = sameText(skinB.dominant, skinB.recessive) && cb->genetic == 0;
        const bool aFirst = leadsA != leadsB ? leadsA : coin(random);
        const Gene pair = aFirst ? Gene{fromA, fromB} : Gene{fromB, fromA};
        child[kSkinAllelesGene] = pair;
        child[kSkinGene] = pair;
        return;
    }
    const float lo = std::min(ca->genetic, cb->genetic), hi = std::max(ca->genetic, cb->genetic);
    std::vector<const content::GeneticColour*> between;
    for (const auto& c : catalog.skins) {
        if (c.genetic >= lo && c.genetic <= hi) {
            between.push_back(&c);
        }
    }
    // The parents' own skins are always between (the catalog keeps only values that are numbers),
    // so the list is never empty.
    const auto* shown = between[draw(random, static_cast<std::uint32_t>(between.size()))];
    child[kSkinAllelesGene] = Gene{fromA, fromB};
    child[kSkinGene] = Gene{shown->guid, shown->guid};
}

void inheritFace(const Genome& a, const Genome& b, SimRandom& random, Genome& child, FacePicks* picks) {
    static constexpr std::array<const char*, 6> kRegions{"nose", "brow", "mouth", "eyes", "jaw", "ears"};
    const std::string dominantA = geneOf(a, kFaceGene).dominant, dominantB = geneOf(b, kFaceGene).dominant;
    std::string kept;
    for (const char* region : kRegions) {
        const bool inA = dominantA.find(region) != std::string::npos;
        const bool inB = dominantB.find(region) != std::string::npos;
        const std::uint32_t r = draw(random, 100);
        const bool fromB = !inA && inB ? r <= 66 : r >= 50;
        if (picks != nullptr) {
            picks->push_back({region, !fromB});
            // The eyes take the cheekbones with them, and the mouth the cheeks.
            if (std::string_view(region) == "eyes") {
                picks->push_back({"cheekbones", !fromB});
            } else if (std::string_view(region) == "mouth") {
                picks->push_back({"cheek", !fromB});
            }
        }
        if (fromB ? inB : inA) {
            kept += region;
            kept += ',';
        }
    }
    child[kFaceGene] = Gene{kept, ""};
}

} // namespace

Genome genomeFromProperties(const std::map<std::string, std::string>& properties) {
    Genome genome;
    for (const auto& [key, value] : properties) {
        char* end = nullptr;
        const unsigned long number = std::strtoul(key.c_str(), &end, 10);
        if (end == key.c_str() || *end != '\0') {
            continue;
        }
        if (number >= kRecessiveKey && number - kRecessiveKey <= kGeneCount) {
            genome[static_cast<std::uint32_t>(number - kRecessiveKey)].recessive = value;
        } else if (number <= kGeneCount) {
            genome[static_cast<std::uint32_t>(number)].dominant = value;
        }
    }
    return genome;
}

Genome childGenome(const Genome& a, const Genome& b, const content::GeneticsCatalog& catalog, SimRandom& random,
                   FacePicks* facePicks) {
    Genome child;
    for (std::uint32_t i = 1; i <= kGeneCount; ++i) {
        if (!a.contains(i) && !b.contains(i)) {
            continue;
        }
        const std::string fromA = allele(geneOf(a, i), random);
        const std::string fromB = allele(geneOf(b, i), random);
        switch (i) {
        case kHairGene: child[i] = byDominance(fromA, fromB, catalog.hairs, random); break;
        case kEyeGene: child[i] = byDominance(fromA, fromB, catalog.eyes, random); break;
        case kSkinAllelesGene: inheritSkin(a, b, catalog, random, child); break;
        case 4:
        case kFaceGene:
        case kSkinGene:
        case 11: break;
        default: child[i] = coin(random) ? Gene{fromA, fromB} : Gene{fromB, fromA}; break;
        }
    }
    inheritFace(a, b, random, child, facePicks);
    return child;
}

Personality childPersonality(const std::function<std::int16_t(bool fromA, std::size_t index)>& parentValue, SimRandom& random, bool baby) {
    constexpr std::int32_t kTraitMax = 1000, kPoint = 100, kMiddleLow = 400, kMiddleHigh = 600, kFirstBudget = 2500;
    std::vector<std::size_t> left(kPersonalityTraits.begin(), kPersonalityTraits.end());
    std::vector<std::size_t> order;
    while (!left.empty()) {
        const std::uint32_t at = draw(random, static_cast<std::uint32_t>(left.size()));
        order.push_back(left[at]);
        left.erase(left.begin() + at);
    }
    const std::int32_t budget = baby ? 3500 : kFirstBudget;
    std::int32_t points = budget;
    Personality out;
    for (const std::size_t trait : order) {
        std::int32_t value = 0;
        switch (draw(random, 3)) {
        case 0: value = parentValue(true, trait); break;
        case 1: value = parentValue(false, trait); break;
        default: value = (static_cast<std::int32_t>(draw(random, 10)) + 1) * kPoint; break;
        }
        value = std::min<std::int32_t>(static_cast<std::int16_t>(value), points);
        out[trait] = static_cast<std::int16_t>(value);
        points -= static_cast<std::int16_t>(value);
    }
    // Each trait raised takes points, so this ends unless every trait is already full. The cap only
    // guards a broken parent value.
    const auto raise = [&](std::size_t trait) {
        const std::int32_t was = out[trait];
        const std::int32_t step = (static_cast<std::int32_t>(draw(random, static_cast<std::uint32_t>(std::max(points / kPoint, 0)))) + 1) * kPoint;
        const std::int16_t now = static_cast<std::int16_t>(std::min<std::int32_t>(static_cast<std::int16_t>(was + step), kTraitMax));
        out[trait] = now;
        points += was - now;
    };
    constexpr int kMostRounds = 64;
    for (int round = 0; round < kMostRounds && points > budget - kFirstBudget; ++round) {
        bool raised = false;
        for (std::size_t i = 0; i < order.size() && points > 0; ++i) {
            if (out[order[i]] >= kMiddleLow && out[order[i]] <= kMiddleHigh) {
                raise(order[i]);
                raised = true;
            }
        }
        for (std::size_t i = 0; !raised && i < order.size() && points > 0; ++i) {
            raise(order[i]);
        }
    }
    return out;
}

namespace {

// Each sign's traits in kPersonalityTraits order, in points.
constexpr std::array<std::array<float, 5>, 12> kSigns{{
    {8, 3, 3, 5, 6}, {5, 4, 8, 5, 3}, {7, 3, 3, 4, 8}, {3, 6, 4, 6, 6}, {10, 3, 4, 4, 4}, {2, 5, 3, 9, 6},
    {8, 7, 6, 2, 2}, {5, 3, 3, 6, 8}, {3, 4, 7, 2, 9}, {4, 5, 8, 7, 1}, {4, 6, 7, 4, 4}, {3, 7, 3, 5, 7},
}};

bool validSign(std::int16_t sign) { return sign >= 1 && sign <= 12; }

} // namespace

Personality signPersonality(std::int16_t sign) {
    Personality out;
    if (!validSign(sign)) {
        return out;
    }
    for (std::size_t i = 0; i < kPersonalityTraits.size(); ++i) {
        out[kPersonalityTraits[i]] = static_cast<std::int16_t>(std::lround(kSigns[static_cast<std::size_t>(sign - 1)][i] * 100.0f));
    }
    return out;
}

std::int16_t signAspiration(std::int16_t sign, std::int16_t lifeStage) {
    // Life stages up to 15 are babies, toddlers and children.
    constexpr std::int16_t kOldestChild = 15, kGrowUp = 0x40;
    static constexpr std::array<std::int16_t, 12> kBySign{16, 1, 4, 2, 16, 32, 1, 4, 32, 4, 2, 32};
    if (lifeStage <= kOldestChild) {
        return kGrowUp;
    }
    return validSign(sign) ? kBySign[static_cast<std::size_t>(sign - 1)] : std::int16_t{0};
}

std::int16_t signPetValue(std::int16_t sign) {
    static constexpr std::array<std::int16_t, 12> kBySign{0x61, 0x21A, 0x144, 0x90, 0x168, 0x211, 0x12A, 0x55, 0x186, 0x209, 0xA2, 0x284};
    return validSign(sign) ? kBySign[static_cast<std::size_t>(sign - 1)] : std::int16_t{0};
}

std::string fillMadeSimPersonData(std::span<std::int16_t> personData, std::uint32_t guid, SimRandom& random) {
    constexpr std::size_t kZodiac = 0x46, kGuidLow = 0x58, kAspiration = 0x2E, kTraitCopy = 0x2F, kPetValue = 0xC7,
                          kLifeStage = 0x3A, kSpecies = 0xBA, kGender = 0x41;
    if (personData.size() <= kPetValue) {
        return {};
    }
    const auto draw = [&](std::uint32_t n) { return static_cast<std::uint32_t>((static_cast<std::uint64_t>(random()) * n) >> 32); };
    const auto sign = static_cast<std::int16_t>(draw(12) + 1);
    personData[kZodiac] = sign;
    personData[kGuidLow] = static_cast<std::int16_t>(guid & 0xFFFF);
    if (personData[kSpecies] != 0) {
        personData[kPetValue] = signPetValue(sign);
        return {};
    }
    for (const auto& [trait, value] : signPersonality(sign)) {
        personData[trait] = value;
    }
    personData[kAspiration] = signAspiration(sign, personData[kLifeStage]);
    // The copy holds neat, nice, active, outgoing and playful, in that order.
    constexpr std::array<std::size_t, 5> kCopied{7, 2, 3, 6, 5};
    for (std::size_t i = 0; i < kCopied.size(); ++i) {
        personData[kTraitCopy + i] = personData[kCopied[i]];
    }
    // Each region's threshold for a male and a female.
    struct Region {
        const char* name;
        std::uint32_t male, female;
    };
    static constexpr std::array<Region, 8> kRegions{{{"nose", 66, 50}, {"brow", 66, 50}, {"mouth", 50, 66}, {"cheek", 50, 50},
                                                     {"cheekbones", 50, 66}, {"eyes", 50, 66}, {"jaw", 66, 50}, {"ears", 50, 50}}};
    const bool female = personData[kGender] == 1;
    std::string regions;
    for (const Region& region : kRegions) {
        if (draw(100) >= (female ? region.female : region.male)) {
            regions += region.name;
            regions += ',';
        }
    }
    return regions;
}

std::int16_t zodiacSign(const Personality& personality) {
    std::array<float, 5> points{};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto it = personality.find(kPersonalityTraits[i]);
        points[i] = static_cast<float>(it != personality.end() ? it->second : 0) * 0.01f;
    }
    std::int16_t nearest = 1;
    float best = 1e38f;
    for (std::size_t sign = 0; sign < kSigns.size(); ++sign) {
        float distance = 0;
        for (std::size_t i = 0; i < points.size(); ++i) {
            distance += (points[i] - kSigns[sign][i]) * (points[i] - kSigns[sign][i]);
        }
        if (distance < best) {
            best = distance;
            nearest = static_cast<std::int16_t>(sign + 1);
        }
    }
    return nearest;
}

} // namespace ts2::sim
