#pragma once

#include "content/genetics_catalog.hpp"
#include "sim/sim_random.hpp"

#include <array>
#include <cstdint>
#include <functional>
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

// The face regions a baby's face is blended from, in the order drawn, each from parent `a` (true)
// or `b`: nose, brow, mouth, cheek, eyes, cheekbones, jaw, ears.
using FacePicks = std::vector<std::pair<std::string, bool>>;

// The genes a human baby inherits from `a` (the parent who carried it) and `b`, on the game's
// genetics random generator.
// - Genes 1-15 either parent has: one allele from each, dominant or recessive by a coin, `a` first.
// - Hair and eyes: the more dominant colour (lower "genetic") leads, a tie or unknown by a coin.
// - Skin: gene 2 keeps gene 6's alleles. Gene 6 shows a random catalog skin whose genetic value lies
//   between theirs, drawn twice. When either value is 0 nothing is blended: a parent with matching
//   skin values who passed a 0 skin leads (a coin when both or neither do), and gene 6 is that pair.
// - Genes 7-15 other than 11 keep both alleles in a random order. Genes 4 and 11 aren't given here.
// - Face regions nose, brow, mouth, eyes, jaw, ears: one only `b` passes dominant stays dominant two
//   times in three. Otherwise a coin picks the parent, dominant when theirs is. Gene 5 lists the
//   dominant ones, and `facePicks` gets which parent gave each.
Genome childGenome(const Genome& a, const Genome& b, const content::GeneticsCatalog& catalog, SimRandom& random,
                   FacePicks* facePicks = nullptr);

// A Sim's personality traits by person data index: 6 outgoing, 2 nice, 5 playful, 7 neat and 3
// active, in points times 100 (0 to 1000).
constexpr std::array<std::size_t, 5> kPersonalityTraits{6, 2, 5, 7, 3};
using Personality = std::map<std::size_t, std::int16_t>;

// A baby's personality, drawn right after its genes on the same generator. The traits go in a
// random order, and each takes parent `a`'s value, parent `b`'s or a random 100 to 1000, a third of
// the time each, never more than the points left of the budget (3500 for a baby, 2500 otherwise).
// While more than 1000 points are left (any otherwise), each trait from 400 to 600 gets a random
// part of what is left on top, up to 1000, or when none is in that range every trait does.
// `parentValue(fromA, index)` gives a parent's trait.
Personality childPersonality(const std::function<std::int16_t(bool fromA, std::size_t index)>& parentValue, SimRandom& random, bool baby);

// The zodiac sign (1 to 12) a personality is nearest to, as person data 0x46 holds it.
std::int16_t zodiacSign(const Personality& personality);

} // namespace ts2::sim
