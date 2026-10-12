#include "wants_panel.hpp"

#include "content/ui_meters.hpp"

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kImages = 0x499DB772;
// The wants script's layouts, by how many wants and fears they hold.
struct LayoutIds {
    std::uint32_t group;
    std::size_t wants, fears;
};
constexpr LayoutIds kLayouts[] = {
    {0x2E9E02BA, 4, 3}, {0xCE9C9E38, 4, 4}, {0xEE9C9E55, 5, 3}, {0xCE9C9F29, 5, 4},
    {0xCE9C9F2D, 6, 3}, {0xCE9C9F40, 6, 4}, {0xCE9C9F39, 7, 3},
};
// Slots: 0xA001 on for wants, 0xB001 on for fears.
constexpr std::uint32_t kFirstWantSlot = 0xA001, kFirstFearSlot = 0xB001;

// A want's icon script and its windows.
constexpr std::uint32_t kIconScript = 0x49064901;
constexpr std::uint32_t kWantBackground = 0x2000, kFearBackground = 0x3000;
constexpr std::uint32_t kBurst = 0xA, kPicture = 0xB, kBadge = 0xC, kLock = 0xD, kInfluence = 0xF;
// The icon sits 2 pixels in from the left of the 44 x 44 picture.
constexpr std::uint32_t kPictureSize = 44;
constexpr int kIconX = 2;
// A badge id that stands for no badge.
constexpr std::uint32_t kNoBadge = 0xC0050000;

} // namespace

void WantsPanel::attach(ts2::ui::Screen& screen, int content) {
    screen_ = &screen;
    for (const LayoutIds& ids : kLayouts) {
        Layout layout;
        layout.group = screen.find(content, ids.group);
        layout.wantCount = ids.wants;
        layout.fearCount = ids.fears;
        for (std::size_t i = 0; i < ids.wants; ++i) {
            layout.wants.push_back({screen.find(layout.group, kFirstWantSlot + static_cast<std::uint32_t>(i))});
        }
        for (std::size_t i = 0; i < ids.fears; ++i) {
            layout.fears.push_back({screen.find(layout.group, kFirstFearSlot + static_cast<std::uint32_t>(i))});
        }
        screen.setVisible(layout.group, false);
        layouts_.push_back(std::move(layout));
    }
}

void WantsPanel::show(const std::vector<Want>& wants, const std::vector<Want>& fears) {
    if (screen_ == nullptr || layouts_.empty()) {
        return;
    }
    // The smallest layout with room for them all (the largest when none has).
    int pick = static_cast<int>(layouts_.size()) - 1;
    for (std::size_t i = 0; i < layouts_.size(); ++i) {
        if (layouts_[i].wantCount >= wants.size() && layouts_[i].fearCount >= fears.size()) {
            pick = static_cast<int>(i);
            break;
        }
    }
    if (pick != shown_) {
        for (std::size_t i = 0; i < layouts_.size(); ++i) {
            screen_->setVisible(layouts_[i].group, static_cast<int>(i) == pick);
        }
        shown_ = pick;
    }
    Layout& layout = layouts_[static_cast<std::size_t>(pick)];
    for (std::size_t i = 0; i < layout.wants.size(); ++i) {
        fill(layout.wants[i], i < wants.size() ? &wants[i] : nullptr, false);
    }
    for (std::size_t i = 0; i < layout.fears.size(); ++i) {
        fill(layout.fears[i], i < fears.size() ? &fears[i] : nullptr, true);
    }
}

void WantsPanel::refresh() {
    for (Layout& layout : layouts_) {
        for (auto* slots : {&layout.wants, &layout.fears}) {
            for (Slot& slot : *slots) {
                slot.guid = 0;
                slot.target = nullptr;
            }
        }
    }
}

void WantsPanel::fill(Slot& slot, const Want* want, bool fear) {
    auto& s = *screen_;
    if (slot.window < 0 || (want == nullptr && !slot.filled) || (want != nullptr && slot.filled && want->guid == slot.guid && want->targetPicture == slot.target)) {
        return;
    }
    if (slot.icon < 0) {
        slot.icon = s.load(kScripts, kIconScript, 0, 0, slot.window);
        if (slot.icon < 0) {
            return;
        }
    }
    s.setVisible(slot.icon, want != nullptr);
    slot.filled = want != nullptr;
    slot.guid = want ? want->guid : 0;
    slot.target = want ? want->targetPicture : nullptr;
    if (want == nullptr) {
        return;
    }
    s.setVisible(s.find(slot.icon, kWantBackground), !fear);
    s.setVisible(s.find(slot.icon, kFearBackground), fear);
    s.setVisible(s.find(slot.icon, kBurst), want->power);
    s.setVisible(s.find(slot.icon, kInfluence), want->influence);
    s.setVisible(s.find(slot.icon, kLock), false);
    const int picture = s.find(slot.icon, kPicture);
    // A want aimed at a Sim shows the middle of that Sim's portrait over the whole icon.
    if (want->targetPicture != nullptr) {
        s.setVisible(picture, true);
        s.setImage(picture, *want->targetPicture);
    }
    const bool hasPicture = !want->hasTarget && want->primaryIcon != 0;
    if (want->targetPicture == nullptr) {
        s.setVisible(picture, hasPicture);
    }
    if (hasPicture) {
        s.setImage(picture, ts2::content::placed(this->picture(want->primaryIcon), kPictureSize, kPictureSize, kIconX, 0));
    }
    const int badge = s.find(slot.icon, kBadge);
    const bool hasBadge = want->secondaryIcon != 0 && want->secondaryIcon != kNoBadge;
    s.setVisible(badge, hasBadge);
    if (hasBadge) {
        s.setImage(badge, this->picture(want->secondaryIcon));
    }
}

const ts2::tga::Pixels& WantsPanel::picture(std::uint32_t instance) {
    auto it = pictures_.find(instance);
    if (it == pictures_.end()) {
        it = pictures_.emplace(instance, screen_->data().image(kImages, instance).value_or(ts2::tga::Pixels{})).first;
    }
    return it->second;
}

} // namespace app
