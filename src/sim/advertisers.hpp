#pragma once

#include "formats/ttab.hpp"
#include "sim/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::sim {

class CodeSource;

// The age kinds interaction tables advertise to: 0 adult, 1 child, 2 toddler, 3 teen, 4 elder, then
// the pets: 5 large dog, 6 elder large dog, 7 small dog, 8 elder small dog, 9 cat, 10 elder cat,
// 11 puppy, 12 kitten.
constexpr std::size_t kAdvertisedKindCount = 13;

// Whether an interaction advertises a motive to an age kind, for the advertiser list: humans read
// the kind's minimum and range (either not 0, whatever the sign or type). Pets read any non-zero
// value in the kind's list. Motives 2 and 10-12 are never advertised. Motives 1 and 4 can be,
// although their human advertisements score nothing.
bool advertisesTo(const ttab::Interaction& interaction, std::size_t kind, std::size_t motive);

// An object that advertises a motive, and the lines of its interaction table that do.
struct Advertiser {
    std::int16_t object = 0;
    std::vector<std::uint16_t> lines;
};

// Which objects advertise each motive to each age kind, as autonomy looks them up. Objects whose
// table has no line autonomy may pick (autonomy level 100 or more, or a debug line), objects of
// definition type 7 and objects whose definition field 8 is above 0 advertise nothing. Each part
// of a multi-tile object advertises on its own.
class Advertisers {
public:
    // Only for `kind` when given (the rest stay empty).
    static Advertisers build(World& world, CodeSource& code, std::optional<std::size_t> kind = std::nullopt);

    std::span<const Advertiser> advertising(std::size_t kind, std::size_t motive) const;
    // The number of objects advertising each motive to `kind`.
    std::array<std::size_t, kMotiveCount> counts(std::size_t kind) const;

private:
    std::array<std::array<std::vector<Advertiser>, kMotiveCount>, kAdvertisedKindCount> lists_;
};

// The motives autonomy looks for interactions for next, as a mask (bit n = motive n): the `count`
// lowest of the Sim's motives that some object advertises (`advertisers`, objects per motive) and
// that are not in `used`. Humans choose from motives 0, 5-9 and 13-15, pets from 0, 1, 5-9, 14 and
// 15. Equal motives go lowest number first.
std::uint16_t worstMotives(const std::array<float, kMotiveCount>& motives, bool human,
                           const std::array<std::size_t, kMotiveCount>& advertisers, std::uint16_t used,
                           std::size_t count = 3);

} // namespace ts2::sim
