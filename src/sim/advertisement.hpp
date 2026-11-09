#pragma once

#include "formats/ttab.hpp"
#include "sim/world.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace ts2::sim {

// The first of the game's pet age kinds. Kinds 0-4 are the human age groups.
constexpr std::size_t kFirstPetKind = 5;

// A pet's age kind: large dogs 5 (adult) and 6 (elder), small dogs 7 and 8, cats 9 and 10, puppies
// 11 and kittens 12 (age 18 or less). Nothing for humans or an unknown species.
std::optional<std::size_t> petKind(const SimObject& sim);

// The age group whose motive advertisements a human Sim reads from an interaction table, by age
// (person data 0x3A): 0 adult (over 18, and young adults: person data 0xAF), 1 child, 2 toddler,
// 3 teen, 4 elder. Babies read the adults'.
std::size_t advertisementGroup(const SimObject& sim);

// How much an interaction advertises it changes a Sim's motive, in motive points. Human type 0 gives
// (minimum + range) / 1000, other types scale the range by a trait or interest (person data, 0-1000,
// some inverted): (minimum + range * value / 1000) / 1000. A pet adds up one value per record of its
// kind's list, picked by its personality (person data 0xC7), in thousandths. Motives 1, 2, 4 and
// 10-12 advertise nothing to humans, 2, 4 and 10-13 nothing to pets, and missing entries nothing.
float advertisedChange(const ttab::Interaction& interaction, const SimObject& sim, std::size_t motive);

// Scales the advertisements a check tree is testing, as the script function MultiplyAdValues does:
// motive 0-15 alone, or any other value for every motive but 2, 4 and 10-12. A human Sim's minimum
// in its age group is scaled. A pet's value for its personality in its species and age group is
// scaled. Entries the line doesn't have stay missing, and a multiplier of 1 changes nothing.
void multiplyAdvertisements(ttab::Interaction& line, const SimObject& sim, float multiplier, std::int32_t motive);

// The motive levels after an interaction's advertised changes.
std::array<float, kMotiveCount> advertisedMotives(const ttab::Interaction& interaction, const SimObject& sim,
                                        const std::array<float, kMotiveCount>& motives);

} // namespace ts2::sim
