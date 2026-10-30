#pragma once

#include "content/genetics_catalog.hpp"
#include "sim/sim_random.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ts2::sim {

// A gene of a Sim's DNA (SDNA): a dominant and a recessive value, each a catalog GUID or text.
struct Gene {
    std::string dominant, recessive;
    bool operator==(const Gene&) const = default;
};

// Genes by number: 1 hair tone, 2 skin tone alleles, 3 eye colour, 5 face (the dominant value
// lists the face regions passed on dominant, as "nose,mouth,"), 6 skin tone shown. What 4 and 7-15
// hold is not known yet.
using Genome = std::map<std::uint32_t, Gene>;

constexpr std::uint32_t kHairGene = 1, kSkinAllelesGene = 2, kEyeGene = 3, kFaceGene = 5, kSkinGene = 6;

// A Sim's genome from its SDNA properties: key "n" holds gene n's dominant value, n + 0x10000000
// its recessive value. Other keys are left out.
Genome genomeFromProperties(const std::map<std::string, std::string>& properties);

// The genes a human baby inherits from parents `a` (the one who carried it) and `b`, drawn as the
// game draws them on its genetics generator.
// - Every gene 1-15 either parent has: one allele from each parent, its dominant or recessive
//   value half the time each, `a` first.
// - Hair and eyes: the allele whose colour is more dominant (lower "genetic") leads, a tie or an
//   unknown colour leads by a coin.
// - Skin: the alleles come from gene 6. Gene 2 keeps them. Gene 6 shows one random catalog skin
//   whose genetic value lies between theirs, twice. When either value is 0 nothing is blended:
//   a parent whose two skin values match and who passed a 0 skin leads (a coin when both or
//   neither do), and gene 6 is that pair too.
// - Genes 7-15 other than 11 keep both alleles in a random order. Genes 4 and 11 aren't given to the
//   baby here.
// - Face regions nose, brow, mouth, eyes, jaw, ears: a region only `b` passes dominant stays
//   dominant two times in three. Otherwise each parent gives it half the time, dominant when that
//   parent has it dominant. Gene 5 lists the dominant ones. `facePicks` gets which parent gave each.
// The face regions a baby's face is blended from, in the order drawn, each from parent `a` (true)
// or `b`: nose, brow, mouth, cheek, eyes, cheekbones, jaw, ears.
using FacePicks = std::vector<std::pair<std::string, bool>>;

Genome childGenome(const Genome& a, const Genome& b, const content::GeneticsCatalog& catalog, SimRandom& random,
                   FacePicks* facePicks = nullptr);

} // namespace ts2::sim
