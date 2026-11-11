#include "sim/selection.hpp"
#include "sim/clock.hpp"
#include "sim/code_source.hpp"
#include "sim/world.hpp"
#include <algorithm>
#include <cctype>
#include <string>

namespace ts2::sim {
namespace {

constexpr std::size_t kPersonTypeData = 0x20, kLifeStage = 0x3A, kAgeDaysLeft = 0x5B, kAgeDuration = 0x9C,
                      kSelectionFlags = 0x9E, kSpecies = 0xBA;  // person data
constexpr std::int16_t kSelectable = 0x01, kPassedOver = 0x02, kSelectableVisitor = 0x08;

bool passedOver(const SimObject& sim) { return (sim.personData[kSelectionFlags] & kPassedOver) != 0; }

// The game compares these as unsigned numbers, so a negative sum sorts as a large one.
std::uint32_t ageKey(const SimObject& sim) {
    const int key = sim.personData[kLifeStage] * 1000 - sim.personData[kAgeDaysLeft] + sim.personData[kAgeDuration];
    return static_cast<std::uint32_t>(key);
}

// Case-blind, below 0 when `a` sorts first.
int compareNames(const std::string& a, const std::string& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb) {
            return ca - cb;
        }
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

} // namespace

bool isSelectable(const SimObject& sim) {
    const std::int16_t flags = sim.personData[kSelectionFlags];
    return (flags & kSelectable) != 0 || (sim.personData[kPersonTypeData] != 0 && (flags & kSelectableVisitor) != 0);
}

std::vector<std::int16_t> selectionOrder(World& world, CodeSource& code) {
    struct Entry {
        std::int16_t id;
        bool human;
        std::uint32_t age;
        std::string name;
        std::uint32_t guid;
    };
    std::vector<Entry> entries;
    const auto selectable = [](const SimObject& o) { return o.isPerson() && isSelectable(o); };
    for (const SimObject* s = world.nextAfter(0, selectable); s != nullptr; s = world.nextAfter(s->id, selectable)) {
        const SimObject& sim = *s;
        const auto set = static_cast<std::uint16_t>(sim.definition.field(objd::field::kCatalogStringsId));
        entries.push_back({sim.id, sim.personData[kSpecies] == 0, ageKey(sim), code.catalogString(sim.group, set, 0).value_or(""),
                           sim.definition.guid()});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.human != b.human) {
            return a.human;
        }
        if (a.age != b.age) {
            return a.age > b.age;
        }
        if (const int names = compareNames(a.name, b.name); names != 0) {
            return names < 0;
        }
        return a.guid > b.guid;
    });
    std::vector<std::int16_t> order;
    order.reserve(entries.size());
    for (const Entry& e : entries) {
        order.push_back(e.id);
    }
    return order;
}

void keepSelection(World& world, CodeSource& code, std::span<std::int16_t> globals) {
    if (globals.size() <= global::kSelectedPerson) {
        return;
    }
    std::int16_t& selected = globals[global::kSelectedPerson];
    const SimObject* current = selected != 0 ? world.find(selected) : nullptr;
    if (current != nullptr && current->isPerson() && isSelectable(*current) && !passedOver(*current)) {
        return;
    }
    const std::vector<std::int16_t> order = selectionOrder(world, code);
    const auto at = std::find(order.begin(), order.end(), selected);
    const std::size_t start = at != order.end() ? static_cast<std::size_t>(at - order.begin()) + 1 : 0;
    std::int16_t next = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const std::int16_t id = order[(start + i) % order.size()];
        if (!passedOver(*world.find(id))) {
            next = id;
            break;
        }
    }
    selected = next;
}

} // namespace ts2::sim
