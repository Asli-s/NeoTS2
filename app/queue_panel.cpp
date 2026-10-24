#include "queue_panel.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kImages = 0x499DB772;
// The row's place (a constants table only) and one icon.
constexpr std::uint32_t kPlaceScript = 0xCCD02691, kIconScript = 0x0609A1C0;
// The icon script's two frames: one for the row, one for nested interactions.
constexpr std::uint32_t kTopFrame = 0x100, kNestedFrame = 0x200, kConstants = 0x300;
// Their parts. The plain frame and the joined one (the row's with its nest cutout, the nested
// frame's middle piece) take turns.
constexpr std::uint32_t kFrame = 1, kJoinedFrame = 2, kPicture = 3, kCancelled = 4, kActive = 5, kHighlight = 6, kPictureBackground = 7;

const ts2::ui::Window* topWindow(const std::vector<ts2::ui::Window>& script, std::uint32_t id) {
    const auto it = std::find_if(script.begin(), script.end(), [id](const ts2::ui::Window& w) { return w.number("id") == id; });
    return it != script.end() ? &*it : nullptr;
}

const ts2::ui::Window* childWindow(const ts2::ui::Window& parent, std::uint32_t id) {
    const auto it = std::find_if(parent.children.begin(), parent.children.end(), [id](const ts2::ui::Window& w) { return w.number("id") == id; });
    return it != parent.children.end() ? &*it : nullptr;
}

// The picture of a frame's picture window: the shape icons are cut to.
ts2::tga::Pixels shapeOf(ts2::ui::Screen& screen, const ts2::ui::Window& frame) {
    const auto* picture = childWindow(frame, kPicture);
    const auto image = picture ? picture->list("image") : std::vector<std::int64_t>{};
    if (image.empty()) {
        return {};
    }
    return screen.data().image(kImages, static_cast<std::uint32_t>(image.back())).value_or(ts2::tga::Pixels{});
}

} // namespace

void QueuePanel::attach(ts2::ui::Screen& screen) {
    screen_ = &screen;
    if (const auto place = screen.data().script(kScripts, kPlaceScript); place && !place->empty()) {
        std::vector<std::string> lines;
        for (const auto& w : place->front().children) {
            lines.push_back(w.text("caption"));
        }
        layout_.x = ts2::content::uiConstant(lines, "kLocationX").value_or(layout_.x);
        layout_.y = ts2::content::uiConstant(lines, "kLocationY").value_or(layout_.y);
        layout_.margin = ts2::content::uiConstant(lines, "kIconMarginX").value_or(layout_.margin);
    }
    const auto script = screen.data().script(kScripts, kIconScript);
    const ts2::ui::Window* top = script ? topWindow(*script, kTopFrame) : nullptr;
    const ts2::ui::Window* nested = script ? topWindow(*script, kNestedFrame) : nullptr;
    if (top == nullptr || nested == nullptr) {
        return;
    }
    top_ = {*top};
    nested_ = {*nested};
    // A nested icon starts where its frame sits below the row's frame in the script.
    const auto topArea = top->list("area"), nestedArea = nested->list("area");
    if (topArea.size() == 4 && nestedArea.size() == 4) {
        layout_.width = static_cast<int>(topArea[2] - topArea[0]);
        layout_.height = static_cast<int>(topArea[3] - topArea[1]);
        layout_.nestedOffset = static_cast<int>(nestedArea[1] - topArea[3]);
    }
    if (const ts2::ui::Window* constants = topWindow(*script, kConstants)) {
        std::vector<std::string> lines;
        for (const auto& w : constants->children) {
            lines.push_back(w.text("caption"));
        }
        slideSteps_ = ts2::content::uiConstant(lines, "kAnimationSteps").value_or(slideSteps_);
        slideDelta_ = static_cast<float>(ts2::content::uiConstant(lines, "kAnimationDelta").value_or(1));
    }
    topShape_ = shapeOf(screen, *top);
    nestedShape_ = shapeOf(screen, *nested);
}

QueuePanel::Icon& QueuePanel::iconFor(std::uint32_t id) {
    for (Icon& icon : icons_) {
        if (icon.shown && icon.id == id) {
            return icon;
        }
    }
    // A free icon, or a new one when all are in use.
    auto free = std::find_if(icons_.begin(), icons_.end(), [](const Icon& icon) { return !icon.shown && icon.top >= 0; });
    if (free == icons_.end()) {
        Icon icon;
        icon.top = screen_->addWindows(top_, 0, 0);
        icon.nested = screen_->addWindows(nested_, 0, 0);
        icons_.push_back(icon);
        free = icons_.end() - 1;
    }
    free->id = id;
    free->shown = true;
    free->pictureSet = false;
    free->placed = false;
    return *free;
}

void QueuePanel::fill(Icon& icon, const Item& item, const ts2::content::QueueSlot& slot) {
    ts2::ui::Screen& s = *screen_;
    const int frame = slot.nested ? icon.nested : icon.top;
    s.setVisible(slot.nested ? icon.top : icon.nested, false);
    s.setVisible(frame, visible_);
    if (!icon.placed || icon.isNested != slot.nested) {
        icon.placed = true;
        icon.x = static_cast<float>(slot.x);
        icon.y = static_cast<float>(slot.y);
        s.move(frame, slot.x, slot.y);
    } else if (slot.x != icon.targetX || slot.y != icon.targetY) {
        // A new place: it slides there from where it is, timed from now.
        icon.x = static_cast<float>(s.x(frame));
        icon.y = static_cast<float>(s.y(frame));
        icon.stepped = std::chrono::steady_clock::now();
    }
    icon.targetX = slot.x;
    icon.targetY = slot.y;
    s.setVisible(s.find(frame, kFrame), !slot.joined);
    s.setVisible(s.find(frame, kJoinedFrame), slot.joined);
    s.setVisible(s.find(frame, kCancelled), item.cancelled);
    s.setVisible(s.find(frame, kActive), item.running);
    s.setTip(frame, item.tip);
    if (!icon.pictureSet || icon.picture != item.picture || icon.isNested != slot.nested) {
        icon.pictureSet = true;
        icon.picture = item.picture;
        icon.isNested = slot.nested;
        const int picture = s.find(frame, kPicture);
        const ts2::tga::Pixels& shape = slot.nested ? nestedShape_ : topShape_;
        s.setVisible(picture, item.picture != nullptr);
        s.setVisible(s.find(frame, kPictureBackground), item.picture != nullptr);
        if (item.picture != nullptr) {
            s.setImage(picture, ts2::content::masked(*item.picture, shape));
        }
    }
}

void QueuePanel::show(const std::vector<Item>& items) {
    if (screen_ == nullptr || top_.empty()) {
        return;
    }
    std::vector<bool> nested;
    for (const Item& item : items) {
        nested.push_back(item.nested);
    }
    const auto slots = ts2::content::queueSlots(nested, layout_);
    // Icons of interactions that are gone go back to the free ones.
    for (Icon& icon : icons_) {
        if (icon.shown && std::none_of(items.begin(), items.end(), [&](const Item& item) { return item.id == icon.id; })) {
            icon.shown = false;
            screen_->setVisible(icon.top, false);
            screen_->setVisible(icon.nested, false);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < items.size(); ++i) {
        Icon& icon = iconFor(items[i].id);
        fill(icon, items[i], slots[i]);
        slide(icon, now);
    }
}

void QueuePanel::slide(Icon& icon, std::chrono::steady_clock::time_point now) {
    const auto tx = static_cast<float>(icon.targetX), ty = static_cast<float>(icon.targetY);
    if (icon.x == tx && icon.y == ty) {
        return;
    }
    const double ms = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(now - icon.stepped).count());
    if (ms <= 0) {
        return;
    }
    icon.stepped = now;
    icon.x = ts2::content::slideStep(icon.x, tx, ms, slideSteps_, slideDelta_);
    icon.y = ts2::content::slideStep(icon.y, ty, ms, slideSteps_, slideDelta_);
    screen_->move(icon.isNested ? icon.nested : icon.top, static_cast<int>(icon.x), static_cast<int>(icon.y));
}

void QueuePanel::setVisible(bool visible) {
    visible_ = visible;
    if (screen_ == nullptr) {
        return;
    }
    for (const Icon& icon : icons_) {
        screen_->setVisible(icon.isNested ? icon.nested : icon.top, visible && icon.shown);
    }
}

void QueuePanel::mouseAt(int x, int y) {
    if (screen_ == nullptr) {
        return;
    }
    const auto at = itemAt(x, y);
    for (const Icon& icon : icons_) {
        if (icon.top >= 0) {
            const bool lit = icon.shown && at == icon.id;
            screen_->setVisible(screen_->find(icon.top, kHighlight), lit);
            screen_->setVisible(screen_->find(icon.nested, kHighlight), lit);
        }
    }
}

std::optional<std::uint32_t> QueuePanel::itemAt(int x, int y) const {
    if (screen_ == nullptr || !visible_) {
        return std::nullopt;
    }
    for (const Icon& icon : icons_) {
        const int frame = icon.isNested ? icon.nested : icon.top;
        if (!icon.shown || frame < 0) {
            continue;
        }
        const int left = screen_->x(frame), top = screen_->y(frame);
        if (x >= left && y >= top && x < left + screen_->width(frame) && y < top + screen_->height(frame)) {
            return icon.id;
        }
    }
    return std::nullopt;
}

} // namespace app
