#include "pie_menu_ui.hpp"

#include <algorithm>
#include <format>

namespace app {

namespace {

namespace pie = ts2::ui::pie;

// The item pictures: group, and the base of each kind (a sub-menu's ring items are lighter, a
// disabled item grey) to which a piece's bits are added.
constexpr std::uint32_t kImageGroup = 0x499DB772;
constexpr std::uint32_t kNormalBase = 0x98000000;
constexpr std::uint32_t kSubmenuBase = 0x96000000;
constexpr std::uint32_t kDisabledBase = 0x76000000;
constexpr std::uint32_t kLeftCap = 0x100, kRightCap = 0x200, kMiddle = 0x10000;
constexpr std::uint32_t kLit = 0x100000;  // the knob lit green, while the item is highlighted
// The head's area (the menu script's top window), centred on the head.
constexpr int kHeadWidth = 250, kHeadHeight = 370;
const std::string kFont = "0x2966548b";  // "piemenu"
constexpr std::array<float, 4> kTextNormal{178 / 255.f, 193 / 255.f, 1, 1};
constexpr std::array<float, 4> kTextHighlighted{0, 1, 0, 1};
constexpr std::array<float, 4> kTextDisabled{80 / 255.f, 80 / 255.f, 80 / 255.f, 1};

std::uint32_t knobPiece(pie::Shape shape) {
    switch (shape) {
    case pie::Shape::Below:      return 0x01;
    case pie::Shape::Above:      return 0x02;
    case pie::Shape::LeftUpper:  return 0x10;
    case pie::Shape::LeftSide:   return 0x08;
    case pie::Shape::LeftLower:  return 0x40;
    case pie::Shape::RightLower: return 0x80;
    case pie::Shape::RightSide:  return 0x04;
    case pie::Shape::RightUpper: return 0x20;
    case pie::Shape::Centre:     break;
    }
    return 0;
}

ts2::ui::Window piece(int x, int y, int w, int h, std::uint32_t instance, bool visible = true) {
    ts2::ui::Window window;
    window.attributes = {{"iid", "IGZWinGen"},
                         {"paint", "yes"},
                         {"area", std::format("({},{},{},{})", x, y, x + w, y + h)},
                         {"image", std::format("{{{:08x},{:08x}}}", kImageGroup, instance)},
                         {"winflag_visible", visible ? "yes" : "no"}};
    return window;
}

ts2::ui::Window text(int x, int y, int w, int h, const std::string& caption) {
    ts2::ui::Window window;
    window.attributes = {{"iid", "IGZWinText"},
                         {"area", std::format("({},{},{},{})", x, y, x + w, y + h)},
                         {"caption", caption},
                         {"font", kFont},
                         {"winflag_visible", "yes"}};
    return window;
}

} // namespace

void PieMenuUi::open(std::vector<ts2::sim::PieChoice> choices, int x, int y, int screenWidth, int screenHeight) {
    close();
    choices_ = std::move(choices);
    screenWidth_ = screenWidth;
    screenHeight_ = screenHeight;
    // Choices sharing the part of their name before a '/' share an item that opens their sub-menu.
    top_.clear();
    for (std::size_t i = 0; i < choices_.size(); ++i) {
        std::vector<Node>* level = &top_;
        const std::string& name = choices_[i].name;
        std::size_t start = 0;
        // Sub-menus nest at most this deep. The rest of a longer name stays one label.
        constexpr int kMaxLevels = 8;
        for (int depth = 0;; ++depth) {
            const std::size_t slash = depth + 1 < kMaxLevels ? name.find('/', start) : std::string::npos;
            const std::string label = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (slash == std::string::npos) {
                Node leaf;
                leaf.label = label;
                leaf.choice = i;
                leaf.disabled = choices_[i].disabled != ts2::sim::OfferedAction::Disabled::No;
                level->push_back(std::move(leaf));
                break;
            }
            auto group = std::find_if(level->begin(), level->end(), [&](const Node& n) { return !n.choice && n.label == label; });
            if (group == level->end()) {
                level->push_back(Node{label, std::nullopt, false, {}});
                group = level->end() - 1;
            }
            level = &group->children;
            start = slash + 1;
        }
    }
    if (top_.empty()) {
        return;
    }
    levels_ = {Level{&top_, x, y, nullptr}};
    show(levels_.back());
}

void PieMenuUi::close() {
    if (root_ >= 0) {
        screen_.remove(root_);
    }
    root_ = -1;
    levels_.clear();
    shown_.clear();
    backItem_.reset();
    highlighted_.reset();
    backHighlighted_ = false;
    pressed_ = false;
}

int PieMenuUi::addItem(const pie::Item& item, int textWidth, std::uint32_t base, const std::string& label, bool disabled, Shown& shown) {
    if (disabled) {
        base = kDisabledBase;
    }
    const auto& r = item.rect;
    const int pillY = item.shape == pie::Shape::Above ? r.y + pie::kStalkHeight - pie::kPillHeight : r.y;
    std::vector<ts2::ui::Window> pieces;
    const std::uint32_t knob = knobPiece(item.shape);
    int knobIndex = -1;
    auto add = [&](int x, int y, int w, int h, std::uint32_t bits) {
        if (w > 0) {
            pieces.push_back(piece(x, y, w, h, base | bits));
        }
    };
    auto addKnob = [&](int x, int y, int w, int h) {
        knobIndex = static_cast<int>(pieces.size());
        pieces.push_back(piece(x, y, w, h, base | knob));
        pieces.push_back(piece(x, y, w, h, base | knob | kLit, false));
    };
    switch (item.shape) {
    case pie::Shape::Below:
    case pie::Shape::Above: {
        const int half = (r.w - 2 * pie::kCapWidth - pie::kStalkWidth) / 2;
        add(r.x, pillY, pie::kCapWidth, pie::kPillHeight, kLeftCap);
        add(r.x + pie::kCapWidth, pillY, half, pie::kPillHeight, kMiddle);
        addKnob(r.x + pie::kCapWidth + half, r.y, pie::kStalkWidth, pie::kStalkHeight);
        add(r.x + pie::kCapWidth + half + pie::kStalkWidth, pillY, half, pie::kPillHeight, kMiddle);
        add(r.x + r.w - pie::kCapWidth, pillY, pie::kCapWidth, pie::kPillHeight, kRightCap);
        break;
    }
    case pie::Shape::LeftUpper:
    case pie::Shape::LeftSide:
    case pie::Shape::LeftLower:
        addKnob(r.x, r.y, pie::kKnobCapWidth, pie::kPillHeight);
        add(r.x + pie::kKnobCapWidth, r.y, textWidth, pie::kPillHeight, kMiddle);
        add(r.x + r.w - pie::kCapWidth, r.y, pie::kCapWidth, pie::kPillHeight, kRightCap);
        break;
    case pie::Shape::RightLower:
    case pie::Shape::RightSide:
    case pie::Shape::RightUpper:
        add(r.x, r.y, pie::kCapWidth, pie::kPillHeight, kLeftCap);
        add(r.x + pie::kCapWidth, r.y, textWidth, pie::kPillHeight, kMiddle);
        addKnob(r.x + pie::kCapWidth + textWidth, r.y, pie::kKnobCapWidth, pie::kPillHeight);
        break;
    case pie::Shape::Centre:
        add(r.x, r.y, pie::kCapWidth, pie::kPillHeight, kLeftCap);
        add(r.x + pie::kCapWidth, r.y, textWidth, pie::kPillHeight, kMiddle);
        add(r.x + r.w - pie::kCapWidth, r.y, pie::kCapWidth, pie::kPillHeight, kRightCap);
        break;
    }
    const int first = static_cast<int>(screen_.children(root_).size());
    for (const auto& p : pieces) {
        const auto area = p.list("area");
        screen_.addWindows({p}, static_cast<int>(area[0]), static_cast<int>(area[1]), root_);
    }
    const auto size = screen_.textSize(kFont, label);
    shown.text = screen_.addWindows({text(r.x + item.textX, r.y + item.textY, textWidth + 2, size[1] + 2, label)}, r.x + item.textX,
                                    r.y + item.textY, root_);
    screen_.setTextColor(shown.text, disabled ? kTextDisabled : kTextNormal);
    shown.disabled = disabled;
    if (knobIndex >= 0) {
        const auto& children = screen_.children(root_);
        shown.knob = children[static_cast<std::size_t>(first + knobIndex)];
        shown.litKnob = children[static_cast<std::size_t>(first + knobIndex + 1)];
    }
    return shown.text;
}

void PieMenuUi::show(const Level& level) {
    if (root_ >= 0) {
        screen_.remove(root_);
    }
    ts2::ui::Window root;
    root.attributes = {{"iid", "IGZWinGen"}, {"area", std::format("(0,0,{},{})", screenWidth_, screenHeight_)}, {"winflag_visible", "yes"}};
    root_ = screen_.addWindows({root}, 0, 0);
    shown_.clear();
    highlighted_.reset();
    backHighlighted_ = false;
    pressed_ = false;

    const auto textHeight = screen_.textSize(kFont, "Ag")[1];
    std::vector<int> widths;
    for (const auto& node : *level.nodes) {
        widths.push_back(screen_.textSize(kFont, node.label)[0]);
    }
    // A sub-menu goes round the item clicked. The head stays where the menu opened.
    const bool top = level.back == nullptr;
    layout_ = pie::layout(level.x, level.y, widths, textHeight, top ? kHeadWidth : 0, top ? kHeadHeight : 0, screenWidth_, screenHeight_);
    for (std::size_t i = 0; i < level.nodes->size(); ++i) {
        const Node& node = (*level.nodes)[i];
        Shown s;
        addItem(layout_.items[i], widths[i], top ? kNormalBase : kSubmenuBase, node.label, node.disabled, s);
        shown_.push_back(s);
    }
    backItem_.reset();
    back_ = {};
    if (level.back != nullptr) {
        const int w = screen_.textSize(kFont, level.back->label)[0];
        backItem_ = pie::centreItem(layout_.centreX, layout_.centreY, w, textHeight);
        // The item in the middle keeps the top ring's darker pictures at every depth.
        addItem(*backItem_, w, kNormalBase, level.back->label, false, back_);
    }
}

void PieMenuUi::highlight(std::optional<std::size_t> item, bool back) {
    if (item == highlighted_ && back == backHighlighted_) {
        return;
    }
    auto set = [&](Shown& s, bool on) {
        if (s.disabled) {
            return;
        }
        if (s.knob >= 0) {
            screen_.setVisible(s.knob, !on);
            screen_.setVisible(s.litKnob, on);
        }
        screen_.setTextColor(s.text, on ? kTextHighlighted : kTextNormal);
    };
    if (highlighted_ && *highlighted_ < shown_.size()) {
        set(shown_[*highlighted_], false);
    }
    if (item && *item < shown_.size()) {
        set(shown_[*item], true);
    }
    highlighted_ = item;
    backHighlighted_ = back;
}

std::optional<ts2::sim::PieChoice> PieMenuUi::mouse(int x, int y, Mouse what) {
    if (!isOpen()) {
        return std::nullopt;
    }
    const bool overBack = backItem_ && backItem_->rect.contains(x, y);
    const auto item = overBack ? std::nullopt : pie::pick(layout_, x, y);
    highlight(item, overBack);
    switch (what) {
    case Mouse::Move:
        break;
    case Mouse::RightDown:
        close();
        break;
    case Mouse::LeftDown:
        if (!item && !overBack) {
            close();
        } else {
            pressed_ = true;
        }
        break;
    case Mouse::LeftUp: {
        // Only the release of a press on the open menu counts, not that of the click that opened it.
        const bool pressed = pressed_;
        pressed_ = false;
        if (pressed) {
            if (overBack) {
                levels_.pop_back();
                show(levels_.back());
            } else if (!item) {
                close();
            } else if (!(*levels_.back().nodes)[*item].disabled) {
                return choose(*item);
            }
        }
        break;
    }
    }
    return std::nullopt;
}

// Picks the item: a choice closes the menu and is returned, a sub-menu opens round the item.
std::optional<ts2::sim::PieChoice> PieMenuUi::choose(std::size_t item) {
    const Node& node = (*levels_.back().nodes)[item];
    if (node.choice) {
        auto chosen = choices_[*node.choice];
        close();
        return chosen;
    }
    const auto& clicked = layout_.items[item];
    const auto size = screen_.textSize(kFont, node.label);
    levels_.push_back(Level{&node.children, clicked.rect.x + clicked.textX + size[0] / 2, clicked.rect.y + clicked.textY + size[1] / 2, &node});
    show(levels_.back());
    return std::nullopt;
}

} // namespace app
