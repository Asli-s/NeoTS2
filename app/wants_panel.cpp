#include "wants_panel.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>

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
constexpr std::uint32_t kWantData = 0xA000;
// Its animated bitmaps: the glow over a satisfied want, the wait before a dropped one goes, and the
// spins that follow them (wants, fears). They play one after another, each ending in the next.
constexpr std::uint32_t kAchieve = 0x4000, kChange = 0x5000, kWantSpin = 0x6000, kFearSpin = 0x7000;
constexpr std::uint32_t kAnimations[] = {kAchieve, kChange, kWantSpin, kFearSpin};
constexpr std::uint32_t kConstantsTable = 0x8D9DF6D4;
constexpr std::uint32_t kBurst = 0xA, kPicture = 0xB, kBadge = 0xC, kLock = 0xD, kButton = 0xE, kInfluence = 0xF;
// The icon sits 2 pixels in from the left of the 44 x 44 picture.
constexpr std::uint32_t kPictureSize = 44;
constexpr int kIconX = 2;
// A badge id that stands for no badge.
constexpr std::uint32_t kNoBadge = 0xC0050000;

// The popup (script 0x49064903): its backgrounds, pointer, picture (burst, picture, badge), name,
// description with its box, divider, lock and hint. It is centred on the panel's Popup Anchor with its
// bottom on the anchor's top, and closes when the mouse leaves the Popup Hover Area. Its parts stack
// as the game lays them out: the name at least 47 high, the description 7 below it, the divider
// 1 below that, the hint right under it with the lock 8 below its top, the popup 36 below the hint.
constexpr std::uint32_t kPopupScript = 0x49064903;
constexpr std::uint32_t kPopupBackground = 0xA0CB, kPopupHorribleBackground = 0xA0CA, kPopupPointer = 0xA02A;
constexpr std::uint32_t kPopupBurst = 0xA001, kPopupPicture = 0xA002, kPopupBadge = 0xA003;
constexpr std::uint32_t kPopupName = 0xA004, kPopupDescription = 0xA005, kPopupBox = 0xA006, kPopupDivider = 0xA007,
                        kPopupLock = 0xA008, kPopupHint = 0xA009;
constexpr std::uint32_t kPopupAnchor = 0xD000, kPopupHoverArea = 0xC000;
constexpr int kNameMinHeight = 47, kDescriptionGap = 7, kDividerGap = 1, kLockDrop = 8, kPopupBottom = 36;
// The background keeps 17 to the popup's bottom. The pointer is 18 high at the bottom, the anchor's
// width less 5.
constexpr int kBackgroundGap = 17, kPointerHeight = 18, kPointerNarrower = 5;
constexpr std::uint32_t kPopupPictureSize = 46;

} // namespace

void WantsPanel::attach(ts2::ui::Screen& screen, int content) {
    screen_ = &screen;
    content_ = content;
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

void WantsPanel::show(const std::vector<Want>& wants, const std::vector<Want>& fears, std::size_t wantSlots, std::size_t fearSlots,
                      const Refill& refill) {
    if (screen_ == nullptr || layouts_.empty()) {
        return;
    }
    // The smallest layout with room for the Sim's slots (the largest when none has).
    int pick = static_cast<int>(layouts_.size()) - 1;
    for (std::size_t i = 0; i < layouts_.size(); ++i) {
        if (layouts_[i].wantCount >= wantSlots && layouts_[i].fearCount >= fearSlots) {
            pick = static_cast<int>(i);
            break;
        }
    }
    // Another Sim or layout fills its slots at once.
    const bool anew = pick != shown_ || refill.sim != sim_;
    if (pick != shown_) {
        for (std::size_t i = 0; i < layouts_.size(); ++i) {
            screen_->setVisible(layouts_[i].group, static_cast<int>(i) == pick);
        }
        shown_ = pick;
    }
    sim_ = refill.sim;
    if (wants != wants_ || fears != fears_) {
        wants_ = wants;
        fears_ = fears;
        // A clicked want that changed closes its popup and highlight. The same want refills an open popup.
        const auto& shown = popupWant_;
        if (shown) {
            const auto same = [&](const Want& w) { return w.guid == shown->guid; };
            const Want* found = nullptr;
            if (const auto it = std::find_if(wants_.begin(), wants_.end(), same); it != wants_.end()) {
                found = &*it;
            } else if (const auto jt = std::find_if(fears_.begin(), fears_.end(), same); jt != fears_.end()) {
                found = &*jt;
            }
            if (found == nullptr) {
                hidePopup();
            } else if (popup_ >= 0 && screen_->visible(popup_)) {
                showPopup(*found);
            } else {
                popupWant_ = *found;
            }
        }
    }
    Layout& layout = layouts_[static_cast<std::size_t>(pick)];
    // A place with GUID 0 is empty.
    for (const bool fear : {false, true}) {
        auto& slots = fear ? layout.fears : layout.wants;
        const auto& list = fear ? fears : wants;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const Want* want = i < list.size() && list[i].guid != 0 ? &list[i] : nullptr;
            std::uint32_t satisfied = 0;
            if (refill.satisfied != nullptr) {
                const auto it = refill.satisfied->find(i + (fear ? 1000 : 0));
                satisfied = it != refill.satisfied->end() ? it->second : 0;
            }
            Slot& slot = slots[i];
            if (anew) {
                stopAnimations(slot);
                slot.target = want != nullptr ? std::optional<Want>(*want) : std::nullopt;
                slot.satisfied = satisfied;
                fill(slot, want, fear);
            } else {
                give(slot, want, fear, satisfied, refill.pending);
            }
            animate(slot, fear);
        }
    }
    highlight();
}

void WantsPanel::refresh() {
    for (Layout& layout : layouts_) {
        for (auto* slots : {&layout.wants, &layout.fears}) {
            for (Slot& slot : *slots) {
                slot.stale = true;
            }
        }
    }
}

std::optional<WantsPanel::SlotRef> WantsPanel::slotOf(int window) const {
    if (screen_ == nullptr || shown_ < 0) {
        return std::nullopt;
    }
    const Layout& layout = layouts_[static_cast<std::size_t>(shown_)];
    for (int w = window; w >= 0; w = screen_->parent(w)) {
        for (const bool fear : {false, true}) {
            const auto& slots = fear ? layout.fears : layout.wants;
            for (std::size_t i = 0; i < slots.size(); ++i) {
                if (slots[i].icon == w && slots[i].shown) {
                    return SlotRef{i, fear};
                }
            }
        }
    }
    return std::nullopt;
}

bool WantsPanel::clicked(int window) {
    const auto slot = slotOf(window);
    if (!slot) {
        return false;
    }
    const auto& list = slot->fear ? fears_ : wants_;
    if (slot->index < list.size()) {
        showPopup(list[slot->index]);
    }
    return true;
}

void WantsPanel::mouseAt(int x, int y) {
    if (screen_ == nullptr || popup_ < 0 || !screen_->visible(popup_) || shown_ < 0) {
        return;
    }
    const int area = screen_->find(layouts_[static_cast<std::size_t>(shown_)].group, kPopupHoverArea);
    const auto at = screenPlace(area);
    const bool inside = area >= 0 && x >= at[0] && y >= at[1] && x < at[0] + screen_->width(area) && y < at[1] + screen_->height(area);
    if (!inside) {
        screen_->setVisible(popup_, false);
    }
}

void WantsPanel::hidePopup() {
    popupWant_.reset();
    if (screen_ != nullptr && popup_ >= 0) {
        screen_->setVisible(popup_, false);
    }
    highlight();
}

void WantsPanel::highlight() {
    if (screen_ == nullptr || shown_ < 0) {
        return;
    }
    // The icon of the want the popup shows looks pressed (its button's checked frame).
    Layout& layout = layouts_[static_cast<std::size_t>(shown_)];
    for (auto* slots : {&layout.wants, &layout.fears}) {
        for (const Slot& slot : *slots) {
            if (slot.icon >= 0) {
                const bool on = slot.shown && popupWant_ && slot.shown->guid == popupWant_->guid;
                screen_->setChecked(screen_->find(slot.icon, kButton), on);
            }
        }
    }
}

std::array<int, 2> WantsPanel::screenPlace(int window) const {
    std::array<int, 2> at{0, 0};
    for (int w = window; w >= 0; w = screen_->parent(w)) {
        at[0] += screen_->x(w);
        at[1] += screen_->y(w);
    }
    return at;
}

void WantsPanel::showPopup(const Want& want) {
    auto& s = *screen_;
    if (popup_ < 0) {
        popup_ = s.load(kScripts, kPopupScript, 0, 0, content_);
        if (popup_ < 0) {
            return;
        }
        for (const std::uint32_t id : {kPopupName, kPopupDescription, kPopupHint}) {
            s.setWrapped(s.find(popup_, id), true);
        }
        s.setVisible(s.find(popup_, kPopupHorribleBackground), false);
    }
    popupWant_ = want;
    auto part = [&](std::uint32_t id) { return s.find(popup_, id); };

    // The picture as the slot shows it, at the popup's size.
    s.setVisible(part(kPopupBurst), want.power);
    const int picture = part(kPopupPicture);
    if (want.popupTargetPicture != nullptr) {
        s.setVisible(picture, true);
        s.setImage(picture, *want.popupTargetPicture);
    } else if (!want.hasTarget && want.primaryIcon != 0) {
        const auto& icon = this->picture(want.primaryIcon);
        s.setVisible(picture, true);
        s.setImage(picture, ts2::content::placed(icon, kPopupPictureSize, kPopupPictureSize, static_cast<int>(kPopupPictureSize - icon.width) / 2,
                                                 static_cast<int>(kPopupPictureSize - icon.height) / 2));
    } else {
        s.setVisible(picture, false);
    }
    const bool hasBadge = want.secondaryIcon != 0 && want.secondaryIcon != kNoBadge;
    s.setVisible(part(kPopupBadge), hasBadge);
    if (hasBadge) {
        s.setImage(part(kPopupBadge), this->picture(want.secondaryIcon));
    }

    // The texts, stacked.
    const int name = part(kPopupName), description = part(kPopupDescription), box = part(kPopupBox);
    const int divider = part(kPopupDivider), hint = part(kPopupHint), lock = part(kPopupLock);
    s.setText(name, want.popupName);
    s.fitHeightToText(name);
    s.resize(name, s.width(name), std::max(kNameMinHeight, s.height(name)));
    s.setText(description, want.description);
    s.move(description, s.x(description), s.y(name) + s.height(name) + kDescriptionGap);
    s.fitHeightToText(description);
    s.move(divider, s.x(divider), s.y(description) + s.height(description) + kDividerGap);
    s.move(hint, s.x(hint), s.y(divider) + s.height(divider));
    s.fitHeightToText(hint);
    s.move(lock, s.x(lock), s.y(hint) + kLockDrop);
    s.move(box, s.x(box), s.y(description));
    s.resize(box, s.width(box), s.y(hint) + s.height(hint) - s.y(description));
    const int height = s.y(hint) + s.height(hint) + kPopupBottom;
    const int width = s.width(popup_);
    s.resize(popup_, width, height);
    const int background = part(kPopupBackground);
    s.resize(background, width, height - kBackgroundGap - s.y(background));

    // Centred on the anchor, its bottom on the anchor's top, the pointer over the anchor.
    const int anchor = shown_ >= 0 ? s.find(layouts_[static_cast<std::size_t>(shown_)].group, kPopupAnchor) : -1;
    if (anchor < 0) {
        return;
    }
    const auto anchorAt = screenPlace(anchor), contentAt = screenPlace(content_);
    const int anchorX = anchorAt[0] - contentAt[0], anchorY = anchorAt[1] - contentAt[1];
    const int pointer = part(kPopupPointer);
    const int pointerWidth = s.width(anchor) - kPointerNarrower;
    const int pointerX = (width - pointerWidth) / 2;
    s.move(pointer, pointerX, height - kPointerHeight);
    s.resize(pointer, pointerWidth, kPointerHeight);
    s.move(popup_, anchorX + (s.width(anchor) - pointerWidth) / 2 - pointerX, anchorY - height);
    s.setVisible(popup_, true);
    highlight();
}

void WantsPanel::fill(Slot& slot, const Want* want, bool fear) {
    auto& s = *screen_;
    const bool same = want != nullptr ? slot.shown == *want : !slot.shown;
    if (slot.window < 0 || (same && !slot.stale)) {
        return;
    }
    if (slot.icon < 0) {
        slot.icon = s.load(kScripts, kIconScript, 0, 0, slot.window);
        if (slot.icon < 0) {
            return;
        }
    }
    s.setVisible(slot.icon, want != nullptr);
    slot.shown = want != nullptr ? std::optional<Want>(*want) : std::nullopt;
    slot.stale = false;
    if (want == nullptr) {
        return;
    }
    s.setVisible(s.find(slot.icon, kWantData), true);
    s.setVisible(s.find(slot.icon, kWantBackground), !fear);
    s.setVisible(s.find(slot.icon, kFearBackground), fear);
    s.setVisible(s.find(slot.icon, kBurst), want->power);
    s.setVisible(s.find(slot.icon, kInfluence), want->influence);
    s.setVisible(s.find(slot.icon, kLock), want->held);
    s.setTip(s.find(slot.icon, kButton), want->tip);
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

void WantsPanel::give(Slot& slot, const Want* want, bool fear, std::uint32_t satisfied, bool pending) {
    if (want == nullptr) {
        // A satisfied want's place stays as it was until the Sim's new wants come.
        if (slot.target && pending) {
            return;
        }
        stopAnimations(slot);
        slot.target.reset();
        fill(slot, nullptr, fear);
        return;
    }
    const bool same = slot.target && slot.target->guid == want->guid && slot.target->argument == want->argument;
    if (same) {
        slot.target = *want;
        slot.satisfied = satisfied;
        if (slot.phase == Slot::Phase::None) {
            fill(slot, want, fear);
        }
        return;
    }
    // A new want: the old one glows when it was satisfied, else it waits, and both end in a spin.
    const bool had = slot.target.has_value();
    const bool achieved = satisfied != slot.satisfied;
    slot.target = *want;
    slot.satisfied = satisfied;
    stopAnimations(slot);
    if (!had || !play(slot, achieved ? Slot::Phase::Achieve : Slot::Phase::Change, achieved ? kAchieve : kChange)) {
        fill(slot, want, fear);
    }
}

bool WantsPanel::play(Slot& slot, Slot::Phase phase, std::uint32_t id) {
    auto& s = *screen_;
    const int window = slot.icon >= 0 ? s.find(slot.icon, id) : -1;
    if (window < 0) {
        return false;
    }
    const auto constants = s.childTexts(s.find(window, kConstantsTable));
    ts2::ui::AnimatedBitmap bitmap(constants, s.imageSize(window)[0], s.width(window));
    if (!bitmap.play(static_cast<std::uint32_t>(random_()))) {
        return false;
    }
    slot.phase = phase;
    slot.animation = window;
    slot.bitmap = std::move(bitmap);
    slot.started = std::chrono::steady_clock::now();
    s.setVisible(window, true);
    return true;
}

void WantsPanel::animate(Slot& slot, bool fear) {
    if (slot.phase == Slot::Phase::None || !slot.bitmap) {
        return;
    }
    auto& s = *screen_;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - slot.started).count();
    const auto state = slot.bitmap->at(static_cast<std::uint32_t>(std::clamp<long long>(ms, 0, UINT32_MAX)));
    if (slot.bitmap->type() == ts2::ui::AnimatedBitmap::Type::AlphaFade) {
        s.setImageColor(slot.animation, {1, 1, 1, static_cast<float>(slot.bitmap->alpha(state.frame)) / 255.0f});
    } else {
        const int w = s.width(slot.animation);
        s.setImagePart(slot.animation, {w * state.frame, 0, w * (state.frame + 1), s.imageSize(slot.animation)[1]});
    }
    if (!state.done) {
        return;
    }
    // The glow or the wait ends in a spin with the want hidden, and the spin in the new want.
    const auto phase = slot.phase;
    stopAnimations(slot);
    if (phase != Slot::Phase::Spin) {
        s.setVisible(s.find(slot.icon, kWantData), false);
        if (play(slot, Slot::Phase::Spin, fear ? kFearSpin : kWantSpin)) {
            return;
        }
    }
    fill(slot, slot.target ? &*slot.target : nullptr, fear);
}

void WantsPanel::stopAnimations(Slot& slot) {
    // The want's picture may have been hidden for the spin.
    slot.stale = slot.stale || slot.phase != Slot::Phase::None;
    slot.phase = Slot::Phase::None;
    slot.animation = -1;
    slot.bitmap.reset();
    if (slot.icon < 0) {
        return;
    }
    for (const std::uint32_t id : kAnimations) {
        screen_->setVisible(screen_->find(slot.icon, id), false);
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
