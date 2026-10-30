#include "baby_name_dialog.hpp"

#include <algorithm>
#include <array>

namespace app {

namespace {

constexpr std::uint32_t kScriptGroup = 0xA99D8A11, kScript = 0x2D91050A;
constexpr std::uint32_t kBabyPanel = 0xCDC2CAA5, kTwinsPanel = 0xCDC2CAA6;
constexpr std::uint32_t kOk = 0xA000;
// Each baby's box is 1-4, its girl icon 0x1A-0x1D and its boy icon 0x2A-0x2D.
constexpr std::uint32_t kFirstBox = 0x01, kFirstGirlIcon = 0x1A, kFirstBoyIcon = 0x2A;
// The script's boxes hold at most 18 characters (maxtext).
constexpr std::size_t kMaxCharacters = 18;
// Every panel of the script is listed here so that only the one shown is visible.
constexpr std::array<std::uint32_t, 13> kPanels{0xCDC2CAA4, 0xCDC2CAA5, 0xCDC2CAA6, 0xCDC2CAA7, 0xCDC2CAA8, 0xCDC2CAA9, 0xCDC2CAAA,
                                                0xCDC2CAAB, 0xCDC2CAAC, 0xCDC2CAAD, 0xCDC2CAAE, 0xCDC2CAAF, 0xCDC2CAB0};

bool continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

std::size_t characters(const std::string& text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) { return !continuation(c); }));
}

// The character boundary before or after byte `at`.
std::size_t previous(const std::string& text, std::size_t at) {
    while (at > 0 && continuation(text[--at])) {
    }
    return at;
}

std::size_t next(const std::string& text, std::size_t at) {
    if (at >= text.size()) {
        return text.size();
    }
    while (++at < text.size() && continuation(text[at])) {
    }
    return at;
}

// A window's top left on the screen.
std::array<int, 2> screenCorner(const ts2::ui::Screen& screen, int window) {
    std::array<int, 2> at{0, 0};
    for (int w = window; w >= 0; w = screen.parent(w)) {
        at[0] += screen.x(w);
        at[1] += screen.y(w);
    }
    return at;
}

} // namespace

bool BabyNameDialog::open(ts2::ui::Screen& screen, int width, int height, const std::vector<Baby>& babies) {
    close(screen);
    root_ = screen.load(kScriptGroup, kScript, 0, 0);
    if (root_ < 0) {
        return false;
    }
    modalBefore_ = screen.modal();
    for (const std::uint32_t id : kPanels) {
        screen.setVisible(screen.find(root_, id), false);
    }
    const int panel = screen.find(root_, babies.size() > 1 ? kTwinsPanel : kBabyPanel);
    screen.setVisible(panel, true);
    boxes_.clear();
    for (std::size_t i = 0; i < babies.size(); ++i) {
        const auto n = static_cast<std::uint32_t>(i);
        Box box;
        if (i < 4) {
            box.window = screen.find(panel, kFirstBox + n);
            screen.setVisible(screen.find(panel, kFirstGirlIcon + n), babies[i].female);
            screen.setVisible(screen.find(panel, kFirstBoyIcon + n), !babies[i].female);
        }
        box.name = babies[i].firstName;
        box.anchor = 0;
        box.caret = box.name.size();
        screen.setText(box.window, box.name);
        boxes_.push_back(std::move(box));
    }
    screen.move(root_, (width - screen.width(panel)) / 2, (height - screen.height(panel)) / 2);
    screen.raise(root_);
    screen.setModal(root_);
    focus(screen, 0);
    ts2::platform::startTextInput();
    return true;
}

void BabyNameDialog::close(ts2::ui::Screen& screen) {
    if (root_ < 0) {
        return;
    }
    ts2::platform::stopTextInput();
    screen.remove(root_);
    screen.setModal(modalBefore_);
    root_ = -1;
    modalBefore_ = -1;
    boxes_.clear();
    dragging_ = false;
}

bool BabyNameDialog::clickedOk(const ts2::ui::Screen& screen, int window) const {
    return root_ >= 0 && window >= 0 && screen.id(window) == kOk && screen.visible(window);
}

std::vector<std::string> BabyNameDialog::names() const {
    std::vector<std::string> out;
    for (const Box& box : boxes_) {
        out.push_back(box.name);
    }
    return out;
}

void BabyNameDialog::mouseDown(ts2::ui::Screen& screen, int x, int y) {
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        const int window = boxes_[i].window;
        if (window < 0) {
            continue;
        }
        const auto [bx, by] = screenCorner(screen, window);
        if (x >= bx && y >= by && x < bx + screen.width(window) && y < by + screen.height(window)) {
            boxes_[i].caret = boxes_[i].anchor = screen.textIndexAt(window, x);
            dragging_ = true;
            focus(screen, i);
            return;
        }
    }
}

void BabyNameDialog::mouseMoved(ts2::ui::Screen& screen, int x, int) {
    if (dragging_ && focus_ < boxes_.size()) {
        boxes_[focus_].caret = screen.textIndexAt(boxes_[focus_].window, x);
        show(screen, focus_);
    }
}

void BabyNameDialog::text(ts2::ui::Screen& screen, const std::string& typed) {
    if (focus_ >= boxes_.size() || boxes_[focus_].window < 0) {
        return;
    }
    eraseSelection();
    Box& box = boxes_[focus_];
    // Characters past the box's limit are left out.
    for (std::size_t at = 0; at < typed.size() && characters(box.name) < kMaxCharacters;) {
        const std::size_t end = next(typed, at);
        box.name.insert(box.caret, typed, at, end - at);
        box.caret += end - at;
        at = end;
    }
    box.anchor = box.caret;
    show(screen, focus_);
}

void BabyNameDialog::key(ts2::ui::Screen& screen, ts2::platform::Key key) {
    using ts2::platform::Key;
    if (focus_ >= boxes_.size() || boxes_[focus_].window < 0) {
        return;
    }
    Box& box = boxes_[focus_];
    switch (key) {
    case Key::Backspace:
        if (!eraseSelection() && box.caret > 0) {
            const std::size_t from = previous(box.name, box.caret);
            box.name.erase(from, box.caret - from);
            box.caret = box.anchor = from;
        }
        break;
    case Key::Delete:
        if (!eraseSelection() && box.caret < box.name.size()) {
            box.name.erase(box.caret, next(box.name, box.caret) - box.caret);
        }
        break;
    case Key::Left: box.caret = box.anchor = previous(box.name, box.caret); break;
    case Key::Right: box.caret = box.anchor = next(box.name, box.caret); break;
    case Key::Home: box.caret = box.anchor = 0; break;
    case Key::End: box.caret = box.anchor = box.name.size(); break;
    default: return;
    }
    show(screen, focus_);
}

bool BabyNameDialog::eraseSelection() {
    Box& box = boxes_[focus_];
    const std::size_t begin = std::min(box.anchor, box.caret), end = std::max(box.anchor, box.caret);
    if (begin == end) {
        return false;
    }
    box.name.erase(begin, end - begin);
    box.caret = box.anchor = begin;
    return true;
}

void BabyNameDialog::focus(ts2::ui::Screen& screen, std::size_t box) {
    focus_ = box;
    for (std::size_t i = 0; i < boxes_.size(); ++i) {
        show(screen, i);
    }
}

void BabyNameDialog::show(ts2::ui::Screen& screen, std::size_t box) {
    const Box& b = boxes_[box];
    screen.setText(b.window, b.name);
    if (box == focus_) {
        screen.setSelection(b.window, b.anchor, b.caret);
        screen.setCaret(b.window, b.caret);
    } else {
        screen.setSelection(b.window, 0, 0);
        screen.setCaret(b.window, std::nullopt);
    }
}

} // namespace app
