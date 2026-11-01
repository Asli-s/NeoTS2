#include "ui/scroll_bar.hpp"

#include <algorithm>

namespace ts2::ui {

namespace {

// The thumb's highest top, just below the up arrow.
constexpr int kThumbStart = ScrollBar::kCell;

} // namespace

void ScrollBar::setHeight(int height) {
    height_ = std::max(height, 3 * kCell + 1);
}

void ScrollBar::setLines(int lines, int visible) {
    if (visible <= 0) {
        return;
    }
    last_ = std::max(0, lines - visible);
    lineSteps_ = last_;
    pages_ = std::max(0, lines) / visible + 1;
}

int ScrollBar::position() const {
    return last_ == 0 ? 0 : static_cast<int>(static_cast<float>(last_) * value_ + 0.5f);
}

bool ScrollBar::setPosition(int position) {
    const int before = this->position();
    if (position == before) {
        return false;
    }
    position = std::clamp(position, 0, last_);
    value_ = last_ == 0 ? 0.0f : static_cast<float>(position) / static_cast<float>(last_);
    return this->position() != before;
}

int ScrollBar::thumbTop() const {
    if (last_ == 0) {
        return kThumbStart;
    }
    const int end = height_ - 2 * kCell;
    // The game cuts the offset toward zero.
    return kThumbStart - static_cast<int>(static_cast<float>(kThumbStart - end) * value_);
}

ScrollBar::Part ScrollBar::partAt(int y) const {
    const int top = thumbTop();
    if (y < kCell) {
        return Part::Up;
    }
    if (y < top) {
        return Part::PageUp;
    }
    if (y >= top + kCell) {
        return y < height_ - kCell ? Part::PageDown : Part::Down;
    }
    return Part::Thumb;
}

bool ScrollBar::step(Part part) {
    const int line = lineSteps_ == 0 ? 0 : last_ / lineSteps_;
    const int page = pages_ == 0 ? 0 : (last_ + 1) / pages_;
    switch (part) {
    case Part::Up: return line != 0 && setPosition(position() - line);
    case Part::Down: return line != 0 && setPosition(position() + line);
    case Part::PageUp: return page != 0 && setPosition(position() - page);
    case Part::PageDown: return page != 0 && setPosition(position() + page);
    case Part::None:
    case Part::Thumb: return false;
    }
    return false;
}

bool ScrollBar::setValue(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    if (value == value_) {
        return false;
    }
    value_ = value;
    return true;
}

bool ScrollBar::dragTo(int y) {
    // The thumb's middle goes to the mouse, kept on the track.
    const int end = height_ - 2 * kCell;
    const int top = std::clamp(y - kCell / 2, kThumbStart, end);
    return setValue(static_cast<float>(top - kThumbStart) / static_cast<float>(end - kThumbStart));
}

bool ScrollBar::press(int y, std::uint64_t ms) {
    pressed_ = partAt(y);
    over_ = true;
    pressedAt_ = ms;
    return step(pressed_);
}

bool ScrollBar::move(int y, bool inside) {
    if (pressed_ == Part::Thumb) {
        return dragTo(y);
    }
    if (pressed_ != Part::None) {
        over_ = inside && partAt(y) == pressed_;
    }
    return false;
}

bool ScrollBar::release(int y) {
    const bool changed = pressed_ == Part::Thumb && dragTo(y);
    pressed_ = Part::None;
    over_ = false;
    return changed;
}

bool ScrollBar::tick(std::uint64_t ms) {
    if (pressed_ == Part::None || pressed_ == Part::Thumb || !over_ || ms - pressedAt_ <= kRepeatMs) {
        return false;
    }
    pressedAt_ = ms;
    return step(pressed_);
}

} // namespace ts2::ui
