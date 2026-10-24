#include "sim/advertisers.hpp"

#include "sim/code_source.hpp"
#include "sim/pie_menu.hpp"

#include <algorithm>

namespace ts2::sim {

namespace {

constexpr std::size_t kHumanKindCount = 5;
constexpr std::uint32_t kDebugLine = 0x80;
constexpr std::uint32_t kNeverAutonomous = 100;
constexpr std::uint16_t kUnadvertisedType = 7;
constexpr std::size_t kNoAdvertisingField = 8;

bool nonZero(const ttab::MotiveEffect& e) {
    return e.min != 0 || e.delta != 0 || e.type != 0;
}

// Whether any line of the table is one autonomy could pick.
bool anyAutonomous(const ttab::InteractionTable& table) {
    for (const auto& line : table.interactions) {
        if (line.autonomy < kNeverAutonomous && (line.flags & kDebugLine) == 0) {
            return true;
        }
    }
    return false;
}

bool mayAdvertise(const SimObject& object) {
    return object.definition.field(objd::field::kType) != kUnadvertisedType &&
           static_cast<std::int16_t>(object.definition.field(kNoAdvertisingField)) <= 0;
}

} // namespace

bool advertisesTo(const ttab::Interaction& interaction, std::size_t kind, std::size_t motive) {
    if (motive >= kMotiveCount || motive == 2 || (motive >= 10 && motive <= 12) || kind >= kAdvertisedKindCount) {
        return false;
    }
    if (kind < kHumanKindCount) {
        if (kind >= interaction.motives.size() || motive >= interaction.motives[kind].size()) {
            return false;
        }
        const ttab::MotiveEffect& e = interaction.motives[kind][motive];
        return e.min != 0 || e.delta != 0;
    }
    const std::size_t pet = kind - kHumanKindCount;
    if (pet >= interaction.moreMotives.size() || motive >= interaction.moreMotives[pet].size()) {
        return false;
    }
    for (const auto& e : interaction.moreMotives[pet][motive]) {
        if (nonZero(e)) {
            return true;
        }
    }
    return false;
}

Advertisers Advertisers::build(World& world, CodeSource& code, std::optional<std::size_t> onlyKind) {
    Advertisers out;
    const auto any = [](const SimObject&) { return true; };
    for (SimObject* object = world.nextAfter(0, any); object != nullptr; object = world.nextAfter(object->id, any)) {
        if (!mayAdvertise(*object)) {
            continue;
        }
        const auto table = interactionsOf(code, *object).table;
        if (!table || !anyAutonomous(*table)) {
            continue;
        }
        for (std::size_t kind = 0; kind < kAdvertisedKindCount; ++kind) {
            if (onlyKind && kind != *onlyKind) {
                continue;
            }
            for (std::size_t motive = 0; motive < kMotiveCount; ++motive) {
                Advertiser found{object->id, {}};
                for (std::size_t i = 0; i < table->interactions.size(); ++i) {
                    if (advertisesTo(table->interactions[i], kind, motive)) {
                        found.lines.push_back(static_cast<std::uint16_t>(i));
                    }
                }
                if (!found.lines.empty()) {
                    out.lists_[kind][motive].push_back(std::move(found));
                }
            }
        }
    }
    return out;
}

std::span<const Advertiser> Advertisers::advertising(std::size_t kind, std::size_t motive) const {
    if (kind >= kAdvertisedKindCount || motive >= kMotiveCount) {
        return {};
    }
    return lists_[kind][motive];
}

std::array<std::size_t, kMotiveCount> Advertisers::counts(std::size_t kind) const {
    std::array<std::size_t, kMotiveCount> out{};
    if (kind < kAdvertisedKindCount) {
        for (std::size_t m = 0; m < kMotiveCount; ++m) {
            out[m] = lists_[kind][m].size();
        }
    }
    return out;
}

std::uint16_t worstMotives(const std::array<float, kMotiveCount>& motives, bool human,
                           const std::array<std::size_t, kMotiveCount>& advertisers, std::uint16_t used,
                           std::size_t count) {
    std::vector<std::size_t> order;
    for (std::size_t m = 0; m < kMotiveCount; ++m) {
        const bool eligible = m == 0 || (m >= 5 && m <= 9) || m == 14 || m == 15 || (human ? m == 13 : m == 1);
        if (eligible) {
            order.push_back(m);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return motives[a] < motives[b]; });
    std::uint16_t mask = 0;
    for (const std::size_t m : order) {
        if (count == 0) {
            break;
        }
        if (advertisers[m] > 0 && (used & (1u << m)) == 0) {
            mask = static_cast<std::uint16_t>(mask | (1u << m));
            --count;
        }
    }
    return mask;
}

} // namespace ts2::sim
