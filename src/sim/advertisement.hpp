#pragma once

#include "formats/ttab.hpp"
#include "sim/world.hpp"

#include <array>
#include <cstddef>

namespace ts2::sim {

// The age group whose motive advertisements a human Sim reads from an interaction table, by age
// (person data 0x3A): 0 adult (over 18, and young adults: person data 0xAF), 1 child, 2 toddler,
// 3 teen, 4 elder. Babies read the adults'.
std::size_t advertisementGroup(const SimObject& sim);

// How much an interaction advertises it changes one of a human Sim's motives, in motive points, for
// autonomy. Each entry has a minimum, a range and a type: type 0 advertises (minimum + range) / 1000.
// Any other type scales the range by one of the Sim's personality traits or interests (person data,
// 0-1000, some types inverted): (minimum + range * value / 1000) / 1000. Motives 1, 2, 4 and 10-12,
// and entries the table doesn't have, advertise nothing.
float advertisedChange(const ttab::Interaction& interaction, const SimObject& sim, std::size_t motive);

// The motive levels after an interaction's advertised changes.
std::array<float, kMotiveCount> advertisedMotives(const ttab::Interaction& interaction, const SimObject& sim,
                                        const std::array<float, kMotiveCount>& motives);

} // namespace ts2::sim
