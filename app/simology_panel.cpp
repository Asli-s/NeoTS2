#include "simology_panel.hpp"

#include <utility>

namespace app {

namespace {

// Each tab button and the page it opens.
constexpr std::pair<std::uint32_t, std::uint32_t> kTabs[] = {
    {0x40000000, 0x10000000},  // Personality
    {0x50000000, 0x20000000},  // Interests
    {0x60000000, 0x30000000},  // Memories
    {0x70000000, 0xA0000000},  // Chemistry
    {0x80000000, 0xB0000000},  // Hobbies
};
constexpr std::uint32_t kPersonalityTab = 0x40000000, kPersonalityPage = 0x10000000, kInterestsPage = 0x20000000, kMemoriesPage = 0x30000000;
constexpr std::uint32_t kHobbiesPage = 0xB0000000;
constexpr std::uint32_t kChemistryTab = 0x70000000, kChemistryPage = 0xA0000000;
// Life stages (person data 0x3A) without chemistry, and the person data bit that also turns it off.
constexpr std::int16_t kToddler = 2, kChild = 3;
constexpr std::size_t kChemistryFlags = 0x54;
constexpr std::int16_t kNoChemistry = 0x200;

} // namespace

void SimologyPanel::attach(ts2::ui::Screen& screen, int content) {
    screen_ = &screen;
    content_ = content;
    personality_.attach(screen, screen.find(content, kPersonalityPage));
    interests_.attach(screen, screen.find(content, kInterestsPage));
    hobbies_.attach(screen, screen.find(content, kHobbiesPage));
    chemistryPage_.attach(screen, screen.find(content, kChemistryPage));
    memories_.attach(screen, screen.find(content, kMemoriesPage));
    showPage(kPersonalityTab);
}

void SimologyPanel::show(std::int16_t lifeStage, std::span<const std::int16_t> personData, std::span<const std::int16_t> secretLots) {
    if (screen_ == nullptr) {
        return;
    }
    const std::int16_t flags = kChemistryFlags < personData.size() ? personData[kChemistryFlags] : 0;
    const bool chemistry = lifeStage != kToddler && lifeStage != kChild && (flags & kNoChemistry) == 0;
    if (chemistry != chemistry_) {
        chemistry_ = chemistry;
        screen_->setEnabled(screen_->find(content_, kChemistryTab), chemistry);
        // An open chemistry page that turns off goes back to the personality.
        showPage(page_ == kChemistryTab && !chemistry ? kPersonalityTab : page_);
    }
    personality_.show(personData);
    interests_.show(personData);
    hobbies_.show(personData, secretLots);
    chemistryPage_.show(personData);
}

bool SimologyPanel::clicked(int window) {
    if (screen_ == nullptr) {
        return false;
    }
    if (memories_.clicked(window) || interests_.clicked(window)) {
        return true;
    }
    if (screen_->parent(window) != content_) {
        return false;
    }
    for (const auto& [button, page] : kTabs) {
        if (screen_->id(window) == button && (button != kChemistryTab || chemistry_)) {
            showPage(button);
            return true;
        }
    }
    return false;
}

void SimologyPanel::showPage(std::uint32_t button) {
    auto& s = *screen_;
    page_ = button;
    for (const auto& [b, page] : kTabs) {
        s.setChecked(s.find(content_, b), b == button);
        s.setVisible(s.find(content_, page), b == button);
    }
}

} // namespace app
