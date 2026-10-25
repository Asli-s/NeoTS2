#include "chemistry_page.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>
#include <array>

namespace app {

namespace {

constexpr std::uint32_t kImages = 0x499DB772;

// The page's half that holds the slots: two turn-ons, then the turn-off. Each slot has its icon
// and a button whose tip names it.
constexpr std::uint32_t kHalf = 0x1000;
constexpr std::uint32_t kSlots[] = {0x1001, 0x1002, 0x1003};
constexpr std::uint32_t kIcon = 0x6, kTipButton = 0x5;
constexpr std::size_t kTurnOnSlots = 2;

// The game's turn-ons in three groups. Each is a bit (from 1) of a person data value that holds
// turn-ons and of one that holds turn-offs. Their names are entries 1 to 34 of UI text table 0xE1,
// in order, and each icon's instance is its group's base with the bit number in decimal digits
// (bit 12 of the first group is 0x30010012).
struct Group {
    std::size_t turnOns, turnOffs;
    int bits;
    std::uint32_t iconBase;
};
constexpr Group kGroups[] = {{0xB6, 0xB8, 14, 0x30010000}, {0xB7, 0xB9, 16, 0x30020000}, {0xC9, 0xCA, 4, 0x30030000}};
constexpr std::uint32_t kUiText = 0x7FA7DC7C, kNames = 0xE1;

struct TurnOn {
    int number;  // its place in the list, from 1
    std::uint32_t icon;
};

TurnOn turnOn(std::size_t group, int bit) {
    int number = bit;
    for (std::size_t g = 0; g < group; ++g) {
        number += kGroups[g].bits;
    }
    return {number, kGroups[group].iconBase | static_cast<std::uint32_t>((bit / 10) << 4 | bit % 10)};
}

std::int16_t at(std::span<const std::int16_t> data, std::size_t index) { return index < data.size() ? data[index] : 0; }

// The first turn-ons (or turn-offs) a Sim has, as many as `out` holds, in the game's order: each
// group's value, bit by bit. Slots left over get 0.
void listed(std::span<const std::int16_t> personData, bool turnOffs, std::span<int> out) {
    std::size_t filled = 0;
    for (std::size_t g = 0; g < std::size(kGroups) && filled < out.size(); ++g) {
        const auto bits = static_cast<std::uint16_t>(at(personData, turnOffs ? kGroups[g].turnOffs : kGroups[g].turnOns));
        for (int bit = 1; bit <= kGroups[g].bits && filled < out.size(); ++bit) {
            if ((bits & (1u << (bit - 1))) != 0) {
                out[filled++] = turnOn(g, bit).number;
            }
        }
    }
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(filled), out.end(), 0);
}

} // namespace

void ChemistryPage::attach(ts2::ui::Screen& screen, int page) {
    screen_ = &screen;
    page_ = page;
    for (std::size_t i = 0; i < std::size(kSlots); ++i) {
        showSlot(i, 0);
    }
}

void ChemistryPage::show(std::span<const std::int16_t> personData) {
    if (screen_ == nullptr) {
        return;
    }
    std::array<int, kTurnOnSlots> ons{};
    std::array<int, 1> off{};
    listed(personData, false, ons);
    listed(personData, true, off);
    for (std::size_t i = 0; i < kTurnOnSlots; ++i) {
        showSlot(i, ons[i]);
    }
    showSlot(kTurnOnSlots, off[0]);
}

void ChemistryPage::showSlot(std::size_t slot, int number) {
    if (shown_[slot] == number) {
        return;
    }
    shown_[slot] = number;
    auto& s = *screen_;
    const int window = s.find(s.find(page_, kHalf), kSlots[slot]);
    s.setVisible(window, number != 0);
    if (number == 0) {
        return;
    }
    std::size_t group = 0;
    int bit = number;
    while (group + 1 < std::size(kGroups) && bit > kGroups[group].bits) {
        bit -= kGroups[group].bits;
        ++group;
    }
    s.setTip(s.find(window, kTipButton), s.data().text(kUiText, kNames << 16 | static_cast<std::uint32_t>(number)));
    // The icon keeps its own size, centred in its window. A missing icon leaves the window empty.
    const int icon = s.find(window, kIcon);
    auto picture = s.data().image(kImages, turnOn(group, bit).icon);
    s.setVisible(icon, picture.has_value());
    if (picture) {
        const int w = s.width(icon), h = s.height(icon);
        s.setImage(icon, ts2::content::placed(*picture, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                              (w - static_cast<int>(picture->width)) / 2, (h - static_cast<int>(picture->height)) / 2));
    }
}

} // namespace app
