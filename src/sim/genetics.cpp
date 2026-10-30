#include "sim/genetics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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

} // namespace ts2::sim
