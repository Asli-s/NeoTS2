#include "description_box.hpp"

#include <algorithm>
#include <format>

namespace app {

namespace {

// The bar's pictures, from the preview's script (containerimage, thumbimage).
constexpr std::uint32_t kImages = 0x499DB772, kTrackImage = 0x0DDD0150, kThumbImage = 0xCCC30015;
// A second press on the text within this long selects a word, wherever it is.
constexpr std::uint64_t kWordPressMs = 500;

bool continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

bool endsWord(char c) {
    return c == ' ' || c == '\n';
}

std::size_t characters(const std::string& text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) { return !continuation(c); }));
}

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

std::size_t wordStart(const std::string& text, std::size_t at) {
    while (at > 0 && !endsWord(text[at - 1])) {
        --at;
    }
    return at;
}

std::size_t wordEnd(const std::string& text, std::size_t at) {
    while (at < text.size() && !endsWord(text[at])) {
        ++at;
    }
    return at;
}

} // namespace

void DescriptionBox::attach(ts2::ui::Screen& screen, int box, std::size_t maxCharacters, bool singleLine) {
    box_ = box;
    maxCharacters_ = maxCharacters;
    singleLine_ = singleLine;
    if (box < 0 || singleLine || screen.children(box).empty()) {
        return;
    }
    // The bar is as wide as its widest picture, at the box's right edge.
    const int bar = screen.children(box).front();
    const int width = GameScrollBar::width(screen, kImages, kTrackImage, kThumbImage);
    screen.resize(bar, width, screen.height(bar));
    screen.move(bar, screen.width(box) - width, 0);
    scroll_.attach(screen, bar, kImages, kTrackImage, kThumbImage);
}

void DescriptionBox::setText(ts2::ui::Screen& screen, const std::string& text) {
    text_ = text;
    caret_ = anchor_ = text_.size();
    if (focused_) {
        focused_ = false;
        ts2::platform::stopTextInput();
    }
    selecting_ = timing_ = false;
    screen.setTopLine(box_, 0);
    show(screen);
}

void DescriptionBox::unfocus(ts2::ui::Screen& screen) {
    if (focused_) {
        focused_ = false;
        ts2::platform::stopTextInput();
    }
    show(screen);
}

void DescriptionBox::show(ts2::ui::Screen& screen) {
    if (box_ < 0) {
        return;
    }
    screen.setText(box_, text_);
    screen.setSelection(box_, focused_ ? anchor_ : 0, focused_ ? caret_ : 0);
    screen.setCaret(box_, focused_ ? std::optional<std::size_t>(caret_) : std::nullopt);
    showBar(screen);
}

void DescriptionBox::showBar(ts2::ui::Screen& screen) {
    if (singleLine_) {
        return;
    }
    // The bar's value is the top line, and with all the text fitting the box shows it from its first.
    const int lines = screen.lineCount(box_), visible = screen.visibleLines(box_);
    scroll_.set(screen, lines, visible, lines > visible ? screen.topLine(box_) : 0);
    followBar(screen);
}

void DescriptionBox::followBar(ts2::ui::Screen& screen) {
    if (!singleLine_ && screen.topLine(box_) != scroll_.value()) {
        screen.setTopLine(box_, scroll_.value());
    }
}

bool DescriptionBox::inText(const ts2::ui::Screen& screen, int x, int y) const {
    int ex = 0, ey = 0;
    for (int w = box_; w >= 0; w = screen.parent(w)) {
        if (!screen.visible(w)) {
            return false;
        }
        ex += screen.x(w);
        ey += screen.y(w);
    }
    return x >= ex && x < ex + screen.width(box_) && y >= ey && y < ey + screen.height(box_) &&
           scroll_.hit(screen, x, y) == GameScrollBar::Part::None;
}

bool DescriptionBox::mouseDown(ts2::ui::Screen& screen, int x, int y, std::uint64_t ms, bool shift) {
    if (box_ < 0 || !screen.visible(box_)) {
        return false;
    }
    if (scroll_.mouseDown(screen, x, y, ms)) {
        followBar(screen);
        return true;
    }
    if (!inText(screen, x, y)) {
        return false;
    }
    caret_ = screen.textIndexAt(box_, x, y);
    if (!shift) {
        anchor_ = caret_;
    }
    if (timing_ && ms - pressedAt_ <= kWordPressMs) {
        // A third press is a plain one again.
        timing_ = false;
        selectWord();
    } else {
        timing_ = true;
        pressedAt_ = ms;
    }
    selecting_ = true;
    if (!focused_) {
        focused_ = true;
        ts2::platform::startTextInput();
    }
    show(screen);
    return true;
}

void DescriptionBox::selectWord() {
    std::size_t at = caret_;
    if (at >= text_.size() || endsWord(text_[at])) {
        // On a space just after a word, that word. On other spaces, nothing.
        if (at == 0 || endsWord(text_[at - 1])) {
            return;
        }
        anchor_ = wordStart(text_, at);
        return;
    }
    anchor_ = wordStart(text_, at);
    caret_ = wordEnd(text_, at);
}

void DescriptionBox::mouseMove(ts2::ui::Screen& screen, int x, int y) {
    if (selecting_) {
        const std::size_t at = screen.textIndexAt(box_, x, y);
        if (at != caret_) {
            caret_ = at;
            show(screen);
        }
        return;
    }
    scroll_.mouseMove(screen, x, y);
    followBar(screen);
}

void DescriptionBox::mouseUp(ts2::ui::Screen& screen, int x, int y) {
    selecting_ = false;
    scroll_.mouseUp(screen, x, y);
}

bool DescriptionBox::wheel(ts2::ui::Screen& screen, int x, int y, int delta) {
    // The wheel works over the text only, not over the bar or while the bar holds the mouse. Each
    // full 100 of the wheel's delta (120 a notch) moves a line, and the rest is dropped.
    if (box_ < 0 || singleLine_ || holdsMouse() || !inText(screen, x, y)) {
        return false;
    }
    screen.setTopLine(box_, std::max(0, screen.topLine(box_) - delta / 100));
    showBar(screen);
    return true;
}

void DescriptionBox::tick(ts2::ui::Screen& screen, std::uint64_t ms) {
    if (scroll_.tick(screen, ms)) {
        followBar(screen);
    }
}

bool DescriptionBox::eraseSelection() {
    if (caret_ == anchor_) {
        return false;
    }
    const std::size_t begin = std::min(anchor_, caret_), end = std::max(anchor_, caret_);
    text_.erase(begin, end - begin);
    caret_ = anchor_ = begin;
    return true;
}

void DescriptionBox::type(ts2::ui::Screen& screen, const std::string& typed) {
    if (!focused_) {
        return;
    }
    eraseSelection();
    // Characters past the box's limit are left out.
    for (std::size_t at = 0; at < typed.size() && characters(text_) < maxCharacters_;) {
        const std::size_t end = next(typed, at);
        text_.insert(caret_, typed, at, end - at);
        caret_ += end - at;
        at = end;
    }
    anchor_ = caret_;
    show(screen);
}

std::size_t DescriptionBox::lineStart(const ts2::ui::Screen& screen) const {
    const auto starts = screen.lineStarts(box_);
    const auto after = std::upper_bound(starts.begin(), starts.end(), caret_);
    return after == starts.begin() ? 0 : *(after - 1);
}

std::size_t DescriptionBox::lineEnd(const ts2::ui::Screen& screen) const {
    const auto starts = screen.lineStarts(box_);
    const auto after = std::upper_bound(starts.begin(), starts.end(), caret_);
    std::size_t end = after == starts.end() ? text_.size() : *after;
    if (end > 0 && end <= text_.size() && text_[end - 1] == '\n') {
        --end;
    }
    return std::max(end, caret_);
}

void DescriptionBox::key(ts2::ui::Screen& screen, ts2::platform::Key key, bool shift, bool ctrl) {
    using ts2::platform::Key;
    if (!focused_) {
        return;
    }
    switch (key) {
    case Key::Backspace:
        if (!eraseSelection() && caret_ > 0) {
            const std::size_t from = previous(text_, caret_);
            text_.erase(from, caret_ - from);
            caret_ = anchor_ = from;
        }
        break;
    case Key::Delete:
        if (!eraseSelection() && caret_ < text_.size()) {
            text_.erase(caret_, next(text_, caret_) - caret_);
        }
        break;
    case Key::Escape: anchor_ = caret_; break;
    // A move key without Shift leaves no selection, the caret where the key took it.
    case Key::Left:
    case Key::Right:
    case Key::Home:
    case Key::End:
        if (key == Key::Left) {
            // Ctrl goes to the start of the word, or of the one before.
            if (ctrl) {
                std::size_t at = caret_;
                while (at > 0 && endsWord(text_[at - 1])) {
                    --at;
                }
                caret_ = wordStart(text_, at);
            } else {
                caret_ = previous(text_, caret_);
            }
        } else if (key == Key::Right) {
            // Ctrl goes to the start of the next word.
            if (ctrl) {
                std::size_t at = wordEnd(text_, caret_);
                while (at < text_.size() && endsWord(text_[at])) {
                    ++at;
                }
                caret_ = at;
            } else {
                caret_ = next(text_, caret_);
            }
        } else if (key == Key::Home) {
            caret_ = ctrl || singleLine_ ? 0 : lineStart(screen);
        } else {
            caret_ = ctrl || singleLine_ ? text_.size() : lineEnd(screen);
        }
        if (!shift) {
            anchor_ = caret_;
        }
        break;
    default: return;
    }
    show(screen);
}

} // namespace app
