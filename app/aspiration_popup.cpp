#include "aspiration_popup.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>
#include <array>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kScript = 0x4906502A, kImages = 0x499DB772;
// In the live panel, the anchor the popup's pointer stands on.
constexpr std::uint32_t kAnchor = 0xCD8B8B20;
// The live panel's wants pop-out background and tile: while the pop-out shows, the pointer grows by
// the distance between their tops.
constexpr std::uint32_t kWantsPopout = 0x2E9CAC56, kWantsTile = 0xA94E97E5;
// The popup's windows.
constexpr std::uint32_t kBackground = 0x1, kPointer = 0x2;
constexpr std::uint32_t kIntroBox = 0xA000, kIntro = 0xA001, kRewardsIcon = 0xA002, kRewards = 0xA003, kLevelLine = 0xA004,
                        kLevelBox = 0xA005, kLevel = 0xA006, kTipBox = 0xA007, kTip = 0xA008, kLifetimeLine = 0xA009,
                        kLifetimeSection = 0xA00A, kLifetimeIcon = 0xA00B, kLifetimeName = 0xA00C, kCheckMark = 0xA00D,
                        kLifetimeBlurb = 0xA00E;
constexpr auto kDelay = std::chrono::milliseconds(500);
// The lifetime want's section is at least this high.
constexpr int kSectionMinHeight = 53;
// The pointer's marker child, whose left edge is the pointer's margin beside the anchor.
constexpr std::uint32_t kPointerMarker = 0x1;

} // namespace

void AspirationPopup::attach(ts2::ui::Screen& screen, int panel) {
    screen_ = &screen;
    panel_ = panel;
    button_ = screen.find(panel, kButton);
    screen.setVisible(button_, true);
}

bool AspirationPopup::overButton(int x, int y) const {
    auto& s = *screen_;
    if (button_ < 0) {
        return false;
    }
    int left = 0, top = 0;
    for (int w = button_; w >= 0; w = s.parent(w)) {
        if (!s.visible(w)) {
            return false;
        }
        left += s.x(w);
        top += s.y(w);
    }
    return x >= left && y >= top && x < left + s.width(button_) && y < top + s.height(button_);
}

void AspirationPopup::mouseAt(int x, int y) {
    if (screen_ == nullptr) {
        return;
    }
    mouseX_ = x;
    mouseY_ = y;
    const bool over = overButton(x, y);
    if (over && !over_) {
        since_ = std::chrono::steady_clock::now();
        filled_ = false;
        // The pointer's height is taken as the mouse comes. While the wants pop-out shows, it
        // grows by the distance between the pop-out's top and the wants tile's.
        auto& s = *screen_;
        const int popout = s.find(panel_, kWantsPopout), tile = s.find(panel_, kWantsTile);
        popoutRaise_.reset();
        if (popout >= 0 && tile >= 0 && s.visible(popout)) {
            popoutRaise_ = s.y(tile) - s.y(popout);
        }
    } else if (!over && over_) {
        hide();
    }
    over_ = over;
}

void AspirationPopup::hide() {
    filled_ = false;
    if (screen_ != nullptr && popup_ >= 0) {
        screen_->setVisible(popup_, false);
    }
}

void AspirationPopup::update(const std::function<Content()>& make) {
    if (screen_ == nullptr || !over_) {
        return;
    }
    // Each tick the popup hides once the mouse is off the button or the live panel, or a window
    // holding it, is hidden (a pet selected), without waiting for the mouse to move.
    if (!overButton(mouseX_, mouseY_)) {
        hide();
        over_ = false;
        return;
    }
    // Once the mouse has stayed on the button, the popup is filled for the Sim selected then and
    // shows. It isn't filled again while it shows.
    if (!filled_ && std::chrono::steady_clock::now() - since_ >= kDelay) {
        fill(make());
        filled_ = true;
        if (popup_ >= 0) {
            screen_->setVisible(popup_, true);
            screen_->raise(popup_);
        }
    }
}

void AspirationPopup::fill(const Content& content) {
    auto& s = *screen_;
    if (popup_ < 0) {
        // A window of its own over the whole screen, so the live panel's area doesn't cut it off.
        popup_ = s.load(kScripts, kScript, 0, 0);
        if (popup_ < 0) {
            return;
        }
        s.setVisible(popup_, false);
        s.setPassive(popup_);
        for (const std::uint32_t id : {kIntro, kRewards, kLifetimeName, kLifetimeBlurb, kTip}) {
            s.setWrapped(s.find(popup_, id), true);
        }
        // The pointer's measures as the script has them, which the layout keeps.
        const int pointer = s.find(popup_, kPointer), background = s.find(popup_, kBackground), tip = s.find(popup_, kTip);
        if (pointer >= 0 && background >= 0 && tip >= 0) {
            measures_.pointerHeight = s.height(pointer);
            measures_.overlap = s.y(background) + s.height(background) - s.y(pointer);
            measures_.right = s.width(popup_) - s.x(pointer) - s.width(pointer);
            measures_.side = s.x(s.find(pointer, kPointerMarker));
            measures_.bottom = s.height(popup_) - s.y(tip) - s.height(tip);
        }
    }
    auto part = [&](std::uint32_t id) { return s.find(popup_, id); };
    auto bottom = [&](int w) { return s.y(w) + s.height(w); };
    auto moveTo = [&](int w, int y) { s.move(w, s.x(w), y); };

    // The introduction and the rewards line stack in their box.
    const int intro = part(kIntro), rewards = part(kRewards), introBox = part(kIntroBox);
    s.fitHeightToText(intro);
    s.fitHeightToText(rewards);
    moveTo(rewards, bottom(intro) - 2);
    moveTo(part(kRewardsIcon), s.y(rewards) + 4);
    s.resize(introBox, s.width(introBox), bottom(rewards) - s.y(introBox));

    // The level, under its line, on its picture.
    const int levelLine = part(kLevelLine), level = part(kLevel);
    moveTo(levelLine, bottom(introBox) + 3);
    s.setText(levelLine, content.levelLine);
    s.fitHeightToText(levelLine);
    moveTo(level, bottom(levelLine));
    s.setText(level, content.level);
    moveTo(part(kLevelBox), s.y(level));
    int last = bottom(level);

    // The lifetime want, when the Sim has one: its icon (with a check mark once fulfilled), name and
    // blurb. The section grows with a long blurb and never shrinks back.
    const int lifetimeLine = part(kLifetimeLine), section = part(kLifetimeSection);
    s.setVisible(lifetimeLine, content.lifetime.has_value());
    s.setVisible(section, content.lifetime.has_value());
    if (content.lifetime) {
        moveTo(lifetimeLine, last - 4);
        s.setText(lifetimeLine, content.lifetimeLine);
        s.fitHeightToText(lifetimeLine);
        moveTo(section, bottom(lifetimeLine) - 3);
        const int icon = part(kLifetimeIcon);
        const auto picture = content.lifetime->icon != 0 ? s.data().image(kImages, content.lifetime->icon) : std::nullopt;
        const bool hasIcon = picture && picture->width > 0;
        s.setVisible(icon, hasIcon);
        s.setVisible(part(kCheckMark), hasIcon && content.lifetime->fulfilled);
        if (hasIcon) {
            const auto w = static_cast<std::uint32_t>(s.width(icon)), h = static_cast<std::uint32_t>(s.height(icon));
            s.setImage(icon, ts2::content::placed(*picture, w, h, (static_cast<int>(w) - static_cast<int>(picture->width)) / 2,
                                                  (static_cast<int>(h) - static_cast<int>(picture->height)) / 2));
        }
        const int name = part(kLifetimeName), blurb = part(kLifetimeBlurb);
        s.setText(name, content.lifetime->name);
        s.fitHeightToText(name);
        moveTo(blurb, bottom(name) - 8);
        s.setText(blurb, content.lifetime->blurb);
        s.fitHeightToText(blurb);
        if (bottom(blurb) > kSectionMinHeight) {
            s.resize(section, s.width(section), bottom(blurb));
        }
        last = bottom(section);
    }

    // The tip at the bottom, on its box.
    const int tip = part(kTip), tipBox = part(kTipBox);
    moveTo(tip, last + 9);
    s.fitHeightToText(tip);
    s.move(tipBox, s.x(tip), s.y(tip));
    s.resize(tipBox, s.width(tip), s.height(tip));

    // The pointer runs down from the background to the popup's bottom, which stands on the anchor.
    // While the wants pop-out showed as the mouse came, the pointer is its margin plus the
    // pop-out's raise, and the popup is that much taller than the script's pointer.
    const int background = part(kBackground), pointer = part(kPointer), anchor = s.find(panel_, kAnchor);
    if (background < 0 || pointer < 0 || anchor < 0) {
        return;
    }
    const int pointerHeight = popoutRaise_ ? measures_.side + *popoutRaise_ : measures_.pointerHeight;
    const int height = bottom(tip) + (pointerHeight - measures_.pointerHeight) + measures_.bottom;
    const int width = s.width(popup_);
    s.resize(popup_, width, height);
    s.resize(background, width, height - pointerHeight + measures_.overlap - s.y(background));
    // The pointer is the anchor's width and its margin each side, and keeps its right edge.
    const int pointerWidth = s.width(anchor) + 2 * measures_.side;
    s.resize(pointer, pointerWidth, pointerHeight);
    s.move(pointer, width - measures_.right - pointerWidth, height - pointerHeight);
    int anchorX = 0, anchorY = 0;
    for (int w = anchor; w >= 0; w = s.parent(w)) {
        anchorX += s.x(w);
        anchorY += s.y(w);
    }
    s.move(popup_, anchorX - measures_.side - s.x(pointer), anchorY + s.height(anchor) - height);
}

} // namespace app
